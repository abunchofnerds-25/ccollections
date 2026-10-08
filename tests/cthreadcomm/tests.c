#include <arpa/inet.h>
#include <assert.h>
#include <cthreadcomm.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <internal/cdeadline.h>
#include <internal/cpoll.h>
#include <internal/csock.h>
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
#include <test_fds.h>
#include <test_sanitizer.h>
#include <time.h>
#include <unistd.h>

TAU_MAIN()

extern struct ccol_event_loop_s *_ccol_event_loop_resolve_for_tests(
    ccol_event_loop h);
extern size_t _ccol_event_loop_slot_table_capacity_for_tests(void);
extern bool _ccol_event_loop_resolve_pin_and_sleep_for_tests(ccol_event_loop h,
                                                             int ms);
extern void _ccol_event_loop_set_poller_batch_gen_for_tests(
    ccol_event_loop loop, uint64_t value);

/* Every write(2) call in this file writes a small signal of a few bytes into
 * a pipe. The same test already owns that pipe (a result fd or a notify fd).
 * Each write has one fixed size and is best-effort. None of them is a bulk
 * transfer that a partial write can damage. The only failure that needs a
 * retry loop is therefore EINTR, as in _eventfd_notify() of
 * src/cthreadcomm.c. This function must really consume the return value. A
 * bare (void) cast is not enough. The fortified write(2) wrapper of
 * _FORTIFY_SOURCE marks itself warn_unused_result, and a (void) cast does
 * not reliably suppress that. */
static void test_write_retry_eintr(int fd, const void *buf, size_t n) {
  ssize_t rv;
  do {
    rv = write(fd, buf, n);
  } while (rv < 0 && errno == EINTR);
}

/* The conversion of a microsecond timeout into an absolute deadline, which
 * every timed call of this module uses. The expected values are written out
 * by hand, not computed with the arithmetic under test. */
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

/* A deadline that has passed gives a poll timeout of 0, which still looks
 * once, and a deadline a fraction of a millisecond ahead gives 1, never 0.
 * A deadline at the end of time gives INT_MAX and does not overflow. */
TEST(ccol_deadline_remaining_ms_ceil, rounds_up_and_saturates) {
  struct timespec past;
  clock_gettime(CLOCK_MONOTONIC, &past);
  past.tv_sec -= 1;
  REQUIRE_EQ(ccol_deadline_remaining_ms_ceil(&past), 0);

  struct timespec soon;
  REQUIRE_TRUE(ccol_deadline_after_us(500, &soon));
  int ms = ccol_deadline_remaining_ms_ceil(&soon);
  /* 1 while the deadline is ahead, 0 once a descheduled thread got past it */
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

  ccol_circular_queue *cq =
      ccol_circular_queue_create_with_mprocs(0, NULL, &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  cq = ccol_circular_queue_create_with_mprocs(-1, NULL, &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  cq = ccol_circular_queue_create_with_mprocs(
      (size_t)INT32_MAX,
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = NULL},
      &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);
}

/* verify_circular_queue_create_inputs has a guard for
 * max_size > SIZE_MAX / sizeof(c_message_t). Without that guard, a max_size
 * of about [ccol_max_elem_count / 16, ccol_max_elem_count] is dangerous.
 * That is the top part of the documented valid range
 * "1 to ccol_max_elem_count". For such a max_size,
 * max_size * sizeof(c_message_t) wraps around size_t. The queue then
 * believes that it has room for max_size messages, but the real allocation
 * of msg_array is very small. For max_size == 2^60 it is exactly 0 bytes.
 * The first send then corrupts the heap, and AddressSanitizer reports it
 * directly. Both values that the test below rejects are well inside
 * ccol_max_elem_count. Only the overflow guard can reject them, and not the
 * max_size > ccol_max_elem_count check. The test gives NULL for
 * mmgmt_procs, which selects the default allocator. The guard must reject
 * both values before it calls malloc(3) at all. */
TEST(circular_queues,
     create_rejects_max_size_that_would_overflow_the_backing_array_size) {
  char *err_str = NULL;

  size_t smallest_overflowing = SIZE_MAX / sizeof(c_message_t) + 1;
  REQUIRE_LT(smallest_overflowing, ccol_max_elem_count);
  ccol_circular_queue *cq = ccol_circular_queue_create_with_mprocs(
      smallest_overflowing, NULL, &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  /* This is the exact value that wraps max_size * sizeof(c_message_t) to
   * exactly 0. Without this guard, the first send then corrupts the heap.
   * The value is still well inside ccol_max_elem_count. You can only build
   * it on a platform where size_t is wider than 32 bits. On a 32-bit size_t
   * (i386, for example), `(size_t)1 << 60` shifts by more than the width of
   * the type. That is undefined behavior and a compile error under
   * -Werror=shift-count-overflow. The compiler reports it even for a branch
   * that never runs. This half of the test therefore has a compile-time #if
   * guard, and the test does not skip it at run time. smallest_overflowing
   * above covers the same overflow guard on every platform and every size_t
   * width. A 32-bit build therefore loses no coverage. This second case only
   * adds one more data point, and that point needs a wider size_t. */
#if SIZE_MAX > 0xFFFFFFFFu
  size_t known_bad = (size_t)1 << 60;
  REQUIRE_LT(known_bad, ccol_max_elem_count);
  err_str = NULL;
  cq = ccol_circular_queue_create_with_mprocs(known_bad, NULL, &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);
#endif
}

/* The create call must still accept a large max_size when
 * max_size * sizeof(c_message_t) is a big allocation but is far from an
 * overflow. The overflow guard must not be too strict.
 * create_succeeds_with_large_elem_size in tests/cvector/tests.c is the
 * precedent for the same class of guard. */
TEST(circular_queues, create_succeeds_with_large_non_overflowing_max_size) {
  char *err_str = NULL;
  size_t large_max_size = 1024 * 1024; /* 16 MiB of c_message_t slots */
  ccol_circular_queue *cq =
      ccol_circular_queue_create_with_mprocs(large_max_size, NULL, &err_str);
  REQUIRE_NE((void *)cq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);
  ccol_circular_queue_destroy(cq);
}

TEST(circular_queues, create_and_destroy_no_mem_procs) {
  char *err_str = "";

  ccol_circular_queue *cq =
      ccol_circular_queue_create_with_mprocs(1, NULL, &err_str);
  REQUIRE_NE((void *)cq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  ccol_circular_queue_destroy(cq);
  REQUIRE_EQ((void *)cq, NULL);
}

TEST(circular_queues, create_and_destroy_with_mem_procs) {
  char *err_str = "";

  ccol_circular_queue *cq = ccol_circular_queue_create_with_mprocs(
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
  ccol_circular_queue *cq =
      ccol_circular_queue_create_with_mprocs(1, NULL, NULL);

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
  ccol_circular_queue *cq = ccol_circular_queue_create_with_mprocs(
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
  ccol_circular_queue *cq =
      ccol_circular_queue_create_with_mprocs(3, NULL, NULL);

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
  ccol_circular_queue *cq =
      ccol_circular_queue_create_with_mprocs(3, NULL, NULL);

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
  ccol_circular_queue *cq =
      ccol_circular_queue_create_with_mprocs(1, NULL, NULL);

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
  ccol_circular_queue *cq =
      ccol_circular_queue_create_with_mprocs(1, NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  uint64_t timeout = 100000;  // 100 msecs

  struct timespec before;
  struct timespec after;

  getWallTime(before);
  REQUIRE_EQ(ccol_circq_timed_send_zc(cq, &m1, timeout), ccol_success);
  getWallTime(after);
  REQUIRE_LT(diffTimeUSec(before, after), 10000);
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
  REQUIRE_LT(diffTimeUSec(before, after), 10000);
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
  ccol_circular_queue *cq =
      ccol_circular_queue_create_with_mprocs(1, NULL, NULL);

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

  /* A timeout of 0 is the try variant, and it reports the same refusal. */
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
  // The capacity is 2. Five sends and five receives force the head index and
  // the tail index to wrap around the ring boundary more than once.
  ccol_circular_queue *cq =
      ccol_circular_queue_create_with_mprocs(2, NULL, NULL);

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

/* A timeout of 0 does not wait: each timed call answers exactly what its
 * try variant answers, ccol_container_full or ccol_container_empty, and not
 * ccol_timed_out. This test is non-vacuous: a timed call that builds a
 * deadline of "now" for a 0 and waits on it returns ccol_timed_out. */
TEST(circular_queues, timed_calls_with_a_zero_timeout_are_the_try_variants) {
  ccol_circular_queue *cq =
      ccol_circular_queue_create_with_mprocs(1, NULL, NULL);

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
  /* Regression test. The wait loop of ccol_circq_timed_send_zc must not
   * always report ccol_unexpected_failure when ccol_cond_var_timedwait
   * returns a value that is not ETIMEDOUT. It must first check again
   * whether space became free. The racing test below tests that second
   * check. This test covers the ordinary case, where the queue is still
   * full. A forced error with no race must still give
   * ccol_unexpected_failure. The loop must not hide the error, and it must
   * not retry forever. The test sets a large timeout and a tight bound on
   * the elapsed time. This proves that the call returns quickly on the
   * forced error. */
  ccol_circular_queue *cq = ccol_circular_queue_create(1, NULL);
  c_message_t filler = {.data = malloc(1), .size = 1};
  REQUIRE_EQ(ccol_circq_try_send_zc(cq, &filler), ccol_success);

  ccol_circq_test_force_next_send_condvar_wait_error();

  c_message_t m = {.data = malloc(1), .size = 1};
  uint64_t timeout = 5000000;
  struct timespec before, after;
  getWallTime(before);
  errno = 0;
  ccol_retval_t send_rv = ccol_circq_timed_send_zc(cq, &m, timeout);
  int send_errno = errno;
  getWallTime(after);
  REQUIRE_EQ(send_rv, ccol_unexpected_failure);
  /* The forced wait error is EINVAL, and errno carries it to the caller. */
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
  /* This covers the same class of failure as _sel_wait_condvar of
   * ccol_select. See
   * ccol_select.timed_wait_ready_racing_condvar_error_still_succeeds. The
   * FAILURE branch of ccol_circq_timed_send_zc must not always report
   * ccol_unexpected_failure. A consumer on another thread can free a slot
   * in the same instant. ccol_cond_var_timedwait always locks cq->mutex
   * again before it returns, on success and on failure. This order of
   * events is therefore possible in production. The test uses the dedicated
   * test hook, which makes that order of events deterministic. A real
   * consumer thread cannot race into this exact window on its own, because
   * the hook replaces ccol_cond_var_timedwait and does not unlock
   * cq->mutex. Without that handling, this test fails with
   * ccol_unexpected_failure and the queue never sends the message. */
  ccol_circular_queue *cq = ccol_circular_queue_create(1, NULL);
  c_message_t filler = {.data = malloc(sizeof(int)), .size = sizeof(int)};
  *(int *)filler.data = 42;
  REQUIRE_EQ(ccol_circq_try_send_zc(cq, &filler), ccol_success);

  ccol_circq_test_force_next_send_condvar_wait_error_racing_ready();

  c_message_t m = {.data = malloc(1), .size = 1};
  uint64_t timeout = 5000000;
  REQUIRE_EQ(ccol_circq_timed_send_zc(cq, &m, timeout), ccol_success);
  REQUIRE_EQ(m.data, NULL);  // the queue takes ownership on success

  c_message_t raced_out = ccol_circq_test_take_race_freed_msg();
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
  /* This is the receive-side mirror of
   * timed_send_reports_unexpected_failure_on_condvar_error. A forced error
   * on an empty queue, with no race, must still give
   * ccol_unexpected_failure quickly. */
  ccol_circular_queue *cq = ccol_circular_queue_create(1, NULL);

  ccol_circq_test_force_next_recv_condvar_wait_error();

  c_message_t m = {.data = NULL, .size = 0};
  uint64_t timeout = 5000000;
  struct timespec before, after;
  getWallTime(before);
  errno = 0;
  ccol_retval_t recv_rv = ccol_circq_timed_recv_zc(cq, &m, timeout);
  int recv_errno = errno;
  getWallTime(after);
  REQUIRE_EQ(recv_rv, ccol_unexpected_failure);
  /* The forced wait error is EINVAL, and errno carries it to the caller. */
  REQUIRE_EQ(recv_errno, EINVAL);
  REQUIRE_LT(diffTimeUSec(before, after), 500000);

  ccol_circular_queue_destroy(cq);
}

TEST(circular_queues, timed_recv_condvar_error_racing_message_still_receives) {
  /* Regression test. This is the receive-side version of
   * timed_send_condvar_error_racing_freed_slot_still_sends. A producer on
   * another thread can complete a send in the same instant that the test
   * forces an unrelated ccol_cond_var_timedwait error. The receive must
   * still return that message. It must not discard the message and report
   * ccol_unexpected_failure. Without that handling, this test fails with
   * ccol_unexpected_failure. The sentinel message that the hook put into the
   * queue is then lost in the queue forever. msg_count stays at 1, and
   * nothing can receive the message, because the caller already stopped. */
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  ccol_circq_test_force_next_recv_condvar_wait_error_racing_ready();

  c_message_t m = {.data = (void *)1, .size = 1};  // overwritten on success
  uint64_t timeout = 5000000;
  REQUIRE_EQ(ccol_circq_timed_recv_zc(cq, &m, timeout), ccol_success);
  REQUIRE_EQ(m.data, NULL);  // the {NULL, 0} sentinel from the hook
  REQUIRE_EQ(m.size, (size_t)0);
  REQUIRE_EQ(ccol_circq_msg_count(cq), (size_t)0);

  ccol_circular_queue_destroy(cq);
}

/* ------------------------------------------------------------------ */
/* The operation that makes progress possible must release a blocked waiter.
 * The waiter must not wait for its own timeout.
 *
 * Both queues signal a condition variable only when the matching waiter
 * count says that somebody is parked in it. A wait site that does not
 * register itself therefore leaves a real waiter invisible, and the queue
 * skips the signal. The symptom is not a wrong answer. It is a delay: the
 * waiter sleeps until its own timeout, checks again, and returns the message
 * that was already there. These tests separate the two cases in two ways.
 * First, the timeout of the wait is several times longer than the bound that
 * the test then allows for the wake. Second, the test polls the waiter
 * count, and it runs the wake-up operation only after the waiter really
 * parks. A missing registration therefore fails the poll, and not only the
 * timing bound.
 *
 * These tests are not vacuous: remove either the increment or the decrement
 * around any of the three timed wait sites, and the matching test fails.
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

/* Bounded poll helpers. Every wait here has a deadline. A regression
 * therefore fails the run and does not hang it. */
static void _wake_poll_pause(void) {
  struct timespec s = {.tv_sec = 0, .tv_nsec = 200000}; /* 0.2 ms */
  nanosleep(&s, NULL);
}

static bool _wake_await_circq_readers(ccol_circular_queue *cq) {
  struct timespec start, now;
  getWallTime(start);
  for (;;) {
    if (ccol_circq_waiting_readers_for_tests(cq) >= 1) return true;
    getWallTime(now);
    if (diffTimeUSec(start, now) > CCOL_WAKE_PARK_BOUND_US) return false;
    _wake_poll_pause();
  }
}

static bool _wake_await_circq_writers(ccol_circular_queue *cq) {
  struct timespec start, now;
  getWallTime(start);
  for (;;) {
    if (ccol_circq_waiting_writers_for_tests(cq) >= 1) return true;
    getWallTime(now);
    if (diffTimeUSec(start, now) > CCOL_WAKE_PARK_BOUND_US) return false;
    _wake_poll_pause();
  }
}

static bool _wake_await_dynmq_readers(ccol_dynamic_queue *dq) {
  struct timespec start, now;
  getWallTime(start);
  for (;;) {
    if (ccol_dynmq_waiting_readers_for_tests(dq) >= 1) return true;
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

  /* This drains the queue before the destroy below. A receiver that comes
     back empty is the regression that this test looks for, and it leaves the
     message in the queue. A destroy of a queue that still holds messages is
     a caller error, and the library aborts on it. That abort would stop the
     whole binary instead of a failure of the assertions below. */
  c_message_t leftover = {.data = NULL, .size = 0};
  while (ccol_circq_try_recv_zc(cq, &leftover) == ccol_success) {
    free(leftover.data);
  }

  /* Every waiter returned, so the queue must give the counter back. */
  size_t residual = ccol_circq_waiting_readers_for_tests(cq);
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
    /* The message in the queue is the one from the released sender. */
    c_message_t tail = {.data = NULL, .size = 0};
    drained = (ccol_circq_try_recv_zc(cq, &tail) == ccol_success);
    matched = (tail.data == payload);
    free(tail.data);
  } else {
    free(payload);
  }

  /* This drains the queue before the destroy below, for the same reason as
     blocked_timed_recv_is_released_by_a_send. On the path where no thread
     starts, the filler is still in the queue. On the path that this test
     looks for, a sender that times out leaves its own message behind. A
     destroy of a queue that still holds messages is a caller error, and the
     library aborts on it. That abort would stop the whole binary instead of
     a failure of the assertions below. */
  c_message_t leftover = {.data = NULL, .size = 0};
  while (ccol_circq_try_recv_zc(cq, &leftover) == ccol_success) {
    free(leftover.data);
  }

  size_t residual = ccol_circq_waiting_writers_for_tests(cq);
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

  /* blocked_timed_recv_is_released_by_a_send says why this drains first. */
  c_message_t leftover = {.data = NULL, .size = 0};
  while (ccol_dynmq_try_recv_zc(dq, &leftover) == ccol_success) {
    free(leftover.data);
  }

  size_t residual = ccol_dynmq_waiting_readers_for_tests(dq);
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
  // A receive must still get the messages that are already in the queue
  // after the test turns sending off. After the queue becomes empty, a timed
  // recv must time out. It must not return ccol_not_permitted, because the
  // off state affects senders only.
  ccol_circular_queue *cq =
      ccol_circular_queue_create_with_mprocs(2, NULL, NULL);

  c_message_t m = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_send_zc(cq, &m), ccol_success);
  REQUIRE_EQ(ccol_circq_send_zc(cq, &m), ccol_success);

  ccol_circq_disable_sending(cq);

  // Existing messages must still drain successfully.
  REQUIRE_EQ(ccol_circq_recv_zc(cq, &m), ccol_success);
  REQUIRE_EQ(ccol_circq_recv_zc(cq, &m), ccol_success);

  // The queue is now empty and sending is off. A timed recv must time out.
  // It must not return ccol_not_permitted.
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
  // ccol_circq_disable_sending must NOT wake a thread that blocks in
  // ccol_circq_recv_zc. That thread must stay blocked. It returns only after
  // the test turns sending on again and a message arrives.
  ccol_circular_queue *cq =
      ccol_circular_queue_create_with_mprocs(1, NULL, NULL);

  cq_disable_recv_args args = {.cq = cq, .result = ccol_unexpected_failure};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, cq_blocking_recv_thread, &args), 0);

  usleep(20000);  // let the receiver block on the empty queue
  ccol_circq_disable_sending(cq);
  usleep(20000);  // receiver must still be blocked at this point

  // Turn sending on again and send a message, which releases the receiver.
  ccol_circq_enable_sending(cq);
  c_message_t m = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_send_zc(cq, &m), ccol_success);
  pthread_join(tid, NULL);

  REQUIRE_EQ(args.result, ccol_success);

  ccol_circular_queue_destroy(cq);
}

TEST(circular_queues, timed_recv_times_out_when_disabled) {
  // ccol_circq_timed_recv_zc must return ccol_timed_out when the queue is
  // empty and sending is off. It must not return ccol_not_permitted. The off
  // state must not make the call return before the deadline.
  ccol_circular_queue *cq =
      ccol_circular_queue_create_with_mprocs(1, NULL, NULL);

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
  // This makes the sender block on the second message.
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
  ccol_circular_queue *cq =
      ccol_circular_queue_create_with_mprocs(1, NULL, NULL);

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

/* Reads /proc/<pid>/<name>, a short pseudo-file, into buf. The result is
 * NUL-terminated. Returns true on success. This is a best-effort diagnostic
 * only. It returns false on any failure, for example a process that the
 * kernel already reaped, or a permission problem. The caller prints that
 * false result as part of the dump and does not treat it as fatal. This
 * function runs only after a bounded wait times out. It must therefore never
 * add a new way to hang or to crash the test binary. */
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

/* This is a diagnostic only. It prints what the /proc of the host kernel
 * still says about pid, and about each thread of pid. It runs after a
 * bounded wait for pid times out. The doc comment of
 * _wait_for_forked_child_bounded says why every parent-side waitpid() in
 * this file has a bound. This function reads real files with plain, buffered
 * stdio and read() calls. It uses nothing that is safe inside a signal
 * handler, and that is deliberate. It runs only on the path that already
 * fails, long after the timing window that mattered closes. More
 * disturbance therefore costs nothing.
 *
 * The State: line of /proc/<pid>/status is the most useful field here. Z
 * (zombie) means that the child really exited and that the kernel only waits
 * for a reap. That points at a bug in how waitpid() sees this under
 * emulation, and not at anything that the child did. D, R or S with a real
 * /proc/<pid>/syscall entry means that the child is still alive and stuck at
 * a place that you can name. One thread of the child can be stuck in the
 * same way. The function reads status, wchan, syscall and stat one by one.
 * One file that is missing or unreadable therefore does not hide the others.
 * For example, wchan needs a kernel config option that the running kernel
 * can lack. */
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

/* This is a bounded replacement for a blocking waitpid(pid, status_out, 0).
 * It polls with WNOHANG for up to timeout_ms of wall-clock time. It returns
 * true and sets *status_out the moment the kernel reaps pid. It returns
 * false if timeout_ms passes first. A qemu-user hang can make the alarm()
 * bound of a forked child useless. The child then completes, and it can even
 * abort visibly with SIGABRT and a core dump, but the waitpid() of the
 * parent still never returns. This is why the parent side has a bound here.
 * _dump_stuck_child_diagnostics, the sibling of this function, then prints
 * the diagnosis from the parent and not from inside the child. A child-side
 * wait with no bound still has its own alarm(). A parent-side wait with no
 * bound has nothing to fall back on. It can therefore hang this whole test
 * binary, and the CI job that runs it, forever. This is true however well
 * bounded every forked child in this file already is. On a timeout this
 * function prints the diagnostics. It then tries to SIGKILL pid and to reap
 * it. That attempt is best-effort. Without it, a child that is only slow
 * stays alive as an orphan after this test ends. That reap attempt also has
 * a bound. Its outcome does not change the false return of this function,
 * because the timeout already establishes the failure that this test must
 * report. */
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
 * "#if CCOL_FORK_SAFETY_REQUIRED" block further down in this file. A build
 * with CCOL_FORK_SAFETY_REQUIRED set to 0 compiles that block out, so the
 * helper would have no caller left. This file builds with -Werror, and
 * -Wunused-function then turns that into a hard build failure for a
 * configuration that the library documents as supported. The guard keeps the
 * helper and its callers on the same switch. It is not a suppression: a
 * helper that loses its LAST caller in the default build is still an error.
 */
#if CCOL_FORK_SAFETY_REQUIRED
/* This is a bounded alternative to a plain blocking read() on the result
 * pipe of a forked child. Every fork_safety test below that uses a result
 * pipe depends on the alarm() of the forked child to close the write end of
 * that pipe. The child closes it in one of two ways. It writes the result
 * byte and exits normally, or the default SIGALRM disposition stops it on a
 * hang. That alarm is the only bound that a bare read() there would have. A
 * qemu-user binary-translation lock that stays locked across the guest
 * fork() can wedge the itimer and the signal delivery of the forked child.
 * It wedges everything else that the child depends on too. That turns the
 * alarm bound off. The write end of the pipe then stays open forever, with
 * nothing written and nothing closed. A bound on the read itself with poll()
 * closes that gap. This bound does not depend on the alarm() of the child.
 * The reasoning is the same as for _wait_for_forked_child_bounded above:
 * this file never trusts a parent-side wait with no bound. The function
 * returns the same value as read() inside timeout_ms. It returns 1 for a
 * byte that it really reads, and 0 for a real EOF. It also returns 0, as if
 * it saw EOF, on a timeout. Every REQUIRE_EQ((int)n, 1) call site below
 * therefore fails that one test cleanly and does not hang the whole
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

/* A destroy of a queue that still has a linked ccol_select() or
 * ccol_event_loop waiter is an error. The node of that waiter then holds a
 * dangling pointer into the mutex of the queue, and the destroy frees that
 * mutex. The msg_count > 0 check cannot catch this, because it sees only
 * the messages that stay in the queue. Both __ccol_circular_queue_destroy
 * and __ccol_dynamic_queue_destroy therefore assert when
 * sel_read_waiters_head or sel_write_waiters_head is still not NULL. This
 * test runs in a forked child, because the ccol_assert() and the abort()
 * that follow it stop the whole process. */
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
    /* This bounds the lifetime of this child. An unexpected hang here then
     * fails this one test loudly and fast. The default SIGALRM disposition
     * stops the process, and the bounded wait of the parent below sees
     * WIFSIGNALED with SIGALRM instead of SIGABRT. This bound does not
     * depend on the bound of the parent below. */
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
    /* This is the misuse under test. Nothing removes the registration
     * above, and nothing destroys the loop, before this call. */
    ccol_circular_queue_destroy(cq);
    _exit(0); /* not reachable when the assert fires, as it must */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  /* 20s is much longer than the alarm(10) of the child. This leaves margin
   * for the parent to see the child and to reap it, even under emulation.
   * The doc comment of _wait_for_forked_child_bounded says why the parent
   * needs a bound of its own, and not only the child. */
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

typedef struct cq_select_waiter_args {
  ccol_circular_queue *cq;
} cq_select_waiter_args;

static void *cq_select_waiter_thread(void *arg) {
  cq_select_waiter_args *a = (cq_select_waiter_args *)arg;
  size_t idx;
  /* This timeout is long enough that the destroy call on the main thread,
   * which races this call, always ends the process first. This call itself
   * never needs to return. */
  ccol_select_timed_va(&idx, 5000000,
                       ccol_selectable_from_circq(a->cq, ccol_select_read));
  return NULL;
}

TEST(circular_queues, destroy_while_ccol_select_is_watching_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    /* This bounds the lifetime of this child. An unexpected hang here then
     * fails this one test loudly and fast. Without the bound, that hang
     * stops the whole test binary, and the CI job that runs it, forever.
     * The same alarm in the sibling test above carries the full reason. The
     * doc comment of _wait_for_forked_child_bounded says why the parent
     * below has its own independent bound. */
    alarm(10);
    ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);
    if (!cq) _exit(2);

    cq_select_waiter_args wargs = {.cq = cq};
    pthread_t waiter;
    /* An if-guard checks this, and not REQUIRE_EQ. This code runs inside
     * the forked child above (pid == 0). The failure path of REQUIRE_EQ is
     * an early `return` out of this Tau test function. That return would
     * leave THIS test function while the process is still the forked child.
     * It would skip the exit calls below and fall into the test-running
     * loop of the harness a second time. This process must run only this
     * one child-side branch. The same guard and comment on
     * fork_does_not_deadlock_with_queue_registered_before_event_loop, lower
     * down in this file, carry the full reasoning. The poll loop below
     * already bounds a failed create here, because linked never becomes
     * true when no thread links it. But an immediate exit with a distinct
     * code shows the real cause in the exit status. Otherwise the cause
     * appears 2 seconds later as a REQUIRE_TRUE(linked) failure that you
     * cannot tell apart from any other. */
    if (pthread_create(&waiter, NULL, cq_select_waiter_thread, &wargs) != 0)
      _exit(4);

    /* This polls until the waiter thread REALLY links itself into the
     * read-waiter list of cq. This is real synchronization on the condition
     * that this test needs, and not a fixed sleep that guesses at it. A
     * return from pthread_create() does not promise that the scheduler ran
     * the new thread at all. It also does not promise that the thread
     * reached its first ccol_mutex_lock and link step inside Phase 1 of
     * ccol_select_timed. A fixed-sleep hand-off here loses this race under
     * qemu-user emulation, where the latency to start a thread is much
     * higher and much more variable. The destroy() then runs first. It
     * correctly sees no linked waiters, has nothing to catch, and frees cq.
     * The late waiter thread then dereferences that freed memory as soon as
     * the scheduler runs it. That is a real use-after-free. Its undefined
     * behaviour shows up as a child that hangs forever, and a waitpid below
     * with no bound could never see that. The bound here is 200 iterations
     * of 10ms, which is 2s. That is well above any realistic scheduling
     * delay, and it is still a hard bound. If the condition never becomes
     * true, the REQUIRE_TRUE below fails this test cleanly instead of going
     * on into the same race. */
    bool linked = false;
    for (int i = 0; i < 200 && !linked; i++) {
      linked = ccol_circq_test_has_sel_read_waiter_for_tests(cq);
      if (!linked) {
        struct timespec ts = {0, 10000000}; /* 10ms */
        nanosleep(&ts, NULL);
      }
    }
    /* not reachable in practice. See the REQUIRE_TRUE below. */
    if (!linked) _exit(3);

    /* This is the misuse under test. The waiter thread above is confirmed
     * to be still linked into the waiter list of cq when this destroys
     * cq. */
    ccol_circular_queue_destroy(cq);
    _exit(0); /* not reachable when the assert fires, as it must */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  /* The doc comment of _wait_for_forked_child_bounded and the sibling test
   * above say why the bound is 20s and why it exists at all. */
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// DYNAMIC_QUEUE TESTS

TEST(dynamic_queues, create_fails) {
  char *err_str = "";

  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_with_mprocs(
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

  ccol_dynamic_queue *dq =
      ccol_dynamic_queue_create_with_mprocs(NULL, &err_str);
  REQUIRE_NE((void *)dq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  ccol_dynamic_queue_destroy(dq);
  REQUIRE_EQ((void *)dq, NULL);
}

TEST(dynamic_queues, create_and_destroy_with_mprocs) {
  char *err_str = "";

  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_with_mprocs(
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      &err_str);
  REQUIRE_NE((void *)dq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  ccol_dynamic_queue_destroy(dq);
  REQUIRE_EQ((void *)dq, NULL);
}

TEST(dynamic_queues, basic_send_and_receive_no_mprocs) {
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_with_mprocs(NULL, NULL);

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

TEST(dynamic_queues, basic_send_and_receive_with_mprocs) {
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_with_mprocs(
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
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_with_mprocs(NULL, NULL);

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
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_with_mprocs(NULL, NULL);

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
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_with_mprocs(NULL, NULL);

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
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_with_mprocs(NULL, NULL);

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
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_with_mprocs(NULL, NULL);

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
  REQUIRE_LT(diffTimeUSec(before, after), 10000);
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
  /* This mirrors circular_queues.timed_recv_reports_unexpected_failure_on_
   * condvar_error. A forced error on an empty queue, with no race, must
   * still give ccol_unexpected_failure quickly. */
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create(NULL);

  ccol_dynmq_test_force_next_recv_condvar_wait_error();

  c_message_t m = {.data = NULL, .size = 0};
  uint64_t timeout = 5000000;
  struct timespec before, after;
  getWallTime(before);
  errno = 0;
  ccol_retval_t recv_rv = ccol_dynmq_timed_recv_zc(dq, &m, timeout);
  int recv_errno = errno;
  getWallTime(after);
  REQUIRE_EQ(recv_rv, ccol_unexpected_failure);
  /* The forced wait error is EINVAL, and errno carries it to the caller. */
  REQUIRE_EQ(recv_errno, EINVAL);
  REQUIRE_LT(diffTimeUSec(before, after), 500000);

  ccol_dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, timed_recv_condvar_error_racing_message_still_receives) {
  /* This covers the same class of failure as _sel_wait_condvar of
   * ccol_select. The FAILURE branch of ccol_dynmq_timed_recv_zc must not
   * always report ccol_unexpected_failure. A producer on another thread can
   * put a message into the queue in the same instant.
   * ccol_cond_var_timedwait always locks dq->mutex again before it returns,
   * on success and on failure. This order of events is therefore possible
   * in production. The test uses the dedicated test hook, which makes that
   * order of events deterministic. A real producer thread cannot race into
   * this exact window on its own. Without that handling, this test fails
   * with ccol_unexpected_failure. The sentinel message that the hook put
   * into the queue is then lost in the queue forever. */
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create(NULL);

  ccol_dynmq_test_force_next_recv_condvar_wait_error_racing_ready();

  c_message_t m = {.data = (void *)1, .size = 1};  // overwritten on success
  uint64_t timeout = 5000000;
  REQUIRE_EQ(ccol_dynmq_timed_recv_zc(dq, &m, timeout), ccol_success);
  REQUIRE_EQ(m.data, NULL);  // the {NULL, 0} sentinel from the hook
  REQUIRE_EQ(m.size, (size_t)0);
  REQUIRE_EQ(ccol_dynmq_msg_count(dq), (size_t)0);

  ccol_dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, enable_disable_sending) {
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_with_mprocs(NULL, NULL);

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

/* A timeout of 0 is ccol_dynmq_try_recv_zc: an empty queue answers
 * ccol_container_empty at once, and not ccol_timed_out. */
TEST(dynamic_queues, timed_recv_with_a_zero_timeout_is_the_try_variant) {
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_with_mprocs(NULL, NULL);

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
  // A receive must still get the messages that are already in the queue
  // after the test turns sending off. After the queue becomes empty, a timed
  // recv must time out. It must not return ccol_not_permitted, because the
  // off state affects senders only.
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_with_mprocs(NULL, NULL);

  c_message_t m = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_dynmq_send_zc(dq, &m), ccol_success);
  REQUIRE_EQ(ccol_dynmq_send_zc(dq, &m), ccol_success);

  ccol_dynmq_disable_sending(dq);

  // Existing messages must still drain successfully.
  REQUIRE_EQ(ccol_dynmq_recv_zc(dq, &m), ccol_success);
  REQUIRE_EQ(ccol_dynmq_recv_zc(dq, &m), ccol_success);

  // The queue is now empty and sending is off. A timed recv must time out.
  // It must not return ccol_not_permitted.
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
  // ccol_dynmq_disable_sending must NOT wake a thread that blocks in
  // ccol_dynmq_recv_zc. That thread must stay blocked. It returns only after
  // the test turns sending on again and a message arrives.
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_with_mprocs(NULL, NULL);

  dq_disable_recv_args args = {.dq = dq, .result = ccol_unexpected_failure};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, dq_blocking_recv_thread, &args), 0);

  usleep(20000);  // let the receiver block on the empty queue
  ccol_dynmq_disable_sending(dq);
  usleep(20000);  // receiver must still be blocked at this point

  // Turn sending on again and send a message, which releases the receiver.
  ccol_dynmq_enable_sending(dq);
  c_message_t m = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_dynmq_send_zc(dq, &m), ccol_success);
  pthread_join(tid, NULL);

  REQUIRE_EQ(args.result, ccol_success);

  ccol_dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, timed_recv_times_out_when_disabled) {
  // ccol_dynmq_timed_recv_zc must return ccol_timed_out when the queue is
  // empty and sending is off. It must not return ccol_not_permitted. The off
  // state must not make the call return before the deadline.
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_with_mprocs(NULL, NULL);

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
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create_with_mprocs(NULL, NULL);

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

/* This is the ccol_dynamic_queue version of
 * circular_queues.destroy_with_live_event_loop_registration_is_fatal above.
 * See the comment of that test. */
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
    /* This is the misuse under test. Nothing removes the registration
     * above, and nothing destroys the loop, before this call. */
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

  ccol_channel *ch = ccol_channel_create_with_mprocs(0, NULL, &err_str);
  REQUIRE_EQ((void *)ch, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  ch = ccol_channel_create_with_mprocs(-1, NULL, &err_str);
  REQUIRE_EQ((void *)ch, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  ch = ccol_channel_create_with_mprocs(
      (size_t)INT32_MAX,
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = NULL, .malloc = malloc, .realloc = realloc},
      &err_str);
  REQUIRE_EQ((void *)ch, NULL);
  REQUIRE_NE((void *)err_str, NULL);
}

TEST(channels, create_and_destroy_no_mprocs) {
  char *err_str = "";

  ccol_channel *ch = ccol_channel_create_with_mprocs(1, NULL, &err_str);
  REQUIRE_NE((void *)ch, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  ccol_channel_destroy(ch);
  REQUIRE_EQ((void *)ch, NULL);
}

TEST(channels, create_and_destroy_with_mprocs) {
  char *err_str = "";

  ccol_channel *ch = ccol_channel_create_with_mprocs(
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
  ccol_channel *ch = ccol_channel_create_with_mprocs(1, NULL, NULL);

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

TEST(channels, basic_send_and_receive_with_mprocs) {
  ccol_channel *ch = ccol_channel_create_with_mprocs(
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
  ccol_channel *ch = ccol_channel_create_with_mprocs(3, NULL, NULL);

  c_message_t m1 = {.data = NULL, .size = 0};

  for (size_t i = 0; i < 3; ++i) {
    REQUIRE_EQ(ccol_chan_msg_count(ch, ccol_owner_to_workers), i);
    ccol_chan_send_zc(ch, &m1);
    REQUIRE_EQ(ccol_chan_msg_count(ch, ccol_owner_to_workers), i + 1);
  }

  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_for_channels_msg_count, ch), 0);

  usleep(100000);

  for (size_t i = 3; i > 0; --i) {
    REQUIRE_EQ(ccol_chan_msg_count(ch, ccol_workers_to_owner), i);
    ccol_chan_recv_zc(ch, &m1);
    REQUIRE_EQ(ccol_chan_msg_count(ch, ccol_workers_to_owner), i - 1);
  }

  pthread_join(tid, NULL);
  ccol_channel_destroy(ch);
}

/* The two channel tests below take turns through phase, and never through
 * a fixed sleep: a sleep of a busy runner can end long after its time, and
 * the two threads then lose their order. The worker records the line of its
 * first failed check in failed_line, and the main thread asserts it after
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

/* Waits until phase reaches want, for at most 10 s. It gives false on a
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
  chan_turns_t t = {.ch = ccol_channel_create_with_mprocs(1, NULL, NULL)};
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

/* A call that must time out gets CHAN_SHORT_US and must take at least that
 * long. A call that must succeed at once gets CHAN_LONG_US and must return
 * well before it: an implementation that waits out its timeout before it
 * takes a message that is already there fails, and a slow runner does not
 * fail a correct one. */
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
  chan_turns_t t = {.ch = ccol_channel_create_with_mprocs(1, NULL, NULL)};
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
  ccol_channel *ch = ccol_channel_create_with_mprocs(1, NULL, NULL);

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

/* This is the ccol_channel version of
 * circular_queues.destroy_with_live_event_loop_registration_is_fatal above.
 * __ccol_channel_destroy destroys both circular queues below the channel.
 * It therefore gets the same assert from whichever direction still has a
 * live ccol_event_loop registration that watches it. */
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
    /* This thread is the owner of the ccol_channel. ccol_select_read here
     * therefore resolves to workers_to_owner_cq. The doc comment of
     * ccol_selectable_from_chan gives the details. */
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_chan(ch, ccol_select_read), handlers, NULL,
        &err);
    if (reg == CCOL_EVENT_REG_INVALID) _exit(2);
    /* This is the misuse under test. Nothing removes the registration
     * above, and nothing destroys the loop, before this call. */
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

/* This turns sending off on both queues and waits for another delay. It then
 * turns sending on again for cq0 and sends one message to it. It checks that
 * ccol_select stays blocked through the off phase. ccol_select must wake
 * only on the message that comes after that phase. */
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
   * ccol_select_timed() holds one waiter node for each selectable in an
   * array. It allocates that array with a single plain
   * malloc(n * sizeof(internal waiter node)) call. That call has no
   * calloc-style overflow check of its own. The waiter node struct has
   * several pointer-sized fields. n == SIZE_MAX is therefore always larger
   * than SIZE_MAX / sizeof(that struct). The call must reject this with
   * ccol_invalid_args. The rejection must also happen BEFORE the code forms
   * the multiplication that overflows. It must happen before the code reads
   * selectables[i] for any i > 0. The test passes an array of exactly one
   * element on purpose, while n claims many more elements than the array
   * holds. A regression that checks n only after it starts to scan the
   * array therefore reads out of bounds here. It does not only get the size
   * of an allocation wrong. */
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
   * _sel_phase1_scan_register narrows the index of a matched selectable
   * into a plain `int`. That index is a size_t in the range 0..n-1. The
   * `int` is its `found` local, which also carries the sentinels -1 for
   * "nothing found yet" and -2 for "system error". ccol_select_timed then
   * widens it back with `(size_t)found`. With an n large enough for a match
   * beyond INT_MAX, that narrowing gives a negative or a wrapped value, and
   * it then corrupts *ready_index. This n value is far below the
   * SIZE_MAX / sizeof(internal waiter node) guard that the test above
   * covers. INT_MAX is about 2^31, and the ceiling of that other guard is
   * several orders of magnitude higher on any 64-bit platform. This test
   * therefore covers the INT_MAX guard alone, and not the earlier one. The
   * rejection must also happen BEFORE the code reads selectables[i] for any
   * i > 0. The test passes an array of exactly one element on purpose,
   * while n claims many more elements than the array holds. A regression
   * that checks n only after it starts to scan the array therefore reads
   * out of bounds here. It does not only get the size of an allocation
   * wrong. */
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
  // An off state for sending must not affect waiters in the read direction.
  // ccol_select_timed must wait out the full timeout. It must not return
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
  // ccol_select must stay blocked when sending is off on all queues. After
  // the test turns sending on again and a message arrives, ccol_select must
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
  ccol_channel *ch = ccol_channel_create_with_mprocs(4, NULL, NULL);

  /* The worker thread sends with ccol_chan_send_zc, which routes to
   * workers_to_owner_cq. The owner thread calls ccol_selectable_from_chan()
   * here, and that call also resolves to workers_to_owner_cq. ccol_select
   * therefore watches the correct queue.
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
} sel_enable_circq_args;

static void *thr_enable_circq_sending(void *arg) {
  sel_enable_circq_args *a = (sel_enable_circq_args *)arg;
  usleep((useconds_t)a->delay_us);
  ccol_circq_enable_sending(a->cq);
  return NULL;
}

typedef struct {
  ccol_dynamic_queue *dq;
  int delay_us;
} sel_enable_dynq_args;

static void *thr_enable_dynq_sending(void *arg) {
  sel_enable_dynq_args *a = (sel_enable_dynq_args *)arg;
  usleep((useconds_t)a->delay_us);
  ccol_dynmq_enable_sending(a->dq);
  return NULL;
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

  /* Drain the message that is left, so the assert in destroy passes */
  c_message_t drain = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_recv_zc(cq, &drain), ccol_success);
  free(drain.data);

  ccol_circular_queue_destroy(cq);
}

TEST(ccol_select, write_circq_wakes_when_sending_reenabled) {
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);
  ccol_circq_disable_sending(cq);

  sel_enable_circq_args args = {.cq = cq, .delay_us = 15000};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_enable_circq_sending, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, ccol_selectable_from_circq(cq, ccol_select_write)),
      ccol_success);
  REQUIRE_EQ(idx, 0);

  pthread_join(tid, NULL);
  ccol_circular_queue_destroy(cq);
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

TEST(ccol_select, write_dynq_wakes_when_sending_reenabled) {
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create(NULL);
  ccol_dynmq_disable_sending(dq);

  sel_enable_dynq_args args = {.dq = dq, .delay_us = 15000};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_enable_dynq_sending, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, ccol_selectable_from_dynq(dq, ccol_select_write)),
      ccol_success);
  REQUIRE_EQ(idx, 0);

  pthread_join(tid, NULL);
  ccol_dynamic_queue_destroy(dq);
}

TEST(ccol_select, write_mixed_full_circq_and_readable_circq) {
  /* q_write is full. q_read is empty and gets a message. ccol_select must
   * pick q_read, in the read direction, first. */
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

  /* Drain the fill message of q_write */
  c_message_t drain = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_recv_zc(q_write, &drain), ccol_success);
  free(drain.data);

  ccol_circular_queue_destroy(q_write);
  ccol_circular_queue_destroy(q_read);
}

TEST(ccol_select, write_channel_owner_resolves_send_direction) {
  ccol_channel *ch = ccol_channel_create_with_mprocs(4, NULL, NULL);

  /* The owner calls ccol_selectable_from_chan with ccol_select_write, which
   * resolves to owner_to_workers_cq. That queue is empty and writable, so
   * ccol_select must return immediately. */
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
  /* This writes data to the pipe before the call to ccol_select. The call
   * must return at once and must report the correct index. The caller then
   * reads the fd itself. */
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

TEST(ccol_select, fd_blocks_until_data_arrives) {
  /* The thread writes to the pipe after a delay. ccol_select must block and
   * then wake when the data arrives. The caller then reads the fd itself. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  sel_fd_write_args args = {.write_fd = pfd[1], .delay_us = 15000, .value = 77};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_write_to_fd, &args), 0);

  size_t idx = 99;
  struct timespec before, after;
  getWallTime(before);
  REQUIRE_EQ(
      ccol_select_va(&idx, ccol_selectable_from_fd(pfd[0], ccol_select_read)),
      ccol_success);
  getWallTime(after);
  REQUIRE_EQ(idx, 0);
  REQUIRE_GE(diffTimeUSec(before, after), 10000); /* the call really blocked */

  int got = 0;
  REQUIRE_EQ((ssize_t)sizeof(got), read(pfd[0], &got, sizeof(got)));
  REQUIRE_EQ(got, 77);

  pthread_join(tid, NULL);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_select, fd_writable_immediately) {
  /* A new pipe write end is always writable. The call must return at once. */
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
  /* The pipe already has data, and the circular queue is empty. The fd must
   * win at index 0. */
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
  /* The pipe has no data. A thread sends to the queue after a delay. The
   * queue must win. This shows that the eventfd bridge correctly wakes
   * epoll_wait for a queue event in a selectable array that mixes fds and
   * queues. */
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
  /* This is the same as the test above, but with a ccol_dynamic_queue. It
   * checks that the eventfd bridge works for dynq selectables in epoll
   * mode. */
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
  /* The queue is empty and sending is on. ccol_select_timed must return
   * ccol_timed_out after the deadline. It must not block forever. */
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
  /* This is the read end of a pipe with no data in it. The call must time
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
  /* A timeout of 0 makes a poll that does not block. The queue is empty, so
   * the call gives timed_out at once. */
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
  /* The producer sends before the 500 ms deadline. select must return
   * success. */
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  pthread_t tid;
  c_message_t send_msg = {.data = malloc(4), .size = 4};
  *(int *)send_msg.data = 1234;

  /* A helper thread that sleeps for 20 ms and then sends. */
  helper_thread_args args = {cq, send_msg};

  REQUIRE_EQ(pthread_create(&tid, NULL, helper_thread_main, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_timed_va(&idx, 500000,
                           ccol_selectable_from_circq(cq, ccol_select_read)),
      ccol_success);
  REQUIRE_EQ(idx, (size_t)0);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_try_recv_zc(cq, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 1234);
  free(recv_msg.data);

  pthread_join(tid, NULL);
  ccol_circular_queue_destroy(cq);
}

TEST(ccol_select, timed_out_deregisters_waiter_node) {
  /* After ccol_select_timed times out, it must remove the waiter node from
   * the waiter list of the queue before it frees the node. Without that
   * removal, a later send dereferences freed memory. valgrind and
   * AddressSanitizer report that. A clean run here confirms that the removal
   * happens on the timeout path. */
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_timed_va(&idx, 30000 /* us */,
                           ccol_selectable_from_circq(cq, ccol_select_read)),
      ccol_timed_out);

  /* This sends to the queue AFTER the select that timed out returns. If the
   * select leaves the waiter node registered, ccol_circq_send_zc ->
   * notify_one_sel_waiter dereferences the freed node here. */
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
   * fd selectables. In that path, the deadline branch of _sel_wait_condvar
   * must not spin forever and retry the same call. This holds when
   * ccol_cond_var_timedwait returns an error that is neither 0 nor
   * ETIMEDOUT. The branch must report ccol_unexpected_failure instead.
   * ccol_circq_timed_send_zc, ccol_circq_timed_recv_zc and
   * ccol_dynmq_timed_recv_zc already do this for the same class of failure.
   * The test uses a test-only hook to force exactly one such error. No
   * correct deadline that this library computes can produce a real EINVAL
   * from ccol_cond_var_timedwait. The test sets a large timeout of 5s and a
   * tight bound on the elapsed time. This proves that the call returns
   * quickly on the forced error. The call does not wait out the full
   * deadline, and it does not hang forever. A hang here stops this test from
   * ever returning. */
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  ccol_select_test_force_next_condvar_wait_error();

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
  /* The deadline branch of _sel_wait_condvar must not always report its
   * FAILURE outcome. This holds for any return from ccol_cond_var_timedwait
   * that is neither 0 nor ETIMEDOUT. The branch checks *ready again before
   * it declares a real failure, in the same way as the sibling ETIMEDOUT
   * branch above it. ccol_cond_var_timedwait always locks its mutex again
   * before it returns, on success and on failure. A notify from a producer
   * can therefore complete and set *ready = true one instant before an
   * unrelated, spurious wait error also arrives. Without that second check
   * in the FAILURE branch, the code drops a wake-up that already arrived.
   * ccol_select_timed then reports ccol_unexpected_failure instead of a
   * resume that succeeds.
   *
   * The test uses the dedicated test hook for exactly that order of events.
   * The hook forces the error and *ready = true in the same instant. A real
   * producer thread cannot race into this exact window on its own, because
   * the hook replaces ccol_cond_var_timedwait and does not unlock sel_mtx.
   * Nothing else can lock sel_mtx in the meantime. A second, real background
   * send races the re-scan that the call makes after the hook fires. That
   * send is what lets the call succeed on either path. After the call
   * resumes instead of giving up, it either sees this message at once or
   * falls through to an ordinary real wait for it. Other tests already cover
   * that wait well. Without that handling, this test always fails with
   * ccol_unexpected_failure and never gives ccol_success. */
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  sel_circq_args args = {.cq = cq, .delay_us = 50000, .value = 77};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_to_circq, &args), 0);

  ccol_select_test_force_next_condvar_wait_error_racing_ready();

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

TEST(ccol_select, write_circq_timed_out_when_sending_disabled) {
  /* A queue with sending off must make a write-direction ccol_select_timed
   * wait out the full timeout. The call must not return ccol_not_permitted
   * at once. The off state blocks new sends only, and it must not cut the
   * select short. */
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);
  ccol_circq_disable_sending(cq);

  size_t idx = 99;
  struct timespec before, after;
  getWallTime(before);
  REQUIRE_EQ(
      ccol_select_timed_va(&idx, 50000 /* us */,
                           ccol_selectable_from_circq(cq, ccol_select_write)),
      ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 50000);
  REQUIRE_EQ(idx, (size_t)99);

  ccol_circular_queue_destroy(cq);
}

TEST(ccol_select, write_dynq_timed_out_when_sending_disabled) {
  /* Same contract as above for ccol_dynamic_queue. */
  ccol_dynamic_queue *dq = ccol_dynamic_queue_create(NULL);
  ccol_dynmq_disable_sending(dq);

  size_t idx = 99;
  struct timespec before, after;
  getWallTime(before);
  REQUIRE_EQ(
      ccol_select_timed_va(&idx, 50000 /* us */,
                           ccol_selectable_from_dynq(dq, ccol_select_write)),
      ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 50000);
  REQUIRE_EQ(idx, (size_t)99);

  ccol_dynamic_queue_destroy(dq);
}

// concurrent write-waiters helpers
typedef struct {
  ccol_circular_queue *cq;
  ccol_retval_t result;
} sel_write_wait_result;

static void *thr_circq_write_wait(void *arg) {
  sel_write_wait_result *a = (sel_write_wait_result *)arg;
  size_t idx = 0;
  a->result = ccol_select_timed_va(
      &idx, 500000 /* us */,
      ccol_selectable_from_circq(a->cq, ccol_select_write));
  return NULL;
}

TEST(ccol_select, write_circq_two_concurrent_waiters_both_wake_on_slot_free) {
  /* Two threads wait at the same time for write readiness on a full queue of
   * capacity 1. One dequeue must cascade through the waiter list and wake
   * BOTH threads. It must not wake only the head waiter. Without that
   * cascade, the second waiter times out. */
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

  usleep(30000); /* let both threads register as write-waiters */

  c_message_t drain = {.data = NULL, .size = 0};
  REQUIRE_EQ(ccol_circq_recv_zc(cq, &drain), ccol_success);
  free(drain.data);

  if (started_t1) pthread_join(t1, NULL);
  if (started_t2) pthread_join(t2, NULL);
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
  a->result =
      ccol_select_timed_va(&idx, 500000 /* us */,
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
  /* This is the cascade in the read direction. ccol_select() never consumes
   * a message itself. It only peeks, in the same shape as the peek and the
   * forward-notify of the write-direction branch. A thread that finds itself
   * ready must therefore forward the notify to the next waiter. Without
   * that, two threads that wait at the same time to read from one empty
   * queue behave badly. Only the first one wakes, even though two messages
   * are available. */
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);

  sel_read_wait_result a1 = {.cq = cq, .result = ccol_unexpected_failure};
  sel_read_wait_result a2 = {.cq = cq, .result = ccol_unexpected_failure};
  pthread_t t1, t2;
  bool started_t1 = (pthread_create(&t1, NULL, thr_circq_read_wait, &a1) == 0);
  bool started_t2 = (pthread_create(&t2, NULL, thr_circq_read_wait, &a2) == 0);

  usleep(30000); /* let both threads register as read waiters */

  int *d0 = malloc(sizeof(int));
  assert(d0);
  *d0 = 1;
  int *d1 = malloc(sizeof(int));
  assert(d1);
  *d1 = 2;
  c_message_t m0 = {.data = d0, .size = sizeof(int)};
  c_message_t m1 = {.data = d1, .size = sizeof(int)};
  REQUIRE_EQ(ccol_circq_send_zc(cq, &m0), ccol_success);
  REQUIRE_EQ(ccol_circq_send_zc(cq, &m1), ccol_success);

  if (started_t1) pthread_join(t1, NULL);
  if (started_t2) pthread_join(t2, NULL);
  REQUIRE_TRUE(started_t1);
  REQUIRE_TRUE(started_t2);

  REQUIRE_EQ(a1.result, ccol_success);
  REQUIRE_EQ(a2.result, ccol_success);

  ccol_circular_queue_destroy(cq);
}

/* _sel_setup_epoll must not call epoll_ctl(EPOLL_CTL_ADD) once for each fd
 * selectable with no removal of duplicates. Two selectables can share one
 * real fd, in any mix of directions, and one can be an exact duplicate. The
 * second EPOLL_CTL_ADD then fails with EEXIST. The whole ccol_select or
 * ccol_select_timed call then fails with ccol_unexpected_failure. That
 * happens even for the common pattern of one connected socket that the
 * caller watches for readability and writability at the same time.
 * _sel_setup_epoll therefore groups the fd selectables by fd. It groups them
 * with a sort and not with a linear scan, so the cost stays O(n log n). It
 * does not fall to O(n^2) for the much more common case of many different
 * fds. It registers one combined epoll_ctl call for each unique fd. When a
 * combined event fires, it resolves that event back to the member selectable
 * whose own direction the event satisfies. */
TEST(ccol_select, fd_same_fd_two_directions_writable_wins) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  size_t idx = 999;
  ccol_selectable sels[2] = {
      ccol_selectable_from_fd(sv[0], ccol_select_read),
      ccol_selectable_from_fd(sv[0], ccol_select_write),
  };
  /* A socket that the test just connected is writable at once and has
   * nothing to read yet. This call must therefore resolve to the
   * write-direction selectable. It must not only succeed. */
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
  /* Both directions are ready now. One byte waits to be read, and the send
   * buffer still has room. Either member can correctly win. But the call
   * itself must succeed and must pick one of the two real members. It must
   * not give the EEXIST failure that the fd grouping prevents. */
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
  /* Nothing writes to this pipe. Two watches on the read end for read
   * readiness must still behave like one ordinary registration and time out.
   * The call must not succeed by accident, and it must not fail with an
   * unrelated error. */
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
  /* This call has an fd that is not ready, and the test registers it twice,
   * so both share one epoll_ctl group. The same call also has a queue
   * selectable that is really ready. This confirms that the fd grouping does
   * not disturb how ccol_select handles queue selectables. */
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
  /* These calls are checked. An unchecked failure here leaves c->mtx and
   * c->cond uninitialized. Every later pthread_mutex_lock and
   * pthread_cond_wait call in this file on them is then undefined behavior.
   * That includes a silent hang with no end, if the uninitialized bytes look
   * like a mutex that is already locked. This helper runs only from ordinary
   * test bodies in this file, and never from a forked child. An assert is
   * therefore safe here. A pthread_create inside a forked child elsewhere in
   * this file is a different case. */
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

/* ccol_event_loop never does the receive itself, for any type of selectable.
 * This matches the contract of _ccol_event_loop_run_callback. A registration
 * that a queue backs must therefore call ccol_circq_try_recv_zc or
 * ccol_dynmq_try_recv_zc here itself, exactly as a real caller does. A
 * result that is not a success leaves last_msg_valid false, and this
 * function does not assert. Such a result means that a consumer on another
 * thread already took the message. This is the same time-of-check to
 * time-of-use gap that a win in the write direction always has. */
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

/* This is an on_readable handler that removes itself. It removes its own
 * registration from inside the callback. It then behaves exactly like
 * evl_on_readable. */
static void evl_on_readable_self_remove(ccol_event_loop loop,
                                        ccol_event_reg reg,
                                        ccol_selectable *sel, void *arg) {
  evl_sync_ctx *c = (evl_sync_ctx *)arg;
  /* This uses assert() and not REQUIRE_EQ. This callback runs on the
   * reactor thread or the dispatch thread of ccol_event_loop. It runs at
   * the same time as the REQUIRE_EQ calls of the main test thread. The
   * assertion machinery of Tau uses plain globals with no lock and no
   * thread-local storage (see tau.h). A call to it from here is therefore a
   * real data race the moment this assertion fails. A failure is exactly
   * what this assertion exists to catch. It also does not abort the TEST()
   * around it, as a top-level REQUIRE_EQ does. Only the stack frame of this
   * function returns early. Every other background-thread helper in this
   * file follows the same assert()-only convention, for the same reason. */
  assert(reg == c->self_reg);
  assert(ccol_event_loop_remove(c->self_loop, reg) == ccol_success);
  evl_on_readable(loop, reg, sel, arg);
}

/* Waits until *counter_field >= target, or until timeout_ms passes. Returns
 * true if *counter_field reaches the target before the deadline. */
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
 * call read() itself. They do not leave the read to the single-threaded test
 * driver, which is what the fd-selectable path of evl_on_readable documents
 * for the same reason. This copies how a real production callback behaves
 * under truly concurrent dispatch. That makes a *blocking* read unsafe. The
 * documentation of ccol_event_loop warns that the receive call of a callback
 * "may find nothing, and must handle that gracefully". A plain blocking
 * read() is not graceful on an fd with nothing left to read and no writer
 * left to produce more. That state happens, for example, after every feeder
 * thread and driver thread in a test finishes. The read() then hangs the
 * reactor thread that called it forever. That in turn hangs the join of
 * ccol_event_loop_shutdown on that thread. This is a real deadlock, and you
 * see exactly that pair of stacks under thread-apply-all-bt in gdb on the
 * stuck test process. O_NONBLOCK makes "nothing available" return -1 with
 * EAGAIN at once instead. */
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
  /* The scope exit destroyed loop. There is nothing more to assert than
   * "no crash, and a clean run under valgrind". The memtest target checks
   * that. */
}

TEST(ccol_event_loop, fd_on_readable_fires) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  bool last_msg_valid;

  {
    /* This is a nested block. The multi-thread tests use the same pattern.
     * The comment on fd_modify_flips_direction_and_updates_sel gives the
     * full explanation. evl_on_readable never drains the data of an fd
     * selectable. That is by design: the caller reads sel->fd itself, as
     * this test does below. After pfd[0] becomes readable it therefore
     * stays ready, because the reactor is level-triggered. The reactor
     * thread then dispatches it again and again until the registration goes
     * away. The test must not destroy ctx, and this function must not
     * return, until that is sure to have stopped. Only the join at the end
     * of this block makes it sure. */
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
    /* This read happens under ctx.mtx and is not unprotected. The same
     * last_dir_seen handling in fd_modify_flips_direction_and_updates_sel
     * says why. */
    pthread_mutex_lock(&ctx.mtx);
    last_msg_valid = ctx.last_msg_valid;
    pthread_mutex_unlock(&ctx.mtx);

    char rbuf[16];
    ssize_t n = read(pfd[0], rbuf, sizeof(rbuf));
    REQUIRE_EQ(n, (ssize_t)sizeof(val));
    REQUIRE_EQ(*(int *)rbuf, 42);

    ccol_event_loop_remove(loop, reg);

    /* The block exit here shuts down loop and joins its one reactor thread.
     * ctx is quiet from this point on. */
  }

  /* An fd selectable never gets a msg. The caller reads sel->fd itself. */
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
    /* This is a nested block. fd_on_readable_fires uses the same pattern
     * and has the same comment. A new pipe write end stays writable
     * forever, because nothing fills its buffer. The reactor thread
     * therefore dispatches it again and again until the registration goes
     * away. */
    ccol_event_loop_construct_scoped(loop, 8, 1, 1);

    ccol_event_handlers_t handlers = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[1], ccol_select_write), handlers,
        &ctx, &err);
    REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

    /* A new pipe write end is always writable at once. */
    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, 1, 2000));

    ccol_event_loop_remove(loop, reg);
  }

  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop, fd_both_directions_combine_and_recombine) {
  /* A socketpair gives an fd that works in both directions. A pipe does not.
   * The test needs that to register read interest and write interest on the
   * SAME fd. This covers the path that adds with EPOLL_CTL_ADD and then
   * combines with MOD. It also covers the MOD back down on a partial
   * removal. */
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx read_ctx, write_ctx;
  evl_sync_ctx_init(&read_ctx);
  evl_sync_ctx_init(&write_ctx);

  {
    /* This is a nested block. fd_on_readable_fires uses the same pattern and
     * has the same comment. Both directions here stay ready forever, because
     * the reactor is level-triggered. evl_on_readable never drains the read
     * side of sv[0], and the write side never fills up. Both ctx structs
     * must therefore outlive every reactor thread. It is not enough for them
     * to outlive their own ccol_event_loop_remove call. */
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

    /* sv[0] is writable at once, because its send buffer is empty. */
    REQUIRE_TRUE(evl_wait_for(&write_ctx, &write_ctx.writable_count, 1, 2000));

    /* This removes the write direction, which is the MOD back down path.
     * The read direction must keep working after it. */
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
  /* The test registers both directions on the same fd. The peer then writes,
   * so sv[0] becomes readable while it is still writable. Both callbacks
   * must fire for the SAME epoll_wait batch. This is why ev.data.ptr carries
   * the shared event_entry and not a bare ccol_event_reg*. A bare
   * ccol_event_reg* delivers only one of the two. */
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx read_ctx, write_ctx;
  evl_sync_ctx_init(&read_ctx);
  evl_sync_ctx_init(&write_ctx);

  {
    /* This is a nested block. fd_on_readable_fires uses the same pattern
     * and has the same comment. */
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

    /* This drains the first "writable at once" dispatch before the test
     * writes data. The later wait then sees the combined batch with no
     * doubt. */
    REQUIRE_TRUE(evl_wait_for(&write_ctx, &write_ctx.writable_count, 1, 2000));

    int val = 99;
    REQUIRE_EQ((ssize_t)sizeof(val), write(sv[1], &val, sizeof(val)));

    REQUIRE_TRUE(evl_wait_for(&read_ctx, &read_ctx.readable_count, 1, 2000));
    /* sv[0] stays writable the whole time, because nothing fills its send
     * buffer. The writable callback must therefore fire again too. */
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
    /* This is a nested block. fd_on_readable_fires uses the same pattern and
     * has the same comment. An EPOLLHUP or EPOLLERR condition from a peer
     * that hung up stays until the fd goes away, because the reactor is
     * level-triggered. This is the same as a readable or writable fd that
     * nobody drains. The reactor thread therefore dispatches on_error again
     * and again until then. */
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

/* This guards against an asymmetric dispatch priority. The has_writer
 * comment in _ccol_event_loop_handle_event in src/cthreadcomm.c gives the
 * full mechanism. EPOLLOUT and EPOLLERR or EPOLLHUP are not mutually
 * exclusive. EPOLLIN and EPOLLERR or EPOLLHUP are not either. The setup of
 * fd_on_error_fires_for_both_directions just above shows this, and this test
 * mirrors it. A socket whose peer just hung up reports both states at once.
 * It reports writable, because the local send buffer still has room. A
 * write(2) on it therefore returns at once with an error and does not block.
 * It also reports an error. A standalone epoll probe against this exact
 * socketpair shape reports both directly. A write-only registration sets
 * on_writable and leaves on_error == NULL. That is a documented, supported
 * pattern. The doc comment of ccol_event_handlers_t says that "a write-only
 * producer that never expects on_error may pass NULL there". Such a
 * registration must still see on_writable fire for such an event. It must
 * not be starved of it forever, with nothing to report the error that comes
 * with it either.
 * num_reactor_threads == 1 here covers the copy of this handling in
 * _ccol_event_loop_handle_event. The _multi_thread sibling test below covers
 * the same copy in _ccol_event_loop_poller_collect. */
TEST(ccol_event_loop,
     fd_write_only_registration_still_fires_on_writable_when_peer_hangs_up) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  int error_count_seen;

  {
    /* This is a nested block. fd_on_readable_fires uses the same pattern and
     * has the same comment. The EPOLLHUP condition from a peer that hung up
     * stays until the fd goes away, because the reactor is level-triggered.
     * The reactor thread therefore dispatches on_writable again and again
     * until then. */
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

  /* on_error is NULL on this registration. There is nothing to consume the
   * error condition that comes with the event. The loop must therefore never
   * dispatch it. */
  REQUIRE_EQ(error_count_seen, 0);

  evl_sync_ctx_destroy(&ctx);
  close(sv[0]);
}

/* This is the same as the test just above, but with
 * num_reactor_threads == 3. That value routes the dispatch through
 * _ccol_event_loop_poller_collect and _ccol_event_loop_dispatch_job_fn
 * instead of _ccol_event_loop_handle_event. That path carries its own
 * separate copy of the same has_writer handling. The two copies are almost
 * identical on purpose, and they are not shared, so the
 * num_reactor_threads == 1 path stays byte for byte the same. The standing
 * design note of this module covers that duplication. The second copy needs
 * its own regression coverage. It is not correct by extension from the
 * first. */
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
 * tests below register an fd, remove that registration, register the same fd
 * number again and wait until the loop has reclaimed the entry of the removed
 * registration. Only then do they make the fd readable, and they require the
 * new registration to fire. A loop that let the entry of a removed
 * registration change the epoll interest of its fd later (an EPOLL_CTL_DEL or
 * EPOLL_CTL_MOD keyed by the fd number, from the reclaim or from the re-arm
 * after a dispatch that was in flight at the removal) takes the new
 * registration out of the interest set, and it then never fires. This test is
 * non-vacuous: an EPOLL_CTL_DEL of the fd in the entry reclaim makes every
 * case fail. */
extern uint64_t _ccol_event_loop_entries_reclaimed_for_tests(
    ccol_event_loop loop);

/* The gate that holds the callback of evl_readd_on_readable while it is
 * closed, and the flag that the callback sets once it is inside. Only the
 * in-flight case closes the gate. */
static atomic_bool evl_readd_gate_closed;
static atomic_bool evl_readd_in_callback;

/* Drains the fd and counts one readable dispatch. The drain is
 * non-blocking, because a dispatch can find nothing left to read. The wait
 * on the gate is bounded, so a callback can never hold the reactor for
 * ever. */
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

/* Runs one case on a fresh loop with num_threads reactor threads. It returns
 * a bit set of failures, 0 on success, and releases everything on every
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
      /* The documented order: the removal comes before the close. The new
       * socket takes the old number, directly when it is the lowest free one
       * and through dup2 otherwise, so the number is reused whatever else
       * the process opened meanwhile. */
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
    /* The entry of r1 is freed only now, after r2 owns the fd. */
    if (!evl_readd_wait_reclaimed(loop, base, 5000)) fail |= 1 << 7;

    if (r2 != CCOL_EVENT_REG_INVALID && sv[1] >= 0) {
      test_write_retry_eintr(sv[1], "x", 1);
      if (!evl_wait_for(&new_ctx, &new_ctx.readable_count, 1, 5000))
        fail |= 1 << 8;
      /* A second event proves that the interest stays armed after the
       * first dispatch (the EPOLLONESHOT re-arm of a dispatch pool). */
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

/* The removal of a registration whose callback is in flight, and a new
 * registration of the same fd while that callback still runs. The callback
 * then returns, and with a dispatch pool its job runs the post-dispatch
 * re-arm for the entry of the removed registration. Neither that step nor
 * the later reclaim of that entry may touch the interest that the new
 * registration installed. */
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

    /* The callback of r1 now drains the byte "a" and returns. */
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

  /* A flip of rreg to the write direction collides with wreg. */
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
    /* This is a nested block. The other multi-thread tests use the same
     * pattern. evl_on_readable never drains sv[0] for this fd selectable,
     * because nothing in this test reads it. After sv[0] becomes readable it
     * therefore stays ready, because the reactor is level-triggered. The
     * reactor thread then dispatches it again and again until the
     * registration goes away. The test must not destroy ctx until that is
     * sure to have stopped. This function must also not return and free the
     * stack slot of ctx before then. Only the join at the end of this block
     * makes it sure. */
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

    /* This reads under ctx.mtx directly. It does not go through
     * evl_wait_for, whose contract covers the counter only. evl_on_readable
     * keeps writing last_dir_seen again and again, because it never drains
     * sv[0] here. It does so for as long as the registration stays live and
     * the reactor thread keeps dispatching. A plain unprotected read right
     * after evl_wait_for returns therefore races those writes.
     * ThreadSanitizer reports that race here even at
     * num_reactor_threads == 1. It has nothing to do with dispatch on many
     * threads. One reactor thread that re-dispatches a level-triggered fd
     * that nobody drains is enough on its own. */
    pthread_mutex_lock(&ctx.mtx);
    last_dir_seen = ctx.last_dir_seen;
    pthread_mutex_unlock(&ctx.mtx);

    ccol_event_loop_remove(loop, reg);

    /* The block exit here shuts down loop and joins its one reactor thread.
     * ctx is quiet from this point on. */
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
  /* ccol_event_loop_modify works for an fd only. ccol_event_loop_pause and
   * ccol_event_loop_resume have the same restriction. See
   * pause_and_resume_reject_queue_selectable. The direction of a queue
   * registration or a ccol_channel registration is part of its identity. To
   * change it, the caller removes the registration and adds it again. The
   * caller does not flip the direction in place. The call must therefore
   * fail with ccol_invalid_args. It must not quietly do nothing, and it must
   * not corrupt the registration. The type check always runs first, before
   * the "already in the direction that the caller asks for" check inside
   * ccol_event_loop_modify. A call for the same direction therefore fails in
   * the same way. The library does not treat it as a success that does
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
  /* A request for the direction that the registration already has must fail
   * in the same way. The library must not treat it as a call that does
   * nothing. */
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
  /* This joins the poller thread and any dispatch workers. No callback can
     then still run with &ctx when the code below destroys the sync context.
     ccol_event_loop_remove alone does not wait for a dispatch that was
     already in flight at the time of the call. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  ccol_circular_queue_destroy(cq);
}

TEST(ccol_event_loop, queue_circq_writable_fires_without_consuming) {
  ccol_circular_queue *cq = ccol_circular_queue_create(1, NULL);

  /* Fill the queue, so the write direction is not ready at once. */
  int *filler = malloc(sizeof(int));
  *filler = 1;
  c_message_t fmsg = {.data = filler, .size = sizeof(int)};
  REQUIRE_EQ(ccol_circq_send_zc(cq, &fmsg), ccol_success);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  {
    /* This is a nested block. fd_on_readable_fires uses the same pattern and
     * has the same comment. evl_on_writable never consumes anything on cq
     * and never produces anything on it. That is by design, to prove that
     * the reactor never does it for the callback. The test itself then does
     * a ccol_circq_try_send_zc and ccol_circq_recv_zc round trip, after the
     * first wait already succeeds. That round trip makes cq ready for a
     * write again. A second on_writable dispatch can therefore still be in
     * flight on the reactor thread. That can happen at the exact moment when
     * this function would go on to remove the registration. It would then
     * destroy ctx and cq. ccol_event_loop_remove only stops a FUTURE
     * dispatch from starting. It does not wait for one that is already in
     * flight. See the comment of _ccol_event_loop_run_callback. The test
     * must not destroy ctx or cq, and this function must not return, until
     * that is sure to have stopped. Only the join at the end of this block
     * makes it sure. That join comes from the scope-exit destructor of
     * ccol_event_loop_construct_scoped. The race is real, not theoretical.
     * Without this nested block, ThreadSanitizer reports that the main
     * thread destroys ctx.mtx and ctx.cond while evl_on_writable still uses
     * them on the reactor thread. It reports this in about 1 of every 4 to 9
     * runs. */
    ccol_event_loop_construct_scoped(loop, 8, 1, 1);

    ccol_event_handlers_t handlers = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_circq(cq, ccol_select_write), handlers, &ctx,
        &err);
    REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

    /* This frees the slot. on_writable must fire. It must NOT consume
     * anything. The queue has room, and it does not hold a message. The
     * callback itself must do the real send. */
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

    /* The block exit here shuts down loop and joins its one reactor thread.
     * ctx and cq are quiet from this point on. */
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
  /* This joins the poller thread and any dispatch workers. No callback can
     then still run with &ctx when the code below destroys the sync context.
     ccol_event_loop_remove alone does not wait for a dispatch that was
     already in flight at the time of the call. */
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
  /* A new thread calls this, so ccol_get_thread_id() differs from the owner
   * of the ccol_channel. The owner is the test thread that called
   * ccol_channel_create. The send therefore routes to workers_to_owner_cq.
   * That is exactly what the owner-side ccol_select_read registration below
   * watches. */
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
  /* This thread is the owner of the ccol_channel. The owner reads from
   * ccol_workers_to_owner. A worker on another thread must therefore send,
   * for the owner-side read registration to fire. */
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
  /* This joins the poller thread and any dispatch workers. No callback can
     then still run with &ctx when the code below destroys the sync context.
     ccol_event_loop_remove alone does not wait for a dispatch that was
     already in flight at the time of the call. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  ccol_channel_destroy(ch);
}

TEST(ccol_event_loop, queue_persistent_across_multiple_cycles) {
  /* This is the core property: a registration is persistent and not for one
   * call only. One registration keeps firing across many separate send and
   * recv cycles, and the test never adds it again. */
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
  /* This joins the poller thread and any dispatch workers. No callback can
     then still run with &ctx when the code below destroys the sync context.
     ccol_event_loop_remove alone does not wait for a dispatch that was
     already in flight at the time of the call. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  ccol_circular_queue_destroy(cq);
}

TEST(ccol_event_loop, queue_already_pending_message_at_registration_time) {
  /* The loop must still deliver a message that the test sends BEFORE the
   * call to ccol_event_loop_add. The bridge eventfd has no earlier notify to
   * use. ccol_event_loop_add must therefore trigger itself when the queue is
   * already in the target state at registration time. */
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
  /* This joins the poller thread and any dispatch workers. No callback can
     then still run with &ctx when the code below destroys the sync context.
     ccol_event_loop_remove alone does not wait for a dispatch that was
     already in flight at the time of the call. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  ccol_circular_queue_destroy(cq);
}

typedef struct evl_two_readers_ctx {
  _Atomic size_t calls;
} evl_two_readers_ctx;

/* This callback is slower than a feeder that refills the queue all the time,
 * on purpose. evl_bounded_hot_queue_on_readable further below has the same
 * shape. A real backlog of 2 or more messages therefore waits behind this
 * registration by the time it dispatches. That backlog gives
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
    /* This send blocks. The finite capacity of the queue is what stops this
     * feeder from racing far ahead of both registrations. It still builds a
     * real backlog against the slow registration. */
    (void)ccol_circq_send_zc(a->cq, &msg);
  }
  return NULL;
}

TEST(ccol_event_loop, queue_second_registration_on_same_queue_is_not_starved) {
  /* This guards against a silent starvation. notify_one_sel_waiter() wakes
   * only the CURRENT head of the waiter list of a queue. A ccol_select()
   * waiter cascades that wake onward correctly, because it always unlinks
   * itself before it checks readiness again. The waiter_node of a
   * ccol_event_loop registration instead stays linked into the list for
   * good, until ccol_event_loop_remove. Without
   * _ccol_event_loop_queue_cascade_notify_next, a second live registration
   * on the same queue and direction therefore gets ZERO notifications
   * forever. That happens when it sits BEHIND another one that never
   * unlinks, however much traffic the queue sees.
   *
   * The test registers r1 FIRST, so that r1 ends up linked behind r2. The
   * registration that the test adds last always becomes the new list head.
   * See the "prepend to head" linking in ccol_event_loop_add. This test is
   * not vacuous: remove the cascade forward, and ctx1.calls stays at 0 in
   * this exact scenario every time. The test is not merely flaky, because
   * nothing else can reach a registration that sits behind a head that never
   * unlinks. The bounded wait below then runs to its full timeout with
   * ctx1.calls still 0, and it does not pass now and then. */
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
     * callback is slow on purpose, which lets a backlog build up. r2 then
     * cascades that backlog forward toward r1. */
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

    /* This is a bounded wait. r2, the slow head, gets most of the count.
     * The property under test is that r1, the tail, gets ANY turn at all. */
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

    /* The block exit here shuts down loop and joins its one reactor thread.
     * Only after this point is it safe to touch cq or to destroy it. Before
     * it, a dispatch that is already in flight can still use cq. That
     * dispatch is an ordinary on_readable callback or a cascade forward.
     * This is the same hazard that
     * remove_from_different_thread_concurrent_with_dispatch above documents
     * and guards against. */
  }

  /* This drains what is left. A small number of messages can stay, and that
   * is expected and correct. It matches the documented coalescing behaviour,
   * which is a separate topic. See the comment of
   * multi_thread_hot_queue_dispatch_pool_pending_stays_bounded.
   * ccol_circular_queue_destroy asserts on any message that stays. */
  c_message_t leftover = {.data = NULL, .size = 0};
  while (ccol_circq_try_recv_zc(cq, &leftover) == ccol_success) {
    /* nothing to free: every sent message here has data == NULL */
  }
  ccol_circular_queue_destroy(cq);
}

/* This feeds one message at a time and sleeps between the sends. The queue
 * therefore always has time to drain fully before the next message arrives.
 * This is the OPPOSITE traffic shape from evl_two_readers_feeder_thread
 * above, on purpose. That feeder floods the queue as fast as it can, to
 * build a backlog. A backlog that never exists is exactly the condition
 * where the cascade has nothing to forward, because the cascade
 * (_ccol_event_loop_queue_cascade_notify_next) depends on readiness. This
 * feeder therefore tests the round-robin rotor in notify_one_sel_waiter, and
 * not the cascade. */
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
  /* This guards against a silent starvation that no ordinary test finds.
   * notify_one_sel_waiter() must not always wake the CURRENT HEAD of the
   * waiter list of a queue. The waiter_node of a ccol_event_loop
   * registration stays linked for good and never links again. The transient
   * node of a ccol_select() caller is different. So the registration that
   * the caller adds LAST becomes the head and stays the head. It would then
   * be the ONLY one that ever gets a direct notify, for as long as it stays
   * registered. The cascade mechanism forwards a wake only when the queue is
   * STILL ready right after the callback of the notified registration
   * returns. A callback that keeps up with the traffic routinely leaves
   * nothing to forward. That includes a plain callback with one single
   * ccol_circq_try_recv_zc() for each call and no loop. That is a
   * documented, ordinary pattern, and it is exactly what both registrations
   * below do. Every OTHER live registration would then get ZERO callbacks
   * for as long as the traffic flows. That directly contradicts the
   * documented guarantee of ccol_event_loop_add: "no live listener is ever
   * passed over indefinitely". That failure is total and not occasional.
   * With head-only notification, two registrations that get 2000 messages
   * one at a time produce calls1=0 and calls2=2000 on every run. The waiter
   * list of each queue and direction therefore carries its own round-robin
   * rotor. See the doc comment of notify_one_sel_waiter. Every live
   * registration then gets a turn once for each full rotation. The same one
   * does not win forever. */
  ccol_circular_queue *cq = ccol_circular_queue_create(64, NULL);

  evl_two_readers_ctx ctx1, ctx2;
  atomic_init(&ctx1.calls, (size_t)0);
  atomic_init(&ctx2.calls, (size_t)0);

  {
    ccol_event_loop_construct_scoped(loop, 8, 1, 1);

    /* Both registrations use the same "fast" callback on purpose. That
     * callback has one recv and no loop. This is the exact pairing where
     * head-only notification starves one side completely. A "slow" head, as
     * the sibling test above uses, builds a backlog by hand. The cascade
     * mechanism can then ride that backlog to reach r1. Two callbacks that
     * are equally fast build no such backlog at all. */
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

    /* A bounded wait, so that any last dispatch in flight can settle. */
    struct timespec ts = {0, 50000000}; /* 50ms */
    nanosleep(&ts, NULL);

    /* Both registrations must get a really fair share, and not only "more
     * than zero". Head-only notification produces an exact 0 to N split, so
     * a loose ">0" bound alone would already mean something. But the test
     * asserts that each side gets at least a quarter of a perfectly even
     * split of one half each. That gives real margin against scheduling
     * jitter, and it still fails hard against all-or-nothing behavior. */
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

/* This is the ccol_dynamic_queue version of the ccol_circular_queue test
 * above. The rotor works in the same way for the sel_read_rotor field and
 * the sel_write_rotor field of ccol_dynamic_queue. It uses the same
 * notify_one_sel_waiter() and _sel_unlink_waiter() code paths. This test
 * therefore covers that half directly. It does not depend on the
 * ccol_circular_queue coverage alone. */
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

    struct timespec ts = {0, 50000000}; /* 50ms */
    nanosleep(&ts, NULL);

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

  /* The loop must NOT deliver a second message, because the registration
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

  /* This joins the poller thread and any dispatch workers. No callback can
     then still run with &ctx when the code below destroys the sync context.
     ccol_event_loop_remove alone does not wait for a dispatch that was
     already in flight at the time of the call. */
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
  /* This repeats a cycle of add, notify and a concurrent remove from another
   * thread. It is a targeted stress test for the refcount design. It must
   * never crash and must never leak. The memtest target checks that
   * separately under valgrind.
   *
   * The test allocates each ctx on the heap for each iteration and logs it.
   * It does not destroy it inline. ccol_event_loop_remove is documented to
   * return while a dispatch for the removed reg can still run. The call
   * defers only the memory reclamation of the library itself, and not how
   * long the callback takes. A join of the remover thread therefore does not
   * prove that evl_on_readable stopped touching ctx. To destroy ctx.mtx, or
   * to reuse its stack slot on the next iteration, would race that callback
   * in flight. ThreadSanitizer reports that even at
   * num_reactor_threads == 1. The hazard has nothing to do with dispatch on
   * many threads. It comes from the documented contract of
   * ccol_event_loop_remove. The test frees every logged ctx only after it
   * joins the whole loop. That join also covers the one reactor thread that
   * can still be inside a callback. The nested block below does that join.
   * cq carries the same hazard. The scoped loop has an automatic destructor
   * that runs at the closing brace of this function. That destructor joins
   * the reactor thread. To destroy cq before that join would let a stale
   * dispatch, already collected for the reg that the last iteration removed,
   * call ccol_circq_try_recv_zc(sel->cq, ...) on a queue that is already
   * freed. The test therefore destroys cq only after that same join, exactly
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

      /* This drains what is left, so the queue does not grow without bound
       * across the iterations. Nothing consumes the message when the removal
       * wins the race before the dispatch. */
      c_message_t leftover;
      while (ccol_circq_try_recv_zc(cq, &leftover) == ccol_success) {
        free(leftover.data);
      }
    }

    REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)0);

    /* The block exit here shuts down loop and joins its one reactor thread.
     * cq and every logged ctx are quiet from this point on. */
  }

  ccol_circular_queue_destroy(cq);
  for (int i = 0; i < ctx_log_count; i++) {
    evl_sync_ctx_destroy(ctx_log[i]);
    free(ctx_log[i]);
  }
}

/* This is a "slow callback" fixture that the test controls, for the ordering
 * guarantee of on_removed. It differs from evl_sync_ctx above. It needs an
 * explicit gate that the test controls, to hold a dispatch open on purpose.
 * That gate copies a callback that is really still in flight. No field of
 * evl_sync_ctx gives that. */
typedef struct evl_slow_cb_ctx {
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  bool callback_started;
  bool release_callback;
  bool callback_finished;
  bool on_removed_called;
  /* This is a copy of callback_finished that evl_on_removed_slow takes from
   * inside itself, under the same mtx. It lets the test prove the order:
   * on_removed always sees callback_finished already true. Without it, the
   * test only sees both flags true in the end, with no proof of which one
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

/* This is a bounded wait for *flag to become true, and cond signals it. The
 * wait is never unbounded. An unbounded wait on the main thread can hang the
 * whole test binary under load, and not only fail one test. This helper
 * works for any bool field of evl_slow_cb_ctx. evl_wait_for above is
 * different, because it works only on the int counters of evl_sync_ctx. */
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

/* This is an on_readable handler that blocks on the reactor thread. It
 * blocks until the main thread of the test releases it. This lets the test
 * force the exact order of events that it must check. It also makes that
 * order deterministic instead of a race against incidental timing. The order
 * under test is a return from ccol_event_loop_remove() while this dispatch
 * is still really in flight. The deadline below bounds the block even when
 * nothing releases it. A bug that skips the release can therefore never hang
 * the test binary. It can only fail the assertion that depends on a release
 * in time. */
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
  /* This reproduces the documented hazard of
   * remove_from_different_thread_concurrent_with_dispatch above, on purpose
   * and deterministically. It proves that on_removed closes that hazard.
   * Without on_removed, a caller has no way to know that a dispatch in
   * flight still touches arg at the moment when ccol_event_loop_remove()
   * returns. The comment of that other test explains this. */
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

  /* The one reactor thread now blocks inside evl_on_readable_slow. It holds
   * the entry->dispatch_lock of this registration. This thread now removes
   * the registration at the same time, while that dispatch is still really
   * in flight. */
  REQUIRE_EQ(ccol_event_loop_remove(loop, reg), ccol_success);

  /* ccol_event_loop_remove() must return and must not wait for the callback.
   * The doc comment of that function says why. This gives on_removed a wide
   * window, to prove that it does NOT fire while the callback still
   * blocks. */
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
   * dispatches it. Only on_removed is under test here. */
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
  /* Most registrations in this codebase leave on_removed unset, at NULL.
   * This test pins that as a fully supported case that does nothing. It must
   * not crash. A caller that has no use for the notification must not need a
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
     * registration still in place. Only on_removed is under test here. */
    ccol_event_handlers_t handlers = {.on_readable = NULL,
                                      .on_writable = NULL,
                                      .on_error = NULL,
                                      .on_removed = evl_on_removed_slow};
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
        &err);
    REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);
    /* Nothing removes this registration. The destructor of the scoped loop,
     * at the closing brace of this block, tears it down while it is still
     * registered. This covers the direct walk in
     * _ccol_event_loop_teardown_raw. It does not cover the ordinary path of
     * removal and reclaim that the tests above cover. */
  }

  pthread_mutex_lock(&ctx.mtx);
  REQUIRE_TRUE(ctx.on_removed_called);
  REQUIRE_EQ(ctx.on_removed_arg_seen, (void *)&ctx);
  pthread_mutex_unlock(&ctx.mtx);

  evl_slow_cb_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

/* This carries the arguments and the result for the background thread below.
 * That thread makes one ccol_event_loop_reg_generation call. The test slows
 * that call down on purpose with
 * ccol_event_loop_test_delay_next_reg_resolve_unpin_ms. The delay lands at
 * the exact point where the call releases its resolve pin on reg. */
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
  /* This reproduces one exact order of events, deterministically. A resolve
   * pin is still held at the exact moment when ccol_event_loop_remove marks
   * reg removed and defers its reclaim. The pin comes from
   * ccol_event_loop_reg_generation here. That call stands in for
   * ccol_event_loop_modify, ccol_event_loop_pause and
   * ccol_event_loop_resume. All of them are documented as callable at the
   * same time as ccol_event_loop_remove against the same reg. The
   * maybe_needs_wake copy inside _ccol_event_reg_resolve_unpin comes from
   * before remove() runs. It is therefore stale by the time the delayed
   * unpin below drops pending_resolve_count to 0. The ping of that unpin is
   * a documented, deliberate no-op in this exact order of events. See its
   * comment. This loop is otherwise idle. Only the bounded epoll_wait retry
   * of EVENT_LOOP_RECLAIM_RETRY_MS is left to reclaim reg and to fire
   * on_removed. See _ccol_event_loop_reclaim_pending_frees. The bounded wait
   * below proves that this retry is what delivers it. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 4, 1);

  evl_slow_cb_ctx ctx;
  evl_slow_cb_ctx_init(&ctx);
  /* on_readable stays NULL. This test never writes to pfd[1], so nothing
   * dispatches it at all. Only on_removed is under test here. */
  ccol_event_handlers_t handlers = {.on_readable = NULL,
                                    .on_writable = NULL,
                                    .on_error = NULL,
                                    .on_removed = evl_on_removed_slow};
  char *err = NULL;
  ccol_event_reg reg = ccol_event_loop_add(
      loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
      &err);
  REQUIRE_NE(reg, CCOL_EVENT_REG_INVALID);

  /* This arms a one-shot delay for the next resolve-unpin in the whole
   * process. See the doc comment of that function. The background thread
   * that starts below is what consumes the delay. */
  ccol_event_loop_test_delay_next_reg_resolve_unpin_ms(300);

  evl_delayed_reg_generation_ctx gen_ctx = {.loop = loop, .reg = reg, .gen = 0};
  pthread_t th;
  int create_rv =
      pthread_create(&th, NULL, evl_delayed_reg_generation_thread, &gen_ctx);
  REQUIRE_EQ(create_rv, 0);

  /* This is a wide window for the background thread. In it, that thread
   * resolves reg, which pins it, and enters the armed delay, before the
   * remove below runs. This is what makes ccol_event_loop_remove see
   * pending_resolve_count > 0 for reg. It then defers the reclaim instead of
   * freeing reg at once. */
  usleep(50000);

  ccol_retval_t remove_rv = ccol_event_loop_remove(loop, reg);

  int join_rv = pthread_join(th, NULL);
  /* This call runs every time. It stops an unconsumed delay from leaking
   * into a later, unrelated test. It does nothing when the resolve-unpin of
   * the background thread above already consumed the delay, which is the
   * expected case. It stands before every REQUIRE_* below, so it always
   * runs whichever assertion fails first. The REQUIRE_* macros of Tau
   * return from the test function at once on a failure. They skip every
   * later line. Cleanup can therefore not stand after an assertion that can
   * fail. */
  ccol_event_loop_test_delay_next_reg_resolve_unpin_ms(0);

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
   * ccol_event_loop_reg_generation. A result that is not 0 proves that the
   * resolve of the background thread really succeeded. It therefore really
   * held a pin on reg. Without this check, the race can go the other way and
   * exercise nothing. */
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
  /* Idempotent: calling again must not hang or double-join. */
  REQUIRE_EQ(ccol_event_loop_shutdown(loop), ccol_success);

  ccol_event_loop_destroy(loop);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop,
     destroy_while_queue_registration_pending_queue_outlives_loop) {
  /* The library must unlink the waiter_node of a queue-backed
   * ccol_event_reg from the waiter list of the queue before it frees that
   * reg. Without that step, the queue holds a dangling pointer. The next
   * send or receive on the queue is then a use-after-free. That is even more
   * dangerous here, because this test deliberately destroys the queue only
   * after it destroys the ccol_event_loop. */
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

  /* This destroys the loop while the registration is still pending. The
   * library must unlink from the waiter list of cq before it frees, and not
   * after. */
  ccol_event_loop_destroy(loop);

  /* The queue must still be perfectly usable afterward. */
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

/* This is a regression test for an ordering hazard in ccol_event_loop_add.
 * No crash reproduction shows this hazard. Only the reasoning about the
 * order shows it. ccol_event_loop_add must NOT wire reg into the fd registry
 * or the queue registry before it mints the public handle of reg. That wire
 * step makes reg fully live and dispatchable through the raw
 * ccol_event_reg_s* that the registry stores. _ccol_event_reg_slot_acquire
 * is what mints the handle. Its only failure mode is a failed allocation
 * that grows loop->reg_slots. On that failure ccol_event_loop_add reports
 * CCOL_EVENT_REG_INVALID to the caller. The caller then believes that the
 * library never created a registration. It has no reason to think that a
 * callback can already run against the arg pointer that it gave. For
 * num_reactor_threads > 1 that callback can also sit queued on a ctpool
 * worker. The caller can therefore free arg at once. That is a real
 * use-after-free. It needs only an ordinary allocation failure at the same
 * time as a target fd that is already ready. The library therefore acquires
 * the slot BEFORE it wires reg into the registry. A slot-acquire failure
 * then always finds reg fully unwired, and no dispatch is possible.
 *
 * The primary assertion below is last_forced_..._reg_count == 0. It proves
 * the ORDER directly and deterministically. It does not depend on a real
 * race between threads.
 * ccol_event_loop_test_last_forced_slot_acquire_failure_reg_count() reports
 * the value of ccol_event_loop_reg_count() at one exact moment. That moment
 * is inside _ccol_event_reg_slot_acquire, when the forced failure fires, and
 * before any rollback can run. The opposite order is "wire first, acquire
 * the slot last". That order would already have incremented reg_count for
 * this registration at that point, so this snapshot would read 1 and not 0.
 * A check of ccol_event_loop_reg_count() after the return cannot tell the
 * two orders apart. The rollback of a failure after the wire step also
 * restores the count to 0 before ccol_event_loop_add returns.
 *
 * This is why a simple live-dispatch race is not a reliable regression guard
 * on its own. Such a race arms the hook, keeps a genuinely ready fd next to
 * it, and checks whether a callback fires. Build ccol_event_loop_add in the
 * "wire, then acquire" shape and keep this same hook. The window between the
 * release of the stripe lock and the slot-acquire call that follows it at
 * once is then too narrow in practice. Even a poller thread on a separate
 * core, already running and already blocked in epoll_wait, does not reliably
 * win it. That holds across 5 consecutive full runs with
 * num_reactor_threads = 3 and with pfd[0] already holding data before the
 * call. The reg_count snapshot below has no such dependency on timing. It
 * reads the order directly off the work that ccol_event_loop_add already
 * completed, at the instant the hook fires. It is not a race between two
 * threads. The live-dispatch checks further down are a secondary,
 * best-effort corroboration. They also exercise the real end-to-end
 * behavior of the module. But the correctness of this test rests on the
 * reg_count snapshot. */
TEST(ccol_event_loop,
     add_slot_acquire_failure_leaves_nothing_wired_or_dispatched) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  int one = 1;
  REQUIRE_EQ(write(pfd[1], &one, sizeof(one)), (ssize_t)sizeof(one));

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  {
    /* num_reactor_threads = 3 exercises the async dispatch-pool path, which
     * is the more dangerous one. A ctpool worker can run the callback long
     * after the caller of ccol_event_loop_add already moves on. This value
     * therefore covers more than the inline num_reactor_threads == 1
     * path. */
    ccol_event_loop_construct_scoped(loop, 8, 1, 3);

    ccol_event_handlers_t handlers = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};

    ccol_event_loop_test_force_next_reg_slot_acquire_failure();

    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
        &err);
    REQUIRE_EQ(reg, CCOL_EVENT_REG_INVALID);
    REQUIRE_NE((void *)err, NULL);
    REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)0);

    /* The deterministic proof: see this test's own leading comment. */
    REQUIRE_EQ(
        ccol_event_loop_test_last_forced_slot_acquire_failure_reg_count(),
        (size_t)0);

    /* This is a best-effort corroboration. pfd[0] is already readable and
     * stays readable. If the failed registration left anything wired, the
     * poller has every chance to dispatch it during this bounded wait. This
     * loop has num_reactor_threads > 1, so a dispatch_pool worker has that
     * chance too. This test does not depend on this check to catch a
     * regression. The leading comment of this test says why. A real bug here
     * still shows up as a crash or a use-after-free under valgrind or
     * AddressSanitizer. It shows up long before the timing margin of this
     * assertion becomes the limit. */
    REQUIRE_FALSE(evl_wait_for(&ctx, &ctx.readable_count, 1, 300));
    pthread_mutex_lock(&ctx.mtx);
    REQUIRE_EQ(ctx.readable_count, 0);
    pthread_mutex_unlock(&ctx.mtx);

    /* The hook is one-shot. An ordinary call right after it must succeed.
     * That confirms that the hook affected only the single call above. */
    err = NULL;
    ccol_event_reg reg2 = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx,
        &err);
    REQUIRE_NE(reg2, CCOL_EVENT_REG_INVALID);
    REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)1);
    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));

    ccol_event_loop_remove(loop, reg2);

    /* The block exit here shuts down loop and joins every one of its
     * threads. ctx is quiet from this point on. */
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

  for (int i = 0; i < n; i++) {
    REQUIRE_EQ(pipe(pfds[i]), 0);
    char *err = NULL;
    regs[i] = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfds[i][0], ccol_select_read), handlers,
        NULL, &err);
    REQUIRE_NE(regs[i], CCOL_EVENT_REG_INVALID);
  }
  REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)n);

  for (int i = 0; i < n; i++) {
    REQUIRE_EQ(ccol_event_loop_remove(loop, regs[i]), ccol_success);
    close(pfds[i][0]);
    close(pfds[i][1]);
  }
  REQUIRE_EQ(ccol_event_loop_reg_count(loop), (size_t)0);
}

TEST(ccol_event_loop, multiple_independent_instances) {
  /* Two separate ccol_event_loop instances share no global state. That
   * includes their lock-stripe arrays. loop_a uses 1 stripe, which is the
   * same as a single lock. loop_b uses 8. The two counts differ on purpose.
   * This confirms that num_lock_stripes is a setting of one instance only.
   * One instance does not disturb the other. */
  int pfd_a[2], pfd_b[2];
  REQUIRE_EQ(pipe(pfd_a), 0);
  REQUIRE_EQ(pipe(pfd_b), 0);

  evl_sync_ctx ctx_a, ctx_b;
  evl_sync_ctx_init(&ctx_a);
  evl_sync_ctx_init(&ctx_b);

  {
    /* This is a nested block. fd_on_readable_fires uses the same pattern and
     * has the same comment. Both fds here stay ready, because the reactor is
     * level-triggered and evl_on_readable never drains an fd selectable. The
     * test must therefore join the reactor threads of both loops before it
     * destroys either ctx. */
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
    /* The registration of loop_b must not fire for the fd of loop_a. That
     * is the property under test. The read takes the lock of ctx_b: its
     * callback writes the count under that lock once pfd_b is written below,
     * and only the lock orders this read before that write for the memory
     * model, since the kernel event in between is no synchronisation that
     * the language knows. */
    pthread_mutex_lock(&ctx_b.mtx);
    int b_count_before = ctx_b.readable_count;
    pthread_mutex_unlock(&ctx_b.mtx);
    REQUIRE_EQ(b_count_before, 0);

    REQUIRE_EQ((ssize_t)sizeof(val_b), write(pfd_b[1], &val_b, sizeof(val_b)));
    REQUIRE_TRUE(evl_wait_for(&ctx_b, &ctx_b.readable_count, 1, 2000));

    ccol_event_loop_remove(loop_a, reg_a);
    ccol_event_loop_remove(loop_b, reg_b);

    /* The block exit here shuts down both loops and joins their reactor
     * threads. ctx_a and ctx_b are quiet from this point on. */
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
 * for the maxevents parameter of epoll_wait(2). The same function also sizes
 * the events buffer of the poller thread from it, as
 * max_events_per_wait * sizeof(ccol_poll_event). That value needs an
 * upper bound. Without one, two failures are possible. A value whose low 32
 * bits read as a non-positive signed int makes epoll_wait fail with EINVAL
 * on the very first call. A value that is merely huge makes the allocation
 * of the events buffer fail instead. The error handling of the poller thread
 * treats both as an ordinary, silent, permanent thread exit. Nothing tells
 * them apart from any other unexpected epoll_wait failure or allocation
 * failure. The constructor then still returns a ccol_event_loop handle that
 * looks valid and live, and ccol_event_loop_add keeps succeeding. But no
 * callback ever fires again, and the caller sees nothing. Creation therefore
 * rejects any max_events_per_wait above INT_MAX. It also rejects any value
 * that overflows size_t when multiplied by sizeof(ccol_poll_event). That
 * second guard is the binding one on an ILP32 platform. There, INT_MAX alone
 * does not stop that multiplication from overflowing a 32-bit size_t. */
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

/* A max_events_per_wait well inside both guards must still be accepted. The
 * loop that results must also really dispatch. This confirms that the
 * validation above is not too strict. */
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
  /* This is the same scenario as fd_both_directions_combine_and_recombine,
   * but with num_lock_stripes > 1. The read direction and the write
   * direction on the SAME fd hash to the SAME stripe, because
   * _stripe_index_for_fd is a pure function of the fd. They therefore stay
   * serialized inside that stripe. The path that adds with EPOLL_CTL_ADD and
   * then combines with MOD must behave exactly as in the single-stripe case.
   * The MOD back down on a partial removal must do the same. */
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx read_ctx, write_ctx;
  evl_sync_ctx_init(&read_ctx);
  evl_sync_ctx_init(&write_ctx);

  {
    /* This is a nested block. fd_on_readable_fires uses the same pattern
     * and has the same comment. */
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
  /* This is the same scenario as fd_modify_after_remove_returns_invalid_args,
   * but with num_lock_stripes > 1. ccol_event_loop_modify must still key its
   * stripe lookup off reg->stripe_idx. That read needs no lock and is always
   * safe for any reg* that the caller legitimately holds. The call must
   * return ccol_invalid_args cleanly when reg is already removed. It must
   * not crash and it must not behave badly.
   *
   * This test deliberately does NOT try to force the memory of reg to be
   * really freed before it calls modify. One way to force that is to let
   * several reactor drain cycles pass first. reg stays valid, stale memory
   * only across the *short* window before the next drain claims it. See the
   * doc comment of _ccol_event_loop_defer_reg_free. Extra drain cycles here
   * would close that window and really free reg. That is no longer a stale
   * reg* that the library handles cleanly. It is a real use-after-free
   * outside the contract. valgrind then correctly reports it against the
   * premise of the test, and not against ccol_event_loop_modify or
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
  /* This test pauses BEFORE anything makes the fd ready. That is
   * deliberate. The other order waits for one delivery and then races a
   * pause call against it. A plain pipe read end stays ready once it becomes
   * ready, until something drains it. With num_reactor_threads > 1,
   * EPOLLONESHOT is in play. The re-arm that the dispatch job runs after the
   * callback then runs on a completely separate thread from the one that
   * calls ccol_event_loop_pause. Nothing synchronizes "the test saw the
   * callback run once" with "the pause takes effect before the next
   * re-arm". An assertion in that order is a real race. It fails now and
   * then, and not deterministically. A pause first avoids the race
   * completely. No delivery can happen before the pause takes effect,
   * because none has happened yet. */
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

  /* This makes the fd genuinely ready while it is paused. No on_readable
   * callback must fire, however long the test waits. The bounded wait below
   * stands in for "never". That is the convention of this file for an
   * assertion of a negative. */
  int val = 1;
  REQUIRE_EQ(write(pfd[1], &val, sizeof(val)), (ssize_t)sizeof(val));
  REQUIRE_FALSE(evl_wait_for(&ctx, &ctx.readable_count, 1, 300));

  REQUIRE_EQ(ccol_event_loop_resume(loop, reg), ccol_success);
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));

  ccol_event_loop_remove(loop, reg);
  /* This joins the poller thread and any dispatch workers. No callback can
     then still run with &ctx when the code below destroys the sync context.
     ccol_event_loop_remove alone does not wait for a dispatch that was
     already in flight at the time of the call. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_event_loop, pause_write_direction) {
  /* This has the same "pause before anything can fire" shape as
   * pause_stops_delivery_then_resume_restores_it above. The reason to avoid
   * the race is the same. It matters even more here. A write end of a pipe
   * never stops being writable on its own. A wait for one delivery, and then
   * a pause before a second one, therefore races a condition that re-fires
   * without bound. It does not race a one-shot event.
   *
   * A write end of a pipe is writable from the instant it exists. That
   * includes the short window between the return of ccol_event_loop_add and
   * the very next line of this test, which calls ccol_event_loop_pause. The
   * read side above is different, because it is genuinely not ready until
   * the later write() call of this test. A dispatch that races ahead of the
   * pause call here is therefore a real possibility, not only a theoretical
   * one. The assertions below are relative to a baseline. The count must not
   * advance past whatever it already was at the instant pause() returned.
   * This test therefore passes deterministically on either path. It does not
   * assume that the count is exactly 0 at that point. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  ccol_event_loop_construct_scoped(loop, 8, 4, 2);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ccol_event_handlers_t handlers = {
      .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
  char *err = NULL;
  /* A write end of a pipe is writable the moment it has room, and it has
   * room at once. This test therefore needs no priming step. The
   * ccol_circq_writable test above is different. It fills the queue first,
   * so that write readiness means something there. */
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
  /* This joins the poller thread and any dispatch workers. No callback can
     then still run with &ctx when the code below destroys the sync context.
     ccol_event_loop_remove alone does not wait for a dispatch that was
     already in flight at the time of the call. */
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

  /* Nothing paused this registration. The call must succeed, must do
   * nothing, and must not disturb delivery. */
  REQUIRE_EQ(ccol_event_loop_resume(loop, reg), ccol_success);

  int val = 1;
  REQUIRE_EQ(write(pfd[1], &val, sizeof(val)), (ssize_t)sizeof(val));
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));

  ccol_event_loop_remove(loop, reg);
  /* This joins the poller thread and any dispatch workers. No callback can
     then still run with &ctx when the code below destroys the sync context.
     ccol_event_loop_remove alone does not wait for a dispatch that was
     already in flight at the time of the call. */
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

  /* This is the same fd-only restriction that ccol_event_loop_modify has.
   * The bridge eventfd of a queue registration or a ccol_channel
   * registration has no use case that is worth this. Such a case would stop
   * caring for a time and still keep the registration. */
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
  /* This mirrors the exact timing of
   * fd_modify_after_remove_returns_invalid_args_multi_stripe. A remove is
   * followed at once by the operation under test, and nothing forces a
   * drain. reg stays valid, stale memory only across the short window before
   * the next reclamation drain claims it. ccol_event_loop_resume must
   * therefore key its stripe lookup off the stripe_idx of reg. It must
   * report ccol_invalid_args cleanly, and it must never crash. */
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
  /* This pins the documented contract exactly. A SECOND
   * ccol_event_loop_remove() call on the same reg is not a success that does
   * nothing. That holds when the call runs strictly after an earlier one
   * already returned. The earlier call already released the slot of reg
   * before it returned. _ccol_event_reg_slot_release does that, and the
   * successful-removal path always calls it before the function returns.
   * reg_h therefore does not resolve at all by the time this second, purely
   * sequential call runs. The call reports ccol_invalid_args instead.
   * ccol_event_loop_modify(), ccol_event_loop_pause(),
   * ccol_event_loop_resume() and ccol_event_loop_reg_generation() already
   * give the same outcome for a reg that is already removed. Only a call
   * that genuinely RACES the first one can see ccol_success from a second
   * removal. Such a call resolves reg before the slot release of that first
   * call. See reg_handle_survives_concurrent_remove_vs_accessor_race below
   * for that separate, narrower window. */
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
  /* A pause and a resume must never look like a remove and then an add to
   * any observer outside the library. The registration itself never goes
   * away. Both of the identities that a caller can see must therefore stay
   * exactly the same across a pause and resume cycle. */
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

/* The kernel always reports EPOLLERR and EPOLLHUP. It does so whatever the
 * registered interest mask is, and even a mask of 0 still gets them. A
 * registration that ccol_event_loop_pause pauses can therefore be fully
 * silenced in only one way. The library removes its fd from the epoll set
 * outright. This matters when the fd enters an error or hangup condition, or
 * is already in one. Otherwise level-triggered epoll_wait reports that
 * condition on every single call, forever. The re-check of reg->paused
 * correctly skips the callback itself, but nothing stops the reactor from
 * observing and collecting the condition again. That is a silent CPU-spin
 * busy loop with no bound. It directly contradicts the documented contract
 * of ccol_event_loop_pause, which says "no callback fires, exactly as if it
 * had been removed". A registration that is really removed produces zero
 * further wakeups, because of EPOLL_CTL_DEL. A paused one whose mask is only
 * narrowed cannot do that, because the interest mask cannot turn ERR and HUP
 * off. _ccol_event_loop_rearm_entry_locked therefore removes the fd from the
 * epoll interest set with EPOLL_CTL_DEL whenever every live direction on it
 * is paused. The event_entry.epoll_added field tracks that. A fd removed
 * this way must be re-added with EPOLL_CTL_ADD, and not with EPOLL_CTL_MOD,
 * once some direction wants real interest again.
 *
 * This test measures with ccol_event_loop_poller_iterations_for_tests, which
 * is a direct count of completed epoll_wait calls. It does not measure
 * wall-clock time or CPU time. With an unguarded pause, this counter races
 * into the thousands inside the sleep window below. With this handling in
 * place, the poller stays genuinely blocked in epoll_wait for the whole
 * window, because nothing else is registered with this loop. The count
 * therefore barely moves. This test is not vacuous. Remove the DEL and ADD
 * logic that epoll_added drives. The delta then grows orders of magnitude
 * larger than the bound below. */
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

  /* This puts the fd into a persistent, level-triggered hangup condition
   * while it is paused. A close of the write end makes the read end report
   * EPOLLHUP. */
  close(pfd[1]);

  /* This gives an unguarded busy loop a real chance to run away before the
   * first sample. The test then measures the delta across a further, wide
   * window. */
  usleep(50000);
  uint64_t before = ccol_event_loop_poller_iterations_for_tests(loop);
  usleep(150000);
  uint64_t after = ccol_event_loop_poller_iterations_for_tests(loop);

  REQUIRE_LT(after - before, (uint64_t)20);

  pthread_mutex_lock(&ctx.mtx);
  REQUIRE_EQ(ctx.readable_count, 0);
  REQUIRE_EQ(ctx.error_count, 0);
  pthread_mutex_unlock(&ctx.mtx);

  ccol_event_loop_remove(loop, reg);
  /* This joins the poller thread and any dispatch workers. No callback can
     then still run with &ctx when the code below destroys the sync context.
     ccol_event_loop_remove alone does not wait for a dispatch that was
     already in flight at the time of the call. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
}

/* This is the multi-threaded reactor counterpart of the test above. With a
 * dispatch_pool present, the fully-paused mask holds EPOLLONESHOT alone. It
 * still holds no real interest bits. The spin therefore takes the shape of a
 * continuous epoll_wait -> ctpool submit -> worker dequeue -> skip -> re-arm
 * cycle, and not a tight single-thread loop. It is just as unbounded. The
 * same poller_iterations_for_tests counter catches it in the same way. That
 * counter counts completed epoll_wait calls on the one and only poller
 * thread, whatever num_reactor_threads is. */
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
  uint64_t before = ccol_event_loop_poller_iterations_for_tests(loop);
  usleep(150000);
  uint64_t after = ccol_event_loop_poller_iterations_for_tests(loop);

  REQUIRE_LT(after - before, (uint64_t)20);

  pthread_mutex_lock(&ctx.mtx);
  REQUIRE_EQ(ctx.readable_count, 0);
  REQUIRE_EQ(ctx.error_count, 0);
  pthread_mutex_unlock(&ctx.mtx);

  ccol_event_loop_remove(loop, reg);
  /* This joins the poller thread and any dispatch workers. No callback can
     then still run with &ctx when the code below destroys the sync context.
     ccol_event_loop_remove alone does not wait for a dispatch that was
     already in flight at the time of the call. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
}

/* This is the companion of the two spin-regression tests above. It covers
 * the other half of the same mechanism. EPOLL_CTL_DEL removes a fully-paused
 * fd from the epoll interest set once every direction on it is paused. A
 * resume must then re-add that fd with EPOLL_CTL_ADD, and not with
 * EPOLL_CTL_MOD. A MOD on a fd that is not currently registered fails with
 * ENOENT. See the field comment of event_entry.epoll_added. This test
 * confirms that the library correctly observes and dispatches the hangup
 * condition, which is still there, once the resume happens. It proves that
 * the re-add path really works. Without it, the fd stays silently
 * unregistered. */
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
  /* This lets the reactor settle with the fd fully de-registered, before the
   * resume below. */
  usleep(100000);

  REQUIRE_EQ(ccol_event_loop_resume(loop, reg), ccol_success);

  /* An empty pipe whose write end is closed reports EPOLLHUP alone. A
   * standalone epoll_ctl and epoll_wait probe against this kernel reports
   * exactly that. No EPOLLIN or EPOLLRDHUP bit comes with it here. The
   * socket case where the peer writes and then closes is different. The
   * has_reader comment of _ccol_event_loop_handle_event documents that case.
   * This therefore dispatches as an error event, and not as a readable
   * event. */
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.error_count, 1, 2000));

  ccol_event_loop_remove(loop, reg);
  /* This joins the poller thread and any dispatch workers. No callback can
     then still run with &ctx when the code below destroys the sync context.
     ccol_event_loop_remove alone does not wait for a dispatch that was
     already in flight at the time of the call. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
}

typedef struct {
  evl_sync_ctx *ctx;
  ccol_event_loop loop;
  /* This field is _Atomic for one reason. reg is known only once
   * ccol_event_loop_add below returns. The code therefore publishes it to
   * this struct AFTER the registration is already live, and another thread
   * can in principle dispatch it by then. In the exact sequence of this test
   * the callback can never observe reg unset. The fd genuinely has nothing
   * to read until the later write() call of this test, which comes strictly
   * after the publish of reg. But a plain struct field read on one thread is
   * still a real data race under the C memory model. That read has no
   * happens-before edge to the write on the other thread. The timing
   * guarantee from the data does not change it. ThreadSanitizer reports this
   * race. A read of the code alone does not show it. An atomic store and
   * load builds the missing synchronization. It changes no real
   * behavior. */
  _Atomic(ccol_event_reg) reg;
} evl_pause_from_callback_args;

/* This callback pauses its own registration from inside itself. It matches
 * the _conn_start_diverted -> ccol_event_loop_pause call site of
 * chttpserver.c exactly. That call site runs synchronously from inside an
 * on_readable dispatch that is in flight. */
static void evl_on_readable_pause_self(ccol_event_loop loop, ccol_event_reg reg,
                                       ccol_selectable *sel, void *arg) {
  evl_pause_from_callback_args *a = (evl_pause_from_callback_args *)arg;
  /* This uses assert() and not REQUIRE_EQ. The comment of
   * evl_on_readable_self_remove above says why a call to a Tau assertion
   * macro from such a callback is unsafe. That callback runs on the reactor
   * thread or the dispatch thread of ccol_event_loop. It runs at the same
   * time as the REQUIRE_* calls of the main test thread. */
  assert(reg == atomic_load(&a->reg));
  assert(ccol_event_loop_pause(a->loop, reg) == ccol_success);
  evl_on_readable(loop, reg, sel, a->ctx);
}

TEST(ccol_event_loop,
     pause_from_within_callback_then_resume_from_another_thread) {
  /* This is the exact pattern that chttpserver.c depends on. on_readable
   * pauses its own registration synchronously from inside the dispatch
   * callback, which mirrors _conn_start_diverted. A completely separate
   * thread then resumes it later. That thread mirrors the worker_pool of
   * chttpserver, which is separate from the internal reactor threads and
   * dispatch threads of ccol_event_loop. */
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

  /* The callback above paused this registration. The test now drains the fd
   * and writes to it again. This proves that no further delivery happens
   * while the registration is paused. The single-thread pause test does
   * exactly the same. */
  int drain;
  REQUIRE_EQ(read(pfd[0], &drain, sizeof(drain)), (ssize_t)sizeof(drain));
  REQUIRE_EQ(write(pfd[1], &val, sizeof(val)), (ssize_t)sizeof(val));
  REQUIRE_FALSE(evl_wait_for(&ctx, &ctx.readable_count, 2, 300));

  /* This resume runs on the thread of the TEST itself. It does not run on
   * an internal thread of ccol_event_loop, which is the poller or a
   * dispatch_pool worker. */
  REQUIRE_EQ(ccol_event_loop_resume(loop, reg), ccol_success);
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 2, 2000));

  ccol_event_loop_remove(loop, reg);
  /* This joins the poller thread and any dispatch workers. No callback can
     then still run with &ctx when the code below destroys the sync context.
     ccol_event_loop_remove alone does not wait for a dispatch that was
     already in flight at the time of the call. */
  ccol_event_loop_shutdown(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

/* A callback of a registration that runs before ccol_event_loop_add has
 * returned still receives the handle of its own registration, and it can
 * remove that registration with it. The add hook of this file makes the
 * first dispatch run while ccol_event_loop_add is still inside the call, so
 * the handle that the adding thread stores afterwards is not yet there. */
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

/* Runs inside ccol_event_loop_add. It waits, bounded at ten seconds, until
 * the first dispatch of the new registration has finished. */
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
    ccol_event_loop_test_set_add_before_return_hook(evl_early_reg_add_hook);
    reg = ccol_event_loop_add(
        loop, ccol_selectable_from_fd(pfd[0], ccol_select_read),
        (ccol_event_handlers_t){.on_readable = evl_early_reg_on_readable,
                                .on_removed = evl_early_reg_on_removed},
        &ctx, NULL);
    ccol_event_loop_test_set_add_before_return_hook(NULL);
    atomic_store(&g_evl_early_reg_ctx, NULL);
    atomic_store(&ctx.returned_reg, reg);
    /* on_removed runs asynchronously after the removal; wait for it,
     * bounded at ten seconds. */
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
  /* The dispatch ran before ccol_event_loop_add returned: the handle that
   * the adding thread stores afterwards was still unset at that moment. */
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
  /* The test opens this once before the loop of this worker, and closes it
   * once after that loop. It does not do so for each iteration. A close for
   * each iteration would race two calls. The loop, and the other 7 workers
   * that run at the same time, all stay alive. A legitimate stale dispatch
   * of the kind that a thundering herd produces can then call read() on this
   * fd while this worker calls close(). The comment of
   * multi_thread_fd_reuse_generation_stays_consistent documents that same
   * close()-against-read() hazard, which ThreadSanitizer reports. */
  int pfd[2];
  /* The test logs the ctx of each iteration here instead of a destroy in
   * place. The identical field of evl_reuse_driver_args has the same reason.
   * ccol_event_loop_remove can return while a callback for the removed reg
   * is still in flight. A destroy of ctx.mtx right after remove() returns
   * can therefore race that callback. A reuse of the stack slot of ctx on
   * the next iteration can race it too. This is a real race. It is present
   * even with a single reactor thread, and nothing about it needs dispatch
   * on many threads. ThreadSanitizer reports it here. The TEST function
   * frees these only after it joins the whole loop and every worker
   * thread. */
  evl_sync_ctx **ctx_log;
  int ctx_log_count;
  /* This field is for the queue worker only. It holds the
   * ccol_circular_queue that pairs with ctx_log[j] at the same index j. The
   * test defers it and frees it beside that ctx, for the identical reason.
   * See the comment of ctx_log above, and how evl_stripe_stress_queue_worker
   * uses this field. The fd worker does not use it and leaves it NULL. */
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
  /* This test uses many threads and many DISTINCT fds.
   * high_add_remove_churn_stress is different, because it is
   * single-threaded and has a stripe count of 1. num_lock_stripes here is
   * well above 1. The test therefore exercises genuinely concurrent
   * ccol_event_loop_add, ccol_event_loop_remove and dispatch calls across
   * different stripes that run in parallel. It is not only churn on a single
   * lock. */
  const int n_threads = 8;
  const int iterations = 25;
  pthread_t threads[8];
  evl_stripe_stress_args args[8];

  for (int i = 0; i < n_threads; i++) {
    REQUIRE_EQ(pipe(args[i].pfd), 0);
    /* This fd is non-blocking. See the comment of evl_set_nonblocking. A
     * legitimate duplicate or stale dispatch can read this fd after this
     * worker already moves on to a later iteration. Such a read must not
     * block forever. */
    evl_set_nonblocking(args[i].pfd[0]);
    args[i].thread_id = i;
    args[i].iterations = iterations;
    args[i].ctx_log = malloc(sizeof(evl_sync_ctx *) * (size_t)iterations);
    args[i].ctx_log_count = 0;
  }

  bool all_started = false;
  size_t residual_regs = 0;
  {
    /* This is a nested block. The other multi-thread tests use the same
     * pattern. The destructor of this loop joins every reactor thread at the
     * closing brace of this block. That join is what makes it safe to free
     * every logged ctx below. */
    ccol_event_loop_construct_scoped(loop, 32, 16, 1);
    int started = 0;
    for (int i = 0; i < n_threads; i++) {
      args[i].loop = loop;
      /* This loop counts the failures. It does not assert inside itself. A
       * REQUIRE_* here returns from the test while the threads that earlier
       * iterations created keep running. Those threads keep using args[] and
       * `loop`, whose scoped destructor runs on that very return. The test
       * joins only the threads that really started. It checks the count once
       * every one of them is back. */
      if (pthread_create(&threads[i], NULL, evl_stripe_stress_fd_worker,
                         &args[i]) != 0)
        break;
      started++;
    }
    for (int i = 0; i < started; i++) {
      pthread_join(threads[i], NULL);
    }

    /* The test captures these values here and does not assert on them yet.
       An assertion that fires returns from the test at once. Everything
       below this block still has pipes to close and logs of each thread to
       free. The test must read the registration count while the loop is
       still alive, which is why that read stays inside the block. */
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
      /* The worker logs these and does not destroy them in place.
       * ccol_event_loop_remove can return while a callback for the removed
       * reg is still in flight. See the field comments of ctx_log and
       * cq_log in evl_stripe_stress_args. A destroy of the mutex and the
       * condition variable of ctx right after remove() returns can race that
       * callback. A free of cq at that point can race it too.
       * evl_stripe_stress_fd_worker uses the identical deferred-cleanup
       * pattern. The TEST function frees these only after it joins the whole
       * loop and every worker thread. */
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
  /* This is the queue-selectable equivalent of the fd stress test above. It
   * uses many distinct queues, and therefore many distinct bridge_efds. The
   * loop assigns each one to a stripe by round robin, through
   * loop->next_queue_stripe. Threads register and remove them at the same
   * time. */
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
    /* This is a nested block. The other multi-thread tests use the same
     * pattern. The destructor of this loop joins every reactor thread at the
     * closing brace of this block. That join is what makes it safe to free
     * every logged ctx and cq below. */
    ccol_event_loop_construct_scoped(loop, 32, 16, 1);
    int started = 0;
    for (int i = 0; i < n_threads; i++) {
      args[i].loop = loop;
      /* This loop counts the failures. It does not assert inside itself. A
       * REQUIRE_* here returns from the test while the threads that earlier
       * iterations created keep running. Those threads keep using args[] and
       * `loop`, whose scoped destructor runs on that very return. The test
       * joins only the threads that really started. It checks the count once
       * every one of them is back. */
      if (pthread_create(&threads[i], NULL, evl_stripe_stress_queue_worker,
                         &args[i]) != 0)
        break;
      started++;
    }
    for (int i = 0; i < started; i++) {
      pthread_join(threads[i], NULL);
    }

    /* The test captures these values here and does not assert on them yet.
       An assertion that fires returns from the test at once. Everything
       below this block still has pipes to close and logs of each thread to
       free. The test must read the registration count while the loop is
       still alive, which is why that read stays inside the block. */
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
  /* This registers enough distinct fds and queues, with num_lock_stripes >
   * 1, to spread the live registrations across several stripes. It then
   * destroys the loop WITHOUT a removal first. This exercises the walk that
   * __ccol_event_loop_destroy does for each stripe. That walk covers the
   * fd_index chmap and the unlink of queue_regs_head. It must run for every
   * stripe, and not only for stripe 0. */
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

    /* The scope exit here destroys loop while every registration is still
     * live. __ccol_event_loop_destroy must walk all of them across every
     * stripe and free them. */
  }

  for (int i = 0; i < n; i++) {
    close(pfds[i][0]);
    close(pfds[i][1]);
    ccol_circular_queue_destroy(queues[i]);
  }
}

/* Multi-threaded reactor (num_reactor_threads > 1) tests below */

TEST(ccol_event_loop, multi_thread_basic_smoke) {
  /* This is a plain single-fd readable dispatch, but with several reactor
   * threads that share the epoll instance. It confirms that ordinary
   * dispatch still works correctly once more than one thread calls
   * epoll_wait on the same epfd. It checks more than "the test does not
   * crash". */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  {
    /* This is a nested block. The other multi-thread tests below use the
     * same reasoning. evl_sync_ctx_destroy assumes that no callback can
     * still touch ctx by the time it runs. Nothing else in this test
     * establishes that. The count of reactor threads does not matter here.
     * Even with one, the poller is a thread of its own. And
     * ccol_event_loop_remove explicitly does not wait for a dispatch that is
     * already in flight when the caller calls it. The join is what makes
     * this safe, so ctx must outlive the loop. The scope exit of this block
     * gives that join. An explicit ccol_event_loop_shutdown before the
     * teardown gives it too. */
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

/* This callback deliberately holds the region that entry->dispatch_lock
 * protects open for a moment. It sleeps after it checks and updates
 * `active`. That widens the window for a second reactor thread. Such a
 * thread can receive this same still-ready fd from its own concurrent
 * epoll_wait call, and enter this callback at the same time. The default
 * level-triggered, non-EPOLLEXCLUSIVE semantics of epoll genuinely allow
 * that. Only a dispatch_lock that is broken or absent lets it happen. */
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
    (void)n; /* A full pipe buffer that blocks for a short time is fine here,
              * and even wanted. It keeps the read end ready all the time.
              * The concurrent epoll_wait calls of several reactor threads
              * then have the best chance to all observe it ready at once. */
  }
  return NULL;
}

TEST(ccol_event_loop, multi_thread_no_double_dispatch_same_fd) {
  /* This test uses many reactor threads and ONE hot fd. A writer thread
   * feeds that fd all the time. This is exactly the thundering-herd scenario
   * that entry->dispatch_lock exists to close. See the field comment of that
   * lock in cthreadcomm.c. Without that lock, this test reliably catches
   * active > 1 under stress. With it, active never goes above 1, however
   * many reactor threads race for the same entry. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  /* This fd is non-blocking. The callback below does its own read() call
   * directly, which copies real use under genuinely concurrent dispatch.
   * Once the feeder thread stops, a legitimate thundering-herd duplicate
   * dispatch can read an already-drained, permanently quiet pipe. A blocking
   * read there would block forever and deadlock the reactor thread that ran
   * it. See the comment of evl_set_nonblocking. */
  evl_set_nonblocking(pfd[0]);

  evl_no_double_dispatch_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  assert(pthread_mutex_init(&ctx.mtx, NULL) == 0);

  {
    /* This is a nested block. multi_thread_cross_direction_serialization
     * uses the identical pattern and comment. The test must not destroy ctx,
     * and must not close pfd, until it really joins every reactor thread.
     * The closing brace of this block does that join. A grace period that
     * only guesses is not enough. */
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

    /* This gives the reactor threads a short grace period. In it they finish
     * the drain of whatever is left in the pipe after the feeder stops. Its
     * only purpose is to let total_calls come close to
     * feeder_args.iterations before the assertion below. Safety does not
     * depend on it. The join of the nested block gives that. */
    for (int spins = 0;
         spins < 400 && atomic_load(&ctx.total_calls) < feeder_args.iterations;
         spins++) {
      struct timespec ts = {0, 2000000}; /* 2ms */
      nanosleep(&ts, NULL);
    }

    ccol_event_loop_remove(loop, reg);

    /* The block exit here shuts down loop and joins every reactor thread.
     * ctx and pfd are quiet from this point on. */
  }

  REQUIRE_FALSE(ctx.violation);
  REQUIRE_GT(atomic_load(&ctx.total_calls), 0);

  pthread_mutex_destroy(&ctx.mtx);
  close(pfd[0]);
  close(pfd[1]);
}

/* This uses the same active-counter protocol as evl_no_double_dispatch_ctx.
 * But a read-direction callback and a write-direction callback on the SAME
 * fd share it. This verifies the stricter cross-direction guarantee of
 * entry->dispatch_lock. That guarantee covers more than self-exclusion. */
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
   * independently. The write direction of sv[0] stays ready forever. Nothing
   * ever fills its send buffer, because nothing here writes from sv[0] to
   * sv[1]. A peer thread feeds sv[1] all the time, which keeps the read
   * direction of sv[0] ready too. Both directions are therefore genuinely
   * dispatchable at the same time, across many reactor threads, for the
   * whole test. */
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  /* This fd is non-blocking for the same reason as pfd[0] of
   * multi_thread_no_double_dispatch_same_fd. See the comment of
   * evl_set_nonblocking. */
  evl_set_nonblocking(sv[0]);

  evl_no_double_dispatch_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  assert(pthread_mutex_init(&ctx.mtx, NULL) == 0);

  {
    /* This is a nested block. The destructor of
     * ccol_event_loop_construct_scoped fires at the closing brace of this
     * block. It shuts down AND JOINS every reactor thread before control
     * leaves the block. That join is a real guarantee, and not a guess. No
     * callback that references ctx can still run after it. A grace period
     * built on a fixed nanosleep is different. ThreadSanitizer still catches
     * a real race through such a period. The documented contract of
     * ccol_event_loop_remove guarantees only that an in-flight callback for
     * the reg under removal finishes. It does not guarantee that no OTHER
     * dispatch for the same, still-live entry is running. Another reactor
     * thread can collect such a dispatch independently. That is a
     * legitimate, expected thundering-herd duplicate.
     * multi_thread_no_double_dispatch_same_fd exists to prove that the
     * library serializes those duplicates with a lock, and does not remove
     * them. A destroy of ctx.mtx before that join is exactly the hazard that
     * this nested block avoids. A return from this function before the join
     * is the same hazard, because it frees the stack slot of ctx. */
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

    /* The block exit here shuts down loop and joins every reactor thread.
     * ctx is quiet from this point on. */
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
  /* A stale batch entry can come from a PREVIOUS registration that the test
   * already removed, on a recycled fd number. If such an entry ever
   * misdispatches into this callback with the WRONG pairing of ctx and reg,
   * this check observes a generation mismatch. */
  if (ccol_event_loop_reg_generation(loop, c->reg) != c->expected_generation) {
    atomic_fetch_add(&c->mismatch_count, 1);
  }
  atomic_fetch_add(&c->call_count, 1);
}

typedef struct evl_reuse_driver_args {
  ccol_event_loop loop;
  int iterations;
  _Atomic int mismatch_total;
  /* The driver logs the ctx of each iteration here instead of a free in
   * place. The documented contract of ccol_event_loop_remove says that the
   * call is "safe to call concurrently with an in-flight dispatch". It also
   * says that "teardown is deferred until any in-progress callback returns".
   * It does NOT promise that no more callback calls for this reg can be in
   * flight by the time remove() returns. A second reactor thread can collect
   * this same, still-registered entry from its own epoll_wait batch. That is
   * the documented thundering-herd behavior that other tests in this file
   * also exercise. A free of ctx in place would race a stale callback that
   * still runs. A reuse of its memory on the next iteration would race it
   * too. The reads and writes of that callback would meet the writes and
   * frees of this iteration. Two other designs are not acceptable here. A
   * ctx that nothing ever frees makes valgrind report definitely-lost. A
   * grace-period sleep of a fixed length makes ThreadSanitizer report a real
   * use-after-free. Only a real join gives the guarantee. The test function
   * therefore frees every logged ctx only after it shuts down and joins the
   * whole ccol_event_loop, which means every reactor thread. The nested-block
   * comment of that test says why a join is a real guarantee and a sleep is
   * not. */
  evl_reuse_ctx **ctx_log;
  int ctx_log_count;
  /* This fd stays open for the ENTIRE run of this driver. The test does not
   * close it and open it again for each iteration. A re-add of the SAME
   * still-open fd after a removal still mints a fresh event_entry, and
   * therefore a fresh generation, every time. ccol_event_loop_remove deletes
   * the registry entry of the fd before it returns. A later
   * ccol_event_loop_add on that same fd number therefore always takes the
   * new-entry path. That is enough to exercise the core generation guarantee
   * of this module. It does so under heavy concurrent churn of add, remove,
   * dispatch and reclaim calls, across drivers that share one
   * ccol_event_loop. It also avoids a close() call on a fd that a legitimate
   * thundering-herd duplicate dispatch can still be reading. Such a dispatch
   * is rare. A close for each iteration produces exactly that
   * close()-against-read() race, and ThreadSanitizer reports it. */
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
  /* This test runs several driver threads at the same time. Each one
   * registers, writes and removes against its own fd in a tight loop. They
   * all share one ccol_event_loop. This is exactly the high-churn workload
   * of add, remove, dispatch and reclaim calls under which the library must
   * mint a fresh generation every single time. Every dispatch must also
   * observe that generation correctly. This is the core guarantee that
   * ccol_event_loop_reg_generation exists to make safe by construction. See
   * its own doc comment. The bug CLASS is stale identity confusion across
   * the registration lifecycle of one fd. A hand-off that reuses a fd across
   * a redirect faces the same class. This test exercises it as repeated
   * re-registration of the same fd under concurrent load. It does not use a
   * real close and reopen at the OS level. That choice avoids a race between
   * a legitimate thundering-herd duplicate dispatch and a close() call. See
   * the comment of evl_reuse_driver_args. The test asserts that every
   * dispatch observed the generation of its OWN registration. No dispatch
   * ever observed a stale or mismatched one. */
  const int n_drivers = 4;
  const int iterations = 150;
  pthread_t drivers[4];
  evl_reuse_driver_args args[4];

  for (int i = 0; i < n_drivers; i++) {
    REQUIRE_EQ(pipe(args[i].pfd), 0);
    /* This fd is non-blocking for the same reason as pfd[0] of
     * multi_thread_no_double_dispatch_same_fd. See the comment of
     * evl_set_nonblocking. Here it also covers one more window. That window
     * starts when the last iteration of a driver removes its registration,
     * and it ends when the whole loop is torn down. */
    evl_set_nonblocking(args[i].pfd[0]);
    args[i].iterations = iterations;
    atomic_init(&args[i].mismatch_total, 0);
    args[i].ctx_log = malloc(sizeof(evl_reuse_ctx *) * (size_t)iterations);
    args[i].ctx_log_count = 0;
  }

  bool all_started = false;
  {
    /* This is a nested block. multi_thread_cross_direction_serialization
     * uses the identical pattern and comment. The destructor of this loop
     * shuts down and joins every reactor thread at the closing brace of this
     * block. That join is what makes it safe to free every logged ctx below.
     * A guess at the timing is not enough. */
    ccol_event_loop_construct_scoped(loop, 8, 4, 8);
    int started = 0;
    for (int i = 0; i < n_drivers; i++) {
      args[i].loop = loop;
      /* This loop counts the failures. It does not assert inside itself. A
       * REQUIRE_* here returns from the test while the threads that earlier
       * iterations created keep running. Those threads keep using args[] and
       * `loop`, whose scoped destructor runs on that very return. The test
       * joins only the threads that really started. It checks the count once
       * every one of them is back. */
      if (pthread_create(&drivers[i], NULL, evl_reuse_driver_thread,
                         &args[i]) != 0)
        break;
      started++;
    }
    for (int i = 0; i < started; i++) {
      pthread_join(drivers[i], NULL);
    }

    /* The fd stress test above says why the test captures this value here
       instead of an assertion. */
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
  /* This test configures many reactor threads and registers nothing. Only
   * ONE thread, poller_thread, ever blocks in epoll_wait here. The rest are
   * idle ctpool workers with nothing queued. See
   * multi_thread_shutdown_drains_idle_dispatch_pool_promptly below for that
   * half. The single write() that ccol_event_loop_shutdown makes to
   * shutdown_efd must still wake poller_thread and join it promptly. Nothing
   * ever drains shutdown_efd, deliberately. The comment of
   * ccol_event_loop_shutdown says which hang a drain of it causes. The
   * epoll_wait call of poller_thread therefore keeps seeing it ready,
   * however long that thread takes to return and observe shutting_down. This
   * test bounds how long the shutdown may take. It does not only assert that
   * the shutdown returns in the end. A regression that leaves poller_thread
   * stuck therefore shows up as a slow or hung test, and not as a silent
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
  /* This is the other half of the shutdown. With num_reactor_threads > 1,
   * ccol_event_loop_shutdown also calls ctpool_shutdown_drain on
   * dispatch_pool. That is a completely different mechanism from the
   * poller-wake path above. It is a condition-variable broadcast that wakes
   * idle ctpool workers, and not an epoll_wait wakeup. This test bounds how
   * long that call takes with a large worker pool that is fully idle. The
   * test above bounds the poller half in the same way. A regression that
   * leaves some idle worker un-woken shows up here as a slow or hung
   * test. */
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

/* This callback signals "started" the moment it begins to run. It then
 * sleeps, and only after that it signals "finished". This gives the test
 * driver a reliable way to call ccol_event_loop_shutdown while this job is
 * provably still in flight. Without it, the shutdown races the poller, which
 * may not even notice the write that triggers the callback. */
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
  /* The two tests above use an idle pool. This one puts a real job in
   * flight. Its callback sleeps on purpose, and it synchronizes with the
   * test driver. The driver therefore knows that the callback genuinely
   * started before it calls ccol_event_loop_shutdown. This directly
   * exercises the "finish in-flight work" contract of
   * ctpool_shutdown_drain. The module chooses that call over
   * ctpool_shutdown_immediate to keep the documented guarantee of
   * ccol_event_loop_shutdown. That guarantee says that no dispatch can be in
   * flight once the call returns. See the comment of that function. This test
   * asserts that the callback really completed. It does not only assert that
   * the shutdown returned. Immediate-cancel semantics would not guarantee
   * that. */
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
  /* A NULL reg reads as generation 0. That value is reserved, and the
   * library never mints it for a real registration. */
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
    /* This is a nested block. The other multi-thread tests in this file use
     * the same reasoning. The write direction of wreg dispatches almost at
     * once, because a fresh pipe write end is always writable. This loop has
     * 4 reactor threads. evl_sync_ctx_destroy must therefore not run until
     * that dispatch, and any other one still in flight, is guaranteed to be
     * finished. This function must also not return before then, because that
     * return frees the stack slot of ctx. Only the join of this block gives
     * that guarantee. */
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

    /* A flip of the direction with ccol_event_loop_modify keeps the same
     * generation. It is the same fd and the same connection below it, with
     * only a different direction. */
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
 * in its loop, as fast as possible, for the whole run of
 * reg_handle_survives_concurrent_remove_vs_accessor_race below. That loop is
 * epoll_wait returning, poller_batch_gen advancing, and epoll_wait being
 * entered again. A write end of a pipe is writable from the instant it
 * exists, and it stays that way forever. A few of them registered for write
 * interest therefore keep the reactor busy. No second thread has to feed
 * it. */
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
  /* This thread genuinely races the ccol_event_loop_remove call on the other
   * thread, for the exact same reg. Any return value from any of these five
   * calls is acceptable. That means ccol_success, ccol_not_permitted,
   * ccol_invalid_args, or generation 0. The value depends only on which
   * thread the scheduler lets win. This test verifies something else. None
   * of these calls must ever touch memory that the reclaimer already freed
   * under them. A clean run shows that, and a clean `make memtest` run shows
   * it best. See the field comment of reg_slots in struct
   * ccol_event_loop_s. */
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
   * dereference a reg pointer from the caller directly. That starts with
   * reg->stripe_idx, which only tells the call which stripe lock would
   * protect the rest of it. Such a dereference has no protection against a
   * concurrent poller reclaim of that exact reg. The reclaim can complete on
   * the very next iteration of the poller loop, after a DIFFERENT thread
   * deferred it with ccol_event_loop_remove(). That is a real use-after-free
   * for two threads that race on the same reg. It is not only a theoretical
   * concern. ccol_event_reg is therefore an opaque, generation-checked VALUE
   * handle. The library resolves it through the registration table of its
   * own loop before it dereferences anything. This mirrors how the handle of
   * ccol_event_loop itself works. See the field comment of reg_slots in
   * struct ccol_event_loop_s for the full design.
   *
   * A few always-ready write-direction "hot" registrations keep the poller
   * cycling all the way through this test. That maximises how often an
   * unprotected window is really exercised. The test then races many
   * short-lived fd registrations, back to back. One thread removes each one.
   * A second thread calls every other reg-accessor entry point on that exact
   * same reg at the same time. This is the most direct way to catch a
   * regression here. Run it under `make memtest`. An unprotected dereference
   * then reliably reports a real use-after-free that valgrind detects. It is
   * not only a failure that happens now and then. */
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

/* This callback is deliberately slower than a feeder that refills all the
 * time. That maximises the chances for the poller to come back to
 * epoll_wait. It can do so while a dispatch job for this exact fd is still
 * queued or still running. That is exactly the scenario that mints an
 * unbounded stream of redundant dispatch jobs. Only the
 * re-arm-after-dispatch handling of EPOLLONESHOT stops it. See the comment
 * of _ccol_event_loop_poller_collect in cthreadcomm.c. */
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
  /* This is a direct regression test for the EPOLLONESHOT re-arm contract.
   * Collection runs on poller_thread and dispatch runs on a ctpool worker,
   * so the two are decoupled. A still-ready fd that nothing re-armed yet
   * must never be collected again. Without that rule, the pending-job count
   * of dispatch_pool grows without bound. It does so for as long as a slow
   * callback lags behind a fast feeder. This test samples
   * ccol_event_loop_dispatch_pool_pending_count_for_tests while a feeder
   * thread writes all the time. It asserts that the count never goes above a
   * small bound. That bound is well below num_reactor_threads - 1 workers
   * plus one more job queued. The livelock that this test guards against
   * goes far past that bound. It does not only push a little over it. */
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

    /* This count is deliberately modest. At about 0.5ms for each dispatch,
     * it fully drains inside the grace period below. That matches the
     * pattern of multi_thread_no_double_dispatch_same_fd. The window in
     * which the test samples while the feeder still runs is what stresses
     * the mechanism. The total byte count does not. */
    evl_feeder_args feeder_args = {.write_fd = pfd[1], .iterations = 1000};
    pthread_t feeder;
    REQUIRE_EQ(
        pthread_create(&feeder, NULL, evl_bounded_feeder_thread, &feeder_args),
        0);

    for (int i = 0; i < 200; i++) {
      size_t pending =
          ccol_event_loop_dispatch_pool_pending_count_for_tests(loop);
      if (pending > max_pending) max_pending = pending;
      struct timespec ts = {0, 1000000}; /* 1ms */
      nanosleep(&ts, NULL);
    }

    pthread_join(feeder, NULL);

    /* This is a short, best-effort settle time before any teardown. It is
     * test hygiene only. Nothing here asserts on pipe bytes that stay
     * unread. The queue version of this test below is different, because
     * ccol_circular_queue_destroy does assert there. The max_pending
     * assertion does not depend on this loop. The loop stays short, because
     * a full drain is not needed and is not worth the wall-clock time of
     * this test. */
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
    /* This send blocks. It is not ccol_circq_try_send_zc. A full queue must
     * throttle this feeder exactly as a full pipe buffer already throttles
     * the plain write() of evl_bounded_feeder_thread above. Both tests then
     * have the same guarantee, which is that every iteration is delivered in
     * the end. A try-send would silently drop the sends past the capacity.
     * ctx.calls would then never reliably reach iterations below. */
    (void)ccol_circq_send_zc(a->cq, &msg);
  }
  return NULL;
}

/* This has the same slow-callback shape as evl_bounded_hot_fd_on_readable.
 * It covers the path of the bridge eventfd and the queue selectable. The
 * design note in _ccol_event_loop_dispatch_job_fn marks this case as one
 * with NO natural throttle of its own. A bridge eventfd has no finite kernel
 * buffer, and a pipe does. This is therefore the more valuable of the two
 * regression tests. It is not a redundant mirror of the fd one. */
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
          ccol_event_loop_dispatch_pool_pending_count_for_tests(loop);
      if (pending > max_pending) max_pending = pending;
      struct timespec ts = {0, 1000000}; /* 1ms */
      nanosleep(&ts, NULL);
    }

    pthread_join(feeder, NULL);

    /* This is a best-effort grace period. Nothing asserts on it. A direct
     * comparison against num_reactor_threads == 1 shows one thing. A queue
     * reader with only ONE registration can legitimately finish with a few
     * messages still unconsumed once the feeder stops. That comes from the
     * notify-on-send coalescing of the bridge eventfd. It has nothing to do
     * with the EPOLLONESHOT coverage of this test. The direct drain below,
     * after the removal of the registration, is what guarantees that
     * ccol_circular_queue_destroy never asserts. This loop does not. */
    for (int spins = 0; spins < 500 && atomic_load(&ctx.calls) <
                                           (size_t)feeder_args.iterations;
         spins++) {
      struct timespec ts = {0, 2000000}; /* 2ms */
      nanosleep(&ts, NULL);
    }

    ccol_event_loop_remove(loop, reg);
  }

  /* This drains directly whatever the consumption above, which
   * ccol_event_loop drove, did not reach. See the comment of the
   * grace-period loop. It runs now, because the test already removed reg and
   * the scope of the loop already closed. ccol_circular_queue_destroy
   * asserts on any message that stays. See its own comment in
   * cthreadcomm.c. This drain is therefore needed for a clean teardown. It
   * is not optional hygiene. */
  c_message_t leftover = {.data = NULL, .size = 0};
  while (ccol_circq_try_recv_zc(cq, &leftover) == ccol_success) {
    /* nothing to free: every sent message here has data == NULL */
  }

  ccol_circular_queue_destroy(cq);
  REQUIRE_LT(max_pending, (size_t)10);
}

/* This is an on_readable handler that never returns on its own. It has one
 * use only. It holds a job in flight on a dispatch_pool worker for long
 * enough. For num_reactor_threads == 1 it occupies the sole thread instead.
 * The test driver can then reliably call ccol_event_loop_shutdown from
 * inside it. */
typedef struct evl_self_shutdown_ctx {
  ccol_event_loop loop;
  _Atomic int observed_rv; /* This holds a ccol_retval_t. It is _Atomic for
                            * one reason. The test driver polls it from the
                            * main thread while the callback writes it. That
                            * callback runs on poller_thread or on a
                            * dispatch_pool worker, and nothing else
                            * synchronizes the two. A plain ccol_retval_t
                            * field here is a real data race that
                            * ThreadSanitizer reports. */
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
  /* num_reactor_threads == 1 means that the sole thread both polls and
   * dispatches. Without the guard, a self-call here joins that thread to
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

  /* This is a bounded poll and not a call to evl_wait_for. This ctx has no
   * mutex and no condition variable of its own, deliberately. The test waits
   * on the default sentinel value of observed_rv instead. */
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
  /* num_reactor_threads > 1 means that the callback runs on a dispatch_pool
   * worker, and not on poller_thread. Without the guard, a self-call here
   * calls ctpool_shutdown_drain from inside one of the workers of that pool.
   * That worker then joins itself with ccol_thread_join. */
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
 * tests/chttpclient/tests.c. It is adapted for the fully lock-free pin
 * mechanism of ccol_event_loop. See the field comment of
 * pending_resolve_count in struct ccol_event_loop_s in src/cthreadcomm.c.
 * That comment says why the unpin side here is a bare atomic decrement, and
 * not the lock-protected one of chttpcli and chttpsvr. The comment of
 * __ccol_event_loop_destroy says why that makes destroy wait by a poll and
 * not by a condition variable. */

/* A destroy can complete fully. A second destroy call later, on a copy of
 * the same original handle value that the caller holds independently, must
 * be a fatal error. This test runs in a forked child, because
 * ccol_fatal_err aborts the whole process. tests/clogger/tests.c sets the
 * precedent for a fork test of misuse that stops the process. */
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
    _exit(0); /* not reachable when ccol_fatal_err() aborts, as it must */
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

/* A call to ccol_event_loop_destroy on a loop must be a fatal error in one
 * case. That case is a call from inside a callback that dispatches on a
 * thread of that same loop. That thread is the poller thread for
 * num_reactor_threads == 1, or a dispatch_pool worker for a larger count.
 * This matches the sequential and
 * concurrent double-destroy cases above. It must not be a teardown that the
 * library skips in silence. The internal ccol_event_loop_shutdown call of
 * __ccol_event_loop_destroy already detects and rejects a self-join here.
 * See shutdown_from_within_callback_returns_not_permitted_* above. That
 * alone is not enough. __ccol_event_loop_destroy has no way to pass that
 * rejection on through its own macro-driven contract, which returns void.
 * Without a guard of its own, it frees every live registration and the loop
 * struct itself under the callback that is still running. That is a real
 * use-after-free, and not only a deadlock. This test runs in a forked child,
 * because ccol_fatal_err aborts the whole process. */
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

    /* This bounds the lifetime of this child. It covers the case where
     * ccol_fatal_err somehow does not fire as expected. Without it, that
     * case hangs the whole suite. */
    struct timespec ts = {1, 0};
    nanosleep(&ts, NULL);
    _exit(0); /* not reachable when ccol_fatal_err() aborts, as it must */
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

    struct timespec ts = {1, 0};
    nanosleep(&ts, NULL);
    _exit(0); /* not reachable when ccol_fatal_err() aborts, as it must */
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

/* Two threads can call destroy on two copies of the SAME, still-valid
 * handle, which each thread holds on its own. They do so as close to the
 * same moment as possible. That must also be fatal. This covers the same
 * class of concurrent double free that the generation-tagged slot table
 * exists to close for chttpcli and chttpsvr. */
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
    /* These are if-guards and not REQUIRE_EQ. This code runs inside the
     * forked child above (pid == 0). The identical guard and comment on
     * cq_select_waiter_thread earlier in this file says why the early-return
     * failure path of REQUIRE_EQ is unsafe here. A join of a pthread_t that
     * pthread_create never initialized is undefined behavior. It can hang or
     * crash. Worse, it can give a stray SIGABRT from something unrelated,
     * and this test would then appear to pass for the wrong reason. These
     * guards exit with a distinct code instead. A failed create is therefore
     * reported as itself. Nothing mixes it up with the ccol_fatal_err() that
     * this test expects. */
    if (pthread_create(&t1, NULL, evl_concurrent_destroy_thread, &a1) != 0)
      _exit(3);
    if (pthread_create(&t2, NULL, evl_concurrent_destroy_thread, &a2) != 0)
      _exit(3);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    _exit(0); /* not reachable: whichever of the two destroy calls loses the
                  race must hit ccol_fatal_err() */
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
  int sleep_ms;
  bool resolved;
} evl_pin_sleep_arg_t;

static void *evl_pin_sleep_thread(void *arg) {
  evl_pin_sleep_arg_t *a = (evl_pin_sleep_arg_t *)arg;
  a->resolved =
      _ccol_event_loop_resolve_pin_and_sleep_for_tests(a->h, a->sleep_ms);
  return NULL;
}

/* This proves that destroy waits out a resolve that is in flight. It races
 * two things against each other on the same handle. One is a thread that
 * resolves and pins the handle for a deliberately long duration that the
 * test controls directly. It does that with
 * _ccol_event_loop_resolve_pin_and_sleep_for_tests. ccol_event_loop has no
 * naturally slow public entry point to borrow for this, where chttpcli has
 * chttpclient_do against a slow endpoint. The other is a concurrent
 * ccol_event_loop_destroy. destroy must block until something releases the
 * pin. It must not race ahead and free the loop under the pointer that is
 * still resolved. */
TEST(ccol_event_loop_handle_lifecycle, resolve_then_use_race_destroy_waits) {
  ccol_event_loop_construct(loop, 8, 1, 1);

  evl_pin_sleep_arg_t pin_arg = {.h = loop, .sleep_ms = 100, .resolved = false};
  pthread_t pin_thread;
  REQUIRE_EQ(pthread_create(&pin_thread, NULL, evl_pin_sleep_thread, &pin_arg),
             0);

  /* This gives the pin thread a short head start. Its resolve, and therefore
   * its pin, then definitely happens before destroy fires. */
  struct timespec startup = {.tv_sec = 0, .tv_nsec = 10000000}; /* 10 ms */
  nanosleep(&startup, NULL);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  ccol_event_loop_destroy(loop); /* This must block until the 100ms sleep of
            the pin thread fully elapses. That thread still holds the pin. */
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

  pthread_join(pin_thread, NULL);
  REQUIRE_TRUE(pin_arg.resolved);
  /* The pin thread slept about 100ms while it held the pin. A destroy that
   * returns well inside that time did NOT really wait for the pin. The
   * resolve-then-use protection has then failed. */
  REQUIRE_GT(elapsed_ms, 50L);
}

typedef struct {
  ccol_event_loop h;
} evl_reg_count_arg_t;

static void *evl_reg_count_thread(void *arg) {
  evl_reg_count_arg_t *a = (evl_reg_count_arg_t *)arg;
  /* This code ignores the return value on purpose. A legitimate race with a
   * concurrent destroy can make this resolve fail and return (size_t)-1
   * instead of a success. Both outcomes are correct. This thread exists only
   * to generate resolve, pin and unpin traffic at the same time as the
   * destroy thread below. */
  ccol_event_loop_reg_count(a->h);
  return NULL;
}

/* This test differs from resolve_then_use_race_destroy_waits above, and it
 * is not redundant. The long pin that the other test holds on purpose keeps
 * pending_resolve_count above 0 for the whole race window. The poll-wait of
 * destroy therefore always finds it nonzero on its first check there. This
 * test needs the opposite shape. It races a fast, non-blocking entry point
 * against a concurrent destroy. That entry point is
 * ccol_event_loop_reg_count, which resolves, does one atomic read, unpins
 * and returns, with no sleep at all. The test repeats that race under
 * stress. The failure window for a genuinely lock-free pin and unpin pair is
 * only a few instructions wide. It does not reproduce reliably in a single
 * run with no stress. Each iteration uses a fresh loop. Every repetition
 * therefore gets its own independent race, and none of them reuses a handle
 * that is already destroyed. */
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

/* The library must never mix up a legitimate reuse of a slot with a stale
 * handle to the earlier occupant of that slot. That is what the generation
 * counter is for. */
TEST(ccol_event_loop_handle_lifecycle,
     legitimate_slot_reuse_not_confused_with_stale_handle) {
  ccol_event_loop a = ccol_event_loop_create(8, 1, 1, NULL);
  REQUIRE_NE(a, CCOL_EVENT_LOOP_INVALID);
  ccol_event_loop stale_a = a;
  ccol_event_loop_destroy(a);

  ccol_event_loop b = ccol_event_loop_create(8, 1, 1, NULL);
  REQUIRE_NE(b, CCOL_EVENT_LOOP_INVALID);

  /* The operations of B must succeed normally. That holds whether or not the
   * allocator reused the exact address of A for B. */
  REQUIRE_EQ(ccol_event_loop_reg_count(b), (size_t)0);

  /* The stale handle of A must never resolve to B. That holds even when B
   * reuses the same address below it. That is what the generation counter is
   * for. */
  REQUIRE_EQ((void *)_ccol_event_loop_resolve_for_tests(stale_a), NULL);

  ccol_event_loop_destroy(b);
}

/* The slot table is bounded and does not grow forever. A churn loop of
 * create and destroy calls keeps only a single slot in flight at a time. It
 * must reuse that one freed slot on every iteration. It must not grow the
 * table further. This test captures the capacity right after the first
 * create and destroy pair. It does not assert a fixed absolute value such as
 * 1. Other tests earlier in this same process may already have grown the
 * table to some N above 1. This test must prove that ITS OWN churn adds no
 * further growth. It does not need to know the absolute size of the table
 * when it runs. */
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
 * src/cthreadcomm.c, which is built on pthread_atfork(). That covers the
 * machinery of ccol_event_loop itself, and the merged mutex registry of
 * ccol_circular_queue and ccol_dynamic_queue. The compiler drops that
 * machinery when CCOL_FORK_SAFETY_REQUIRED is 0. See the doc comment of that
 * macro in common.h. Without the machinery, the premises of these tests do
 * not hold. Those premises are that a forked child never inherits a locked
 * ccol_event_loop mutex or queue mutex, and that it never hits the AB-BA
 * lock-ordering hazard. Two sets of atfork handlers that register
 * independently are exposed to that hazard. The compiler therefore drops
 * these tests along with the machinery. Nothing leaves them in to hang or to
 * fail. */
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
  /* This field is _Atomic and not plain volatile. volatile alone guarantees
   * no atomicity, and it guarantees no ordering under the C11 memory model.
   * It only stops the compiler from caching the read in a register. The
   * ordering matters between the write of the main test thread
   * (churn.stop = 1;) and the read of this thread. A plain volatile field
   * here is a real data race under the C11 memory model, even though its
   * impact is small. Every other cross-thread handshake flag in this file
   * already uses _Atomic for exactly this reason. */
  _Atomic int stop;
} fork_safety_churn_arg_t;

/* This thread creates and destroys throwaway ccol_event_loop instances all
 * the time. They are completely unrelated to the loop that the main test
 * thread keeps busy below. Its only purpose is to keep SOME thread inside
 * the mutex of ccol_event_loop_slot_table as often as possible. It reaches
 * that mutex through ccol_event_loop_create and ccol_event_loop_destroy. It
 * therefore races the repeated fork() calls of this test. */
static void *fork_safety_churn_thread(void *arg) {
  fork_safety_churn_arg_t *a = (fork_safety_churn_arg_t *)arg;
  while (!atomic_load(&a->stop)) {
    char *err = NULL;
    ccol_event_loop l = ccol_event_loop_create_with_mprocs(4, 1, 1, NULL, &err);
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

/* This test guards against a fork-safety hang. See the doc comment of
 * _ccol_event_loop_atfork_prepare in src/cthreadcomm.c for the full
 * mechanism. fork() duplicates only the thread that calls it. Without that
 * machinery, a child therefore inherits several locks in a locked state.
 * Those are the mutex of ccol_event_loop_slot_table, and the shutdown_lock,
 * the reg_slot_rwlock and the stripes[].lock of any live loop. No thread is
 * left alive in that child that could ever unlock them.
 *
 * This single bounded test recreates both shapes of the hazard at once. A
 * churn thread creates and destroys throwaway ccol_event_loop instances all
 * the time, which touches the mutex of ccol_event_loop_slot_table. It races
 * the repeated fork() calls of this process. That process ALSO keeps one
 * single-stripe, multi-threaded ccol_event_loop busy all the time with a hot
 * fd. Its poller thread and dispatch threads therefore acquire and release
 * the one and only stripe lock of that loop over and over. Each forked child
 * tries one more ccol_event_loop_add at once, on the exact loop that it just
 * inherited. alarm(3) guards that try. Without the alarm, a hang on an
 * inherited locked mutex becomes a test that never finishes. With it, the
 * hang becomes a visible failure. A single stripe maximizes the chance that
 * fork() lands in the middle of a critical section on it. */
TEST(fork_safety, fork_does_not_inherit_a_locked_event_loop_mutex) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  /* The one genuinely UNBOUNDED call here is fork() itself, on the MAIN test
   * thread. Every OTHER hang-prone test in this file is different. alarm(3)
   * bounds the lifetime of a forked child. The WNOHANG poll loop of
   * _wait_for_forked_child_bounded() bounds the reap of the parent. But
   * nothing bounds the fork() syscall itself. Nothing bounds the atfork
   * prepare() and parent() handlers that it runs synchronously before it
   * returns either. A lock-ordering hazard can wedge THERE. This outer alarm
   * is what catches that one case. It is armed for the whole test body and
   * disarmed just before the normal end. 30 trials at a worst-case reap
   * bound of 10s each fit comfortably inside it. */
  alarm(360);
  fork_safety_churn_arg_t churn = {.stop = 0};
  pthread_t churn_tid;
  REQUIRE_EQ(pthread_create(&churn_tid, NULL, fork_safety_churn_thread, &churn),
             0);

  char *err = NULL;
  ccol_event_loop loop =
      ccol_event_loop_create_with_mprocs(16, 1, 3, NULL, &err);
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
      /* This bounds the lifetime of this child. It covers the case where the
       * hazard that this test guards against somehow still fires. Without
       * it, that case hangs the whole suite. The parent below tells this
       * apart from a clean exit with WIFEXITED. */
      alarm(3);

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
    /* 10s is comfortably longer than the alarm(3) of the child. The doc
     * comment of _wait_for_forked_child_bounded says why the parent needs a
     * bound of its own here too, independent of the one of the child. */
    bool reaped = _wait_for_forked_child_bounded(pid, &status, 10000);
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
  int hold_ms;
} queue_fork_lock_arg_t;

static void *queue_fork_lock_thread(void *arg) {
  queue_fork_lock_arg_t *a = (queue_fork_lock_arg_t *)arg;
  ccol_circq_test_lock_mutex_for_tests(a->cq);
  atomic_store(&a->locked, true);
  /* This thread releases on its own fixed schedule. That schedule is
   * completely independent of anything the forking thread does below. The
   * prepare() handler of pthread_atfork MUST block on this exact mutex until
   * this thread releases it. See _queue_atfork_prepare. The forking thread
   * must therefore never be the one that signals this thread to let go. That
   * would make the two threads wait on each other in a real cycle. fork()
   * would block in prepare() and wait for this thread to unlock. This thread
   * would wait for a signal that the forking thread can only send once
   * fork() returns. That is a deadlock inside the test itself. It has
   * nothing to do with the behaviour under test. */
  struct timespec ts = {.tv_sec = a->hold_ms / 1000,
                        .tv_nsec = (long)(a->hold_ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  ccol_circq_test_unlock_mutex_for_tests(a->cq);
  return NULL;
}

/* A pthread_atfork() handler of its own walks the internal mutex of
 * ccol_circular_queue, ccol_dynamic_queue and ccol_channel. That is exactly
 * how every lock that ccol_event_loop owns works. See
 * fork_does_not_inherit_a_locked_event_loop_mutex above, and the doc comment
 * of _queue_atfork_prepare in src/cthreadcomm.c. Without that handler, a
 * child inherits a locked cq->mutex. That happens when a thread OTHER than
 * the one that calls fork() holds it at the exact instant of the fork. No
 * thread is left alive in the child to ever unlock it. Every future
 * operation on that same queue in the child then hangs.
 *
 * The ccol_event_loop regression test above depends on many repeated fork()
 * trials that race a real critical section, which is naturally short. This
 * test reproduces the hazard deterministically instead.
 * ccol_circq_test_lock_mutex_for_tests and
 * ccol_circq_test_unlock_mutex_for_tests exist only under
 * RUNNING_UNIT_TESTS. They let a dedicated holder thread keep cq->mutex
 * locked for a fixed window of HOLD_MS. That window is much longer than any
 * real critical section. The holder then releases on its own schedule. The
 * forking thread calls fork() only once it confirms that the lock is
 * genuinely held. With the atfork prepare() handler in place, fork() must
 * then BLOCK until the holder releases. Its own ccol_mutex_lock(cq->mutex)
 * cannot return before then. The assertion below is a wall-clock lower bound
 * on the duration of fork() itself. It proves that the blocking behaviour
 * really engaged on this run. Without it, the run only shows that the race
 * happened not to matter. */
TEST(fork_safety, fork_does_not_inherit_a_locked_circular_queue_mutex) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char *err = NULL;
  ccol_circular_queue *cq = ccol_circular_queue_create(4, &err);
  REQUIRE_NE((void *)cq, NULL);

  enum { HOLD_MS = 300 };
  queue_fork_lock_arg_t arg = {.cq = cq, .locked = false, .hold_ms = HOLD_MS};
  pthread_t holder;
  REQUIRE_EQ(pthread_create(&holder, NULL, queue_fork_lock_thread, &arg), 0);

  while (!atomic_load(&arg.locked)) {
    /* This waits for the holder thread to confirm that it acquired
     * cq->mutex, before the fork below. Natively this is a short, bounded
     * spin, because the holder thread does nothing else before that store.
     * But a bare atomic-load spin with no yield is not cheap under valgrind.
     * memcheck time-slices every thread through one single instrumented
     * execution engine. It does not give them true parallelism across
     * cores. ctp_fork_feeder_thread in tests/cthreadpool/tests.c has the
     * identical behaviour. Each iteration of this loop is cheap natively.
     * But the iteration count alone would make the `make memtest` run of
     * this test take tens of seconds, or even several minutes. It should
     * take a fraction of a second. sched_yield() caps the spin rate of this
     * thread
     * at what the time-slice granularity of the scheduler allows. The holder
     * thread is then scheduled promptly. Without the yield, this thread
     * starves it by winning the turn of the single instrumented engine over
     * and over. */
    sched_yield();
  }

  /* The child reports its own result over a pipe, and not through its
   * process exit status. Under make memtest, the WEXITSTATUS of a forked
   * child is not reliable. The waitpid() of the parent observes it, and it
   * is not always the value that the child passed to _exit(). The
   * --errors-for-leak-kinds=all and --error-exitcode machinery of valgrind
   * can override it. It does so from whatever it finds "reachable" in the
   * inherited process image of the child at exit time. That has nothing to
   * do with the logic of this test. A debug build shows that the rv of the
   * child is genuinely ccol_success on every run. WEXITSTATUS still comes
   * back non-zero now and then under valgrind. The fork_safety section
   * of tests/clogger/tests.c documents the identical mechanism for its
   * child_can_log_after_fork test. Only WIFEXITED is asserted below, for the
   * same reason. Unlike WEXITSTATUS, it still reliably tells a real
   * regression apart from a clean exit. Such a regression is a hang past
   * alarm(3), or a real ccol_assert() or ccol_fatal_err() abort that raises
   * SIGABRT. */
  int result_pipe[2];
  REQUIRE_EQ(pipe(result_pipe), 0);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  pid_t pid = fork();
  REQUIRE_NE(pid, -1);
  if (pid == 0) {
    /* `holder` does not exist here, because fork() duplicates only the
     * thread that calls it. This child process can come into existence only
     * once the fork() call of the parent returns. By the atfork contract,
     * that needs the ccol_mutex_lock(cq->mutex) of _queue_atfork_prepare to
     * have already succeeded. The holder thread, which has vanished in this
     * process, must therefore already have released it. cq->mutex is
     * unlocked here, and this call returns at once. Without this handling,
     * no atfork handler ever touches cq->mutex. fork() then returns almost
     * at once, whatever the still-live holder thread on the parent side is
     * doing. It hands this child a snapshot of a mutex that was genuinely
     * still locked. This exact call then hangs until alarm(3) kills the
     * child. */
    close(result_pipe[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(3);
    c_message_t msg = {.data = NULL, .size = 0};
    ccol_retval_t rv = ccol_circq_try_send_zc(cq, &msg);
    char byte = (rv == ccol_success) ? 1 : 0;
    test_write_retry_eintr(result_pipe[1], &byte, 1);
    close(result_pipe[1]);
    _exit(0);
  }
  close(result_pipe[1]);

  clock_gettime(CLOCK_MONOTONIC, &t1);
  long long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000LL + (t1.tv_nsec - t0.tv_nsec) / 1000000LL;
  /* This proves that the blocking behaviour of the atfork prepare handler
   * really engaged. fork() must wait for close to the HOLD_MS of the holder
   * before it returns. It must not return almost at once while the lock is
   * still genuinely held. */
  REQUIRE_GE(elapsed_ms, (long long)(HOLD_MS / 2));

  /* This read is bounded, and it is not a bare blocking read(). The doc
   * comment of _read_result_byte_bounded says why a plain read() here cannot
   * be trusted to return. That holds even for a child that was killed or
   * that hangs.
   * 10s matches the parent-side _wait_for_forked_child_bounded call of this
   * test below. */
  char byte = 0;
  ssize_t n = _read_result_byte_bounded(result_pipe[0], &byte, 10000);
  close(result_pipe[0]);
  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);

  int status = 0;
  /* 10s is comfortably longer than the alarm(3) of the child. The doc
   * comment of _wait_for_forked_child_bounded says why the parent needs a
   * bound of its own here too, independent of the one of the child. */
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 10000);
  REQUIRE_TRUE(reaped);
  if (reaped) REQUIRE_TRUE(WIFEXITED(status));

  pthread_join(holder, NULL);
  ccol_circular_queue_destroy(cq);
}

typedef struct {
  ccol_event_loop loop;
  _Atomic bool locked;
  int hold_ms;
} reg_slot_fork_lock_arg_t;

static void *reg_slot_fork_lock_thread(void *arg) {
  reg_slot_fork_lock_arg_t *a = (reg_slot_fork_lock_arg_t *)arg;
  /* The matching unlock call below must get the opaque pointer that this
   * call returns. It must not get a->loop itself. See the doc comment of
   * ccol_event_loop_test_wrlock_reg_slot_for_tests. A second resolve of
   * a->loop from this thread, at unlock time, is a real deadlock hazard
   * against a concurrent fork(). */
  void *resolved = ccol_event_loop_test_wrlock_reg_slot_for_tests(a->loop);
  atomic_store(&a->locked, true);
  /* This thread releases on its own fixed schedule. That schedule is
   * completely independent of anything the forking thread does below. The
   * identical comment of queue_fork_lock_thread says why. The same reasoning
   * applies word for word. It applies here to the Phase 1 reg_slot_rwlock
   * write-lock of _cthreadcomm_atfork_prepare, and not to the cq->mutex lock
   * of _queue_atfork_prepare. */
  struct timespec ts = {.tv_sec = a->hold_ms / 1000,
                        .tv_nsec = (long)(a->hold_ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  ccol_event_loop_test_wrunlock_reg_slot_for_tests(resolved);
  return NULL;
}

/* This is a regression test for the TID-tracked write-lock hazard of
 * reg_slot_rwlock. The comment of _cthreadcomm_atfork_release_impl on its
 * in_child branch for reg_slot_rwlock describes that hazard.
 * reg_slot_rwlock is a ccol_rw_lock_t. Concurrent _ccol_event_reg_resolve
 * calls therefore do not serialize behind one lock. Those calls are the
 * hottest path of ccol_event_loop under a caller that pauses and resumes for
 * each request, such as chttpserver. Any thread that calls
 * ccol_event_loop_add or ccol_event_loop_remove can acquire the write side.
 * That thread is not always the one that later calls fork(). The rwlock
 * write-lock of glibc tracks ownership by TID. A plain ccol_rw_lock_unlock
 * from the surviving thread of the child has a different TID. It therefore
 * silently fails to release a lock that a different, now-vanished thread
 * really locked. Every later resolve in that child then hangs. This test
 * mirrors the structure of
 * fork_does_not_inherit_a_locked_circular_queue_mutex exactly. It puts the
 * reg_slot_rwlock of ccol_event_loop in place of the cq->mutex of
 * ccol_circular_queue. */
TEST(fork_safety, fork_does_not_inherit_a_write_locked_reg_slot_rwlock) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char *err = NULL;
  ccol_event_loop loop =
      ccol_event_loop_create_with_mprocs(4, 1, 1, NULL, &err);
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
      .loop = loop, .locked = false, .hold_ms = HOLD_MS};
  pthread_t holder;
  REQUIRE_EQ(pthread_create(&holder, NULL, reg_slot_fork_lock_thread, &arg), 0);

  while (!atomic_load(&arg.locked)) {
    /* The identical spin-wait comment of
     * fork_does_not_inherit_a_locked_circular_queue_mutex says why
     * sched_yield() matters under valgrind, and a bare spin does not. */
    sched_yield();
  }

  int result_pipe[2];
  REQUIRE_EQ(pipe(result_pipe), 0);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  pid_t pid = fork();
  REQUIRE_NE(pid, -1);
  if (pid == 0) {
    /* `holder` does not exist here, because fork() duplicates only the
     * thread that calls it. This child can come into existence only once the
     * fork() call of the parent returns. That needs the
     * ccol_rw_lock_wrlock(loop->reg_slot_rwlock) of
     * _cthreadcomm_atfork_prepare to have already succeeded. The holder
     * thread, which has vanished in this process, must therefore already
     * have released it. Without this handling, the child does a plain
     * ccol_rw_lock_unlock instead of a reinit. This process then inherits
     * reg_slot_rwlock in a write-locked state, with no thread that could
     * ever release it. The resolve inside ccol_event_loop_reg_generation
     * below then hangs until alarm(3) kills this child. */
    close(result_pipe[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(3);
    uint64_t gen = ccol_event_loop_reg_generation(loop, reg);
    char byte = (gen != 0) ? 1 : 0;
    test_write_retry_eintr(result_pipe[1], &byte, 1);
    close(result_pipe[1]);
    _exit(0);
  }
  close(result_pipe[1]);

  clock_gettime(CLOCK_MONOTONIC, &t1);
  long long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000LL + (t1.tv_nsec - t0.tv_nsec) / 1000000LL;
  /* This proves that the blocking behaviour of the atfork prepare handler
   * really engaged. fork() must wait for close to the HOLD_MS of the holder
   * before it returns. */
  REQUIRE_GE(elapsed_ms, (long long)(HOLD_MS / 2));

  /* This read is bounded, and it is not a bare blocking read(). The doc
   * comment of _read_result_byte_bounded says why a plain read() here cannot
   * be trusted to return. That holds even for a child that was killed or
   * that hangs. */
  char byte = 0;
  ssize_t n = _read_result_byte_bounded(result_pipe[0], &byte, 10000);
  close(result_pipe[0]);
  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);

  int status = 0;
  /* 10s is comfortably longer than the alarm(3) of the child. The doc
   * comment of _wait_for_forked_child_bounded says why the parent needs a
   * bound of its own here too, independent of the one of the child. */
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 10000);
  REQUIRE_TRUE(reaped);
  if (reaped) REQUIRE_TRUE(WIFEXITED(status));

  pthread_join(holder, NULL);

  /* The reg_slot_rwlock of the parent must still be genuinely usable after
   * all of the above. The release path of the parent is a plain
   * ccol_rw_lock_unlock, where the child does a reinit. The fork() call of
   * this same thread validly released that lock. A plain unlock on it is
   * exactly what must work. */
  uint64_t gen_after = ccol_event_loop_reg_generation(loop, reg);
  REQUIRE_NE(gen_after, (uint64_t)0);

  ccol_event_loop_remove(loop, reg);
  ccol_event_loop_destroy(loop);
  close(pfd[0]);
  close(pfd[1]);
}

typedef struct {
  _Atomic bool locked;
  int hold_ms;
} ccol_event_loop_slot_table_fork_lock_arg_t;

static void *ccol_event_loop_slot_table_fork_lock_thread(void *arg) {
  ccol_event_loop_slot_table_fork_lock_arg_t *a =
      (ccol_event_loop_slot_table_fork_lock_arg_t *)arg;
  ccol_event_loop_test_wrlock_slot_table_for_tests();
  atomic_store(&a->locked, true);
  /* This thread releases on its own fixed schedule. That schedule is
   * completely independent of anything the forking thread does below. The
   * identical reasoning of queue_fork_lock_thread elsewhere in this file
   * says why. */
  struct timespec ts = {.tv_sec = a->hold_ms / 1000,
                        .tv_nsec = (long)(a->hold_ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  ccol_event_loop_test_wrunlock_slot_table_for_tests();
  return NULL;
}

/* This is a regression test for the TID-tracked write-lock hazard of
 * ccol_event_loop_slot_table.rwlock. The comment of
 * _cthreadcomm_atfork_release_impl on its in_child branch for that lock
 * describes the hazard. This lock guards the process-wide LOOP table. It
 * differs from the reg_slot_rwlock of a single loop, which
 * fork_does_not_inherit_a_write_locked_reg_slot_rwlock above covers. Every
 * ccol_event_loop_create call and every ccol_event_loop_destroy call
 * acquires it. Its read side is also the very first thing that
 * ccol_event_loop_pause, ccol_event_loop_resume, ccol_event_loop_modify,
 * ccol_event_loop_remove and ccol_event_loop_reg_generation do. Any thread
 * that calls ccol_event_loop_create or ccol_event_loop_destroy can acquire
 * the write side. That thread is not always the one that later calls
 * fork(). The rwlock write-lock of glibc tracks ownership by TID. A plain
 * ccol_rw_lock_unlock from the surviving thread of the child has a different
 * TID. It therefore silently fails to release a lock that a different,
 * now-vanished thread really locked. Every later _ccol_event_loop_resolve in
 * that child then hangs. That covers every ccol_event_loop_create,
 * ccol_event_loop_destroy, ccol_event_loop_add, ccol_event_loop_remove,
 * ccol_event_loop_pause, ccol_event_loop_resume and ccol_event_loop_modify
 * call. */
TEST(fork_safety, fork_does_not_inherit_a_write_locked_event_loop_slot_table) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  enum { HOLD_MS = 300 };
  ccol_event_loop_slot_table_fork_lock_arg_t arg = {.locked = false,
                                                    .hold_ms = HOLD_MS};
  pthread_t holder;
  REQUIRE_EQ(pthread_create(&holder, NULL,
                            ccol_event_loop_slot_table_fork_lock_thread, &arg),
             0);

  while (!atomic_load(&arg.locked)) {
    /* The identical spin-wait reasoning of
     * fork_does_not_inherit_a_locked_circular_queue_mutex says why
     * sched_yield() matters under valgrind, and a bare spin does not. */
    sched_yield();
  }

  /* Every step from here on can fail, and holder is already alive and holds
   * ccol_event_loop_slot_table.rwlock. NONE of these steps may therefore
   * carry a direct assertion. A REQUIRE_* failure returns from this test
   * function at once. The lifetime of holder does not depend on the success
   * of pipe(), of fork(), of the timing check, or of the response of the
   * child. A direct assertion on pipe(), on fork(), or on the elapsed_ms
   * check would leak holder un-joined on each of those failure paths. That
   * is true for the LATER assertions below the read and reap steps too. The
   * test therefore captures every outcome into a local below, every time.
   * The matching REQUIRE_* runs only after pthread_join(holder, NULL). When
   * the test really forked a child, it also runs only after
   * _wait_for_forked_child_bounded, for the identical reason. See the
   * comment of that call. This is the one test in this file where a skipped
   * _dump_stuck_child_diagnostics on a real hang would matter most. */
  int result_pipe[2] = {-1, -1};
  bool pipe_ok = (pipe(result_pipe) == 0);

  pid_t pid = -1;
  long long elapsed_ms = -1;
  char byte = 0;
  ssize_t n = -1;
  bool reaped = false;
  int status = 0;

  if (pipe_ok) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pid = fork();
    if (pid == 0) {
      /* `holder` does not exist here, because fork() duplicates only the
       * thread that calls it. This child can come into existence only once
       * the fork() call of the parent returns. That needs the
       * ccol_rw_lock_wrlock(ccol_event_loop_slot_table.rwlock) of
       * _cthreadcomm_atfork_prepare to have already succeeded. The holder
       * thread, which has vanished in this process, must therefore already
       * have released it. Without this handling, the child does a plain
       * ccol_rw_lock_unlock instead of a reinit. This process then inherits
       * ccol_event_loop_slot_table.rwlock in a write-locked state, with no
       * thread that could ever release it. The resolve inside
       * ccol_event_loop_create below then hangs until alarm(3) kills this
       * child. */
      close(result_pipe[0]);
      alarm(3);
      char *err = NULL;
      ccol_event_loop l = ccol_event_loop_create(4, 1, 1, &err);
      char b = (l != CCOL_EVENT_LOOP_INVALID) ? 1 : 0;
      if (l != CCOL_EVENT_LOOP_INVALID) ccol_event_loop_destroy(l);
      test_write_retry_eintr(result_pipe[1], &b, 1);
      close(result_pipe[1]);
      _exit(0);
    }

    if (pid > 0) {
      close(result_pipe[1]);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000LL +
                   (t1.tv_nsec - t0.tv_nsec) / 1000000LL;

      /* This read is bounded, and it is not a bare blocking read(). The doc
       * comment of _read_result_byte_bounded says why a plain read() here
       * cannot be trusted to return. That holds even for a child that was
       * killed or that hangs. This is exactly the test where an unbounded
       * read can hang a whole CI job. Under qemu-arm, both processes can go
       * silent right after the child-side atfork release of this fork()
       * completes. Nothing is heard from either of them again. */
      n = _read_result_byte_bounded(result_pipe[0], &byte, 10000);
      close(result_pipe[0]);

      /* This wait is bounded, and it is not a bare blocking waitpid(). That
       * matches the convention of every sibling fork_safety test. The doc
       * comment of _wait_for_forked_child_bounded says why this file never
       * uses an unbounded parent-side wait. That holds even though the
       * alarm(3) of the child above already bounds the ordinary case. */
      reaped = _wait_for_forked_child_bounded(pid, &status, 10000);
    } else {
      /* fork() itself failed. Nothing was ever forked, so there is no child
       * to reap. Only the two pipe fds of this process need a close. */
      close(result_pipe[0]);
      close(result_pipe[1]);
    }
  }

  /* This join runs every time, after every step above. It runs whichever of
   * those steps failed, if any. The lifetime of holder never depended on the
   * success of pipe(), of fork(), or of the response of the child. */
  pthread_join(holder, NULL);

  REQUIRE_TRUE(pipe_ok);
  REQUIRE_NE(pid, -1);
  /* This proves that the blocking behaviour of the atfork prepare handler
   * really engaged. fork() must wait for close to the HOLD_MS of the holder
   * before it returns. */
  REQUIRE_GE(elapsed_ms, (long long)(HOLD_MS / 2));
  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);
  REQUIRE_TRUE(reaped);
  if (reaped) REQUIRE_TRUE(WIFEXITED(status));

  /* The ccol_event_loop_slot_table.rwlock of the parent must still be
   * genuinely usable after all of the above. The release path of the parent
   * is a plain ccol_rw_lock_unlock, where the child does a reinit. The
   * fork() call of this same thread validly released that lock. A plain
   * unlock on it is exactly what must work. */
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

/* This test guards against an AB-BA deadlock. A SEPARATE, independent
 * ccol_at_fork() call for the queue-mutex atfork handling would make that
 * deadlock possible. The prepare handlers of pthread_atfork run in REVERSE
 * registration order. Which of two independent handler sets runs first at
 * fork() time is therefore an accident. It depends only on which subsystem a
 * process happens to use first, a ccol_circular_queue or a ccol_event_loop.
 * Take a queue created before the first ccol_event_loop. The atfork triple
 * of the queue registers first, so the prepare handler of ccol_event_loop,
 * which registers second, runs FIRST at fork() time. That prepare handler
 * locks the wait_mtx of a queue-backed registration. A separate
 * queue-registry prepare handler then tries to lock the cq->mutex of that
 * SAME queue. That is the reverse of the order that _notify_waiter always
 * uses. Any ordinary ccol_circq_send_zc or ccol_circq_recv_zc call on an
 * unrelated thread that runs at the same time reaches _notify_waiter. The
 * forking thread then deadlocks inside fork() itself. It blocks on the lock
 * of cq->mutex from that second, independent atfork prepare handler. A
 * concurrent sender thread blocks at the same time on the lock of wait_mtx
 * inside _notify_waiter. That is a textbook cycle, and gdb shows exactly
 * those two stacks. The locking of both subsystems is merged into ONE
 * ccol_at_fork() registration. That registration is
 * _cthreadcomm_atfork_prepare, _cthreadcomm_atfork_release and
 * _cthreadcomm_atfork_child_release. See the three-phase design comment of
 * that function in src/cthreadcomm.c. Nothing then depends on which of two
 * independent handler sets happens to run first.
 *
 * This test forces exactly the dangerous construction order. It creates a
 * ccol_circular_queue before the first ccol_event_loop in a fresh process. It
 * then uses _notify_waiter_test_set_delay_us, which exists only under
 * RUNNING_UNIT_TESTS. That widens a real window inside _notify_waiter. The
 * window sits between "cq->mutex already held" and "about to lock wait_mtx".
 * It is otherwise only a few instructions long. The widened window lasts long
 * enough for a concurrent fork() call to land inside it reliably. Without
 * that, the test depends on timing luck against a very narrow window.
 *
 * The whole scenario runs inside its own forked child process, which an
 * alarm bounds. That mirrors the established pattern of this file, which
 * isolates a risky operation and bounds it from the parent with an alarm and
 * a waitpid. Tests that trigger ccol_fatal_err() use it elsewhere. Every
 * OTHER fork_safety test in this file is different. A regression here
 * deadlocks the FORKING THREAD ITSELF. It does so inside the prepare()
 * handler of fork(), before fork() ever returns to userspace. It does not
 * only deadlock a spawned child, which a plain alarm(3) inside that child
 * could bound on its own. The whole scenario therefore runs in its own child
 * process, bounded by ITS OWN alarm(180). A regression then never hangs the
 * outer test suite.
 *
 * This test reports success over a pipe, and not through the exit status of
 * the child. The doc comment of
 * fork_does_not_inherit_a_locked_circular_queue_mutex above documents the
 * identical reason. The WEXITSTATUS of a forked child, as waitpid()
 * observes it, is not reliably what the child passed to _exit(). The
 * --errors-for-leak-kinds=all and --error-exitcode machinery of make memtest
 * overrides it. Only WIFEXITED means anything, and this test uses even that
 * only to reap the process. A child that hangs exits through SIGALRM from
 * its own alarm(180), which gives WIFSIGNALED. The pipe read is what tells
 * a success apart from a regression. That same alarm bounds the read,
 * because the parent already closed its own copy of the write end. */
TEST(fork_safety,
     fork_does_not_deadlock_with_queue_registered_before_event_loop) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  int result_pipe[2];
  REQUIRE_EQ(pipe(result_pipe), 0);

  pid_t outer_pid = fork();
  REQUIRE_NE(outer_pid, -1);
  if (outer_pid == 0) {
    close(result_pipe[0]);
    /* This bound is wide and not tight. It matches the precedent of this
     * project for a timing margin that valgrind needs, as the async tests of
     * chttpclient also do. The bound must comfortably absorb the real,
     * substantial overhead that valgrind adds to each fork() under a fully
     * loaded test run. That run carries the accumulated heap state and
     * shadow-memory state of 168 other tests ahead of this one. The overhead
     * alone can approach several real seconds now and then, with no bug at
     * all. A regression that this test exists to catch hangs forever in any
     * case. A wider bound therefore costs nothing but a slower report of a
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

    /* This forces the dangerous construction order. It is the first use of a
     * ccol_circular_queue or a ccol_event_loop in this freshly forked
     * process. A queue created before the loop therefore makes the lazy init
     * of the queue-mutex registry happen first, deterministically. */
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
      /* This create is checked, and it is not a fire-and-forget call. It
       * runs inside the forked child above (outer_pid == 0). A failed create
       * cannot be reported with REQUIRE_EQ here. Every sibling fork_safety
       * test with the identical spin-wait setup can do that, because its own
       * pthread_create runs on the PARENT, before any fork. The failure path
       * of REQUIRE_EQ is an early `return` out of this Tau test function.
       * There it lands safely back in the harness that called it. Here that
       * same early `return` leaves THIS test function while the process is
       * still the forked child. It skips the _exit(0) below and falls into
       * the test-running loop of the harness a second time. This process was
       * only ever meant to run this one child-side branch and exit. This
       * code therefore handles a failed create exactly like a failed create
       * of cq, loop or reg just above. It skips the risky section fully and
       * falls through with byte still 0. The existing pipe protocol then
       * reports this run as a failure. Nothing enters the wait loop below
       * with no thread created to ever satisfy it. A deliberately tightened
       * ulimit on processes or threads reproduces this on demand. An
       * unchecked failure here leaves sarg.started false forever. The
       * sched_yield() loop below then spins forever. That is a silent,
       * endless hang instead of a clean, fast failure. */
      if (pthread_create(&sender, NULL, fork_safety2_sender_thread, &sarg) ==
          0) {
        while (!atomic_load(&sarg.started)) {
          /* This waits for the sender to confirm that it is about to call
           * ccol_circq_send_zc, before the fork below. A bare atomic-load
           * spin with no yield is not cheap under valgrind. memcheck
           * time-slices every thread through one single instrumented
           * execution engine. It does not give them true parallelism across
           * cores. The identical reasoning of
           * fork_does_not_inherit_a_locked_circular_queue_mutex above says
           * the same, and so does ctp_fork_feeder_thread in
           * tests/cthreadpool/tests.c. Each iteration of this loop is cheap
           * natively. But the iteration count alone would make the `make
           * memtest` run of this test take well over a minute. It sometimes
           * even passes the alarm of this test, instead of a fraction of a
           * second. sched_yield() caps the spin rate of this thread at what
           * the time-slice granularity of the scheduler allows. The sender
           * thread is then scheduled promptly. Without the yield, this
           * thread starves it by winning the turn of the single instrumented
           * engine over and over. */
          sched_yield();
        }
        /* This gives the sender a moment to acquire cq->mutex and to enter
         * the widened _notify_waiter delay. It runs before the risky fork()
         * call below. */
        usleep(50000);

        pid_t inner_pid = fork();
        if (inner_pid == 0) {
          _exit(0);
        } else if (inner_pid > 0) {
          /* The thing under test is that control reaches this point at all.
           * A regression hangs until the alarm(180) above kills this
           * process. This wait is bounded, and it is not a bare blocking
           * waitpid(). _wait_for_forked_child_bounded holds no Tau macro, so
           * it is safe to call from this already-forked child too. That also
           * keeps this call consistent with every other forked-child wait in
           * this file, instead of one bare exception. */
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
  /* This read is bounded, and it is not a bare blocking read(). The doc
   * comment of _read_result_byte_bounded says why a plain read() here cannot
   * be trusted to return. That holds even for a child that was killed or
   * that hangs.
   * The identical comment of
   * fork_does_not_inherit_a_write_locked_event_loop_slot_table above says
   * the same. 200s matches the outer_pid bound of this test below. */
  char byte = 0;
  ssize_t n = _read_result_byte_bounded(result_pipe[0], &byte, 200000);
  close(result_pipe[0]);

  int status = 0;
  /* This wait is bounded, and it is not a bare blocking waitpid(). The
   * identical comment of
   * fork_does_not_inherit_a_write_locked_event_loop_slot_table above says
   * why. That holds even though the alarm(180) of the child already bounds
   * the ordinary case. */
  bool reaped = _wait_for_forked_child_bounded(outer_pid, &status, 200000);
  REQUIRE_TRUE(reaped);

  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);
}

/* This test covers ccol_event_loop_shutdown and ccol_event_loop_destroy on a
 * ccol_event_loop that a child INHERITS across fork(). Every other fork test
 * in this file does one of two other things. It only calls
 * ccol_event_loop_add and ccol_event_loop_remove on the inherited loop, or
 * it destroys a loop that the child itself created fresh. This is exactly
 * the scenario that the foreign_since_fork field of struct
 * ccol_event_loop_s exists to handle. fork() duplicates only the thread that
 * calls it. The poller_thread field of a forked child therefore names a
 * pthread_t that this process never created and can never join. And
 * loop->shutdown_efd is a real, kernel-level object that the child shares
 * with the parent, and does not copy. The parent still has a genuinely live
 * poller thread on it. Without the foreign_since_fork fixup, a
 * ccol_event_loop_destroy() in a forked child SIGSEGVs. That happens for an
 * inherited loop with num_reactor_threads > 1, on almost every run. The
 * crash is inside __pthread_clockjoin_ex of glibc, reached through the
 * worker-thread join loop of ctpool_shutdown_drain. This test guards that
 * against a future regression. Such a regression can move where the
 * foreign_since_fork check sits relative to the self-call guard. It can also
 * change the matching fixup in cthreadpool.c that this mechanism depends on.
 *
 * This test also verifies the other half. A destroy of the loop in the child
 * must not disturb the still-live loop of the PARENT at all. In particular,
 * it must never write loop->shutdown_efd, which is a kernel object that the
 * two processes share across fork(). Such a write would incorrectly wake the
 * poller thread of the parent. The test checks this by confirming one thing.
 * The identical, still-registered fd selectable of the parent keeps
 * dispatching normally after the child fully tears down its own copy. */
TEST(fork_safety, ccol_event_loop_destroy_of_inherited_loop_in_child_is_safe) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  int result_pipe[2];
  REQUIRE_EQ(pipe(result_pipe), 0);

  char *err = NULL;
  ccol_event_loop loop =
      ccol_event_loop_create_with_mprocs(8, 2, 3, NULL, &err);
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
    /* This bounds the lifetime of this child. It covers the case where the
     * hazard that this test guards against somehow still fires as a hang and
     * not as a crash. Without it, that case hangs the whole suite. The
     * parent below tells this apart from a clean exit with WIFEXITED. That
     * mirrors the convention of this file for these fork-safety tests. */
    alarm(5);

    /* This is the misuse under test. It destroys the exact, fully inherited
     * loop handle. The dispatch_pool workers and the poller thread of that
     * loop exist in this process only as inert, copy-on-write memory. */
    ccol_event_loop_destroy(loop);

    char byte = 1;
    test_write_retry_eintr(result_pipe[1], &byte, 1);
    close(result_pipe[1]);
    _exit(0);
  }

  close(result_pipe[1]);
  /* This read is bounded, and it is not a bare blocking read(). The doc
   * comment of _read_result_byte_bounded says why a plain read() here cannot
   * be trusted to return. That holds even for a child that was killed or
   * that hangs.
   * 10s matches the bound below, which is the alarm(5) of this test plus a
   * margin. */
  char byte = 0;
  ssize_t n = _read_result_byte_bounded(result_pipe[0], &byte, 10000);
  close(result_pipe[0]);

  int status = 0;
  /* 10s is comfortably longer than the alarm(5) of the child. The doc
   * comment of _wait_for_forked_child_bounded says why the parent needs a
   * bound of its own here too, independent of the one of the child. There is
   * no early return on a timeout here, unlike in the simpler fork-safety
   * tests of this file. The loop, the pfd and the ctx of the parent below
   * still need a teardown, whether or not the reap of the child
   * succeeded. */
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 10000);
  REQUIRE_TRUE(reaped);

  /* The real assertion is that the child reaches a clean exit and really
   * wrote the byte. A SIGSEGV is the regression that this test guards
   * against, and it makes both of those fail. The status then says
   * WIFSIGNALED. The read() above returns 0, because the only writer of the
   * pipe dies without a write. This test checks only WIFEXITED on the
   * status, and not WEXITSTATUS. That mirrors the precedent of this file
   * elsewhere. The WEXITSTATUS of a forked child, as the waitpid() of the
   * parent observes it, is not reliably what the child passed to _exit().
   * The --errors-for-leak-kinds=all and --error-exitcode machinery of make
   * memtest overrides it. See the comment of
   * fork_does_not_inherit_a_locked_circular_queue_mutex. */
  if (reaped) REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);

  /* The loop of the parent itself must still be fully alive and must still
   * dispatch normally. The destroy that the child ran on its own inherited
   * copy must not have written the shared shutdown_efd. Such a write would
   * have woken the still-live poller of the parent. That destroy must not
   * have disturbed the kernel-level epoll instance of the parent in any
   * other way either. */
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

/* Counts this process's currently open file descriptors; see test_fds.h. */
static size_t evl_count_open_fds(void) { return (size_t)test_count_open_fds(); }

/* Sleeps for ms milliseconds. Used only to pace bounded polling loops, never
 * as the mechanism any property under test depends on. */
static void evl_sleep_ms(int ms) {
  struct timespec ts = {.tv_sec = ms / 1000,
                        .tv_nsec = (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

TEST(ccol_event_loop, queue_cascade_finishes_before_remove_returns) {
  /* The cascade step that runs after a dispatch for a queue-backed
   * registration locks the mutex of the queue below it. The application owns
   * that queue, and the queue carries no refcount of its own. This module
   * documents one supported way to retire such a registration. The caller
   * calls ccol_event_loop_remove and then destroys the queue at once. See
   * ccol_circular_queue_destroy. The cascade must therefore never still be
   * on its way to that mutex once remove returns. Otherwise it locks freed
   * memory.
   *
   * This test asserts the ordering directly. It does not hope that a
   * sanitizer sees the freed access. The cascade publishes a begun count and
   * a finished count. The property is that remove cannot return between the
   * two. The armed delay below sits exactly in the window that a removal
   * would have to be held off across. It only lengthens that window. It
   * cannot make a correct implementation fail, however long it is.
   *
   * This test is not vacuous. Take the readiness check of the cascade out
   * from under the stripe lock. remove then returns with finished still at
   * the value it had before the dispatch, and this test fails. */
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
  ccol_event_loop_test_queue_cascade_counts(&begun_before, &finished_before);

  bool sent = false, dispatched = false, cascade_begun = false;
  if (registered) {
    ccol_event_loop_test_delay_next_queue_cascade_us(300000);
    char *payload = (char *)malloc(4);
    if (payload) {
      memcpy(payload, "abc", 4);
      c_message_t msg = {.data = payload, .size = 4};
      sent = (ccol_circq_send_zc(cq, &msg) == ccol_success);
      if (!sent) free(payload);
    }
    dispatched = sent && evl_wait_for(&ctx, &ctx.readable_count, 1, 5000);

    /* This wait is bounded. Once begun advances, the cascade holds the
     * stripe lock that the removal below needs. This is therefore a real
     * synchronisation point. It is not a guess about elapsed time. */
    for (int i = 0; i < 5000 && !cascade_begun; i++) {
      uint64_t begun = 0;
      ccol_event_loop_test_queue_cascade_counts(&begun, NULL);
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
    ccol_event_loop_test_queue_cascade_counts(NULL, &finished_at_return);
  }

  /* The documented retirement pattern, exercised for real: nothing else is
   * waited on between the removal and this destroy. */
  if (queue_created) ccol_circular_queue_destroy(cq);

  /* Unconditional, before any assertion, so an unconsumed delay can never
   * leak into a later test. */
  ccol_event_loop_test_delay_next_queue_cascade_us(0);
  evl_sync_ctx_destroy(&ctx);

  REQUIRE_TRUE(queue_created);
  REQUIRE_TRUE(registered);
  REQUIRE_TRUE(sent);
  REQUIRE_TRUE(dispatched);
  REQUIRE_TRUE(cascade_begun);
  REQUIRE_EQ(remove_rv, ccol_success);
  REQUIRE_GT(finished_at_return, finished_before);
}

/* This is a bounded wait. It waits until the poller thread of loop stops
 * completing epoll_wait calls. That is what "parked forever with nothing
 * left to reclaim" looks like from outside. While anything is still
 * deferred, the poller re-arms a bounded retry and this count keeps
 * advancing. The count must already be non-zero. A poller that has simply
 * not started yet is then never mistaken for a parked one. */
static bool evl_wait_until_poller_parked(ccol_event_loop loop, int timeout_ms) {
  uint64_t prev = ccol_event_loop_poller_iterations_for_tests(loop);
  for (int waited = 0; waited < timeout_ms; waited += 150) {
    evl_sleep_ms(150);
    uint64_t now = ccol_event_loop_poller_iterations_for_tests(loop);
    if (now > 0 && now == prev) return true;
    prev = now;
  }
  return false;
}

TEST(ccol_event_loop,
     idle_loop_reclaims_removed_registrations_without_handlers) {
  /* The library closes the bridge eventfd of a removed registration only
   * when it finally reclaims that deferred registration. Only the poller
   * thread ever reclaims one. Two calls do not wake a blocked epoll_wait.
   * Those are epoll_ctl(EPOLL_CTL_DEL), and epoll_ctl(EPOLL_CTL_ADD) of a
   * descriptor that is not ready at that moment. On a loop with no other
   * traffic, the removal itself therefore has to wake the poller. Without
   * that, a repeated add and remove on an idle loop leaks one descriptor for
   * each cycle, without bound.
   *
   * on_removed is deliberately NULL for every registration that the count
   * below covers. Almost every registration in this codebase uses that case.
   *
   * The test drives the poller to a genuinely parked state first. Only then
   * does it take the baseline. A burst of add and remove calls can run
   * before the poller ever blocks. Without that step, the very first reclaim
   * pass of the poller sweeps up that burst. The check then passes whatever
   * the removal does.
   *
   * This test is not vacuous. Restrict the wakeup of the removal to
   * registrations that carry an on_removed handler. The descriptor count
   * then stays CYCLES above its baseline, and this test fails. */
  enum { CYCLES = 8 };
  ccol_event_loop_construct_scoped(loop, 8, 4, 1);
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);
  bool queue_created = (cq != NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  /* This is one real dispatch first. Its only purpose is to prove that the
   * poller thread runs and that it completed at least one epoll_wait. */
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

  /* The test takes this baseline with the poller genuinely blocked. Every
   * descriptor that the cycles below open is therefore one that only their
   * own removals can close again. */
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

  /* This loop is bounded. A correct reclaim happens inside one poller
   * wakeup. This loop therefore normally exits on its first or second
   * sample. */
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
   * registered frees that registration and the event entry that it owns. The
   * registration is what names the entry. Whether on_removed fires depends
   * on the free of that registration. The code must therefore read both
   * before it releases either one. This test pins that on_removed still
   * reaches a queue-backed registration torn down this way. */
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
    /* Nothing ever removes either of these. The destructor of the scoped
     * loop, at the closing brace of this block, tears both of them down
     * while they are still registered. */
  }

  pthread_mutex_lock(&read_ctx.mtx);
  bool read_removed_fired = read_ctx.on_removed_called;
  void *read_arg_seen = read_ctx.on_removed_arg_seen;
  pthread_mutex_unlock(&read_ctx.mtx);
  pthread_mutex_lock(&write_ctx.mtx);
  bool write_removed_fired = write_ctx.on_removed_called;
  void *write_arg_seen = write_ctx.on_removed_arg_seen;
  pthread_mutex_unlock(&write_ctx.mtx);

  /* This is safe now. The teardown walk unlinked both waiter nodes from the
   * queue. */
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

extern void _ccol_event_loop_force_next_reg_free_index_push_failure_for_tests(
    void);
extern size_t _ccol_event_loop_reg_slot_count_for_tests(ccol_event_loop loop);

TEST(ccol_event_loop, a_free_list_push_failure_does_not_strand_a_reg_slot) {
  /* The library releases a registration slot in two steps. It clears the
   * entry of that slot. It then pushes the index of the slot back onto the
   * free list of the loop. That push can fail to allocate. The library then
   * counts the index as lost, and a later acquire recovers it. The slot array
   * therefore does not grow once for each failure while the loop lives.
   *
   * This test is not vacuous. Without the recovery, the slot array grows by
   * one on every cycle. `after` then ends up larger than `before` by the
   * number of cycles, instead of staying the same. */
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

    /* This is one add and remove pair first. It warms the free list. `before`
     * then counts a table that already reached its steady size. */
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

    /* This registration must come from a recovered index, and it must work.
     * A recovered slot that still carries stale bookkeeping fails here. */
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
  /* The recovery reuses the stranded indices, so the table does not grow for
   * each cycle. One more slot is acceptable, for the registration that is in
   * flight. */
  REQUIRE_LE(after, before + 1);
}

/* ========================================================================== */
/*               EVENT LOOP SLOT TABLE LOST-FREE-INDEX RECOVERY               */
/* ========================================================================== */

extern void _ccol_event_loop_force_next_free_index_push_failure_for_tests(void);
extern size_t _ccol_event_loop_slot_table_capacity_for_tests(void);

/*
 * A destroy of a loop pushes the slot index of that loop back onto the free
 * list. That push allocates. When it fails, the index is stranded unless the
 * acquire path reclaims it. Without that reclaim, a process that creates and
 * destroys loops under memory pressure grows the table once for each
 * destroy.
 *
 * This test is not vacuous. Remove the reclaim branch from
 * _ccol_event_loop_handle_slot_acquire. The final capacity then goes past
 * the tolerance below by the number of iterations.
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

  /* This loop must come from a reclaimed slot, and it must be usable. A
     reclaimed index that carries stale bookkeeping answers a resolve with
     the wrong object. It does not fail at acquire time. */
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

/* This callback drains the one message of the queue at once. The queue is
 * therefore genuinely empty by the time the destroy of the test runs. The
 * callback then holds the queue for a long, precisely controlled window. It
 * touches the queue one final time after that window. That final
 * ccol_circq_try_recv_zc is the read that lands on freed memory. It does so
 * when a destroy may complete while this callback is still in flight. */
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

/* This test pins the documented teardown sequence as genuinely safe, and not
 * only as documented. That sequence is a drain of the queue, a call to
 * ccol_event_loop_remove, and then a destroy. ccol_event_loop_remove unlinks
 * the waiter node of the registration. It then returns without a wait for a
 * callback that the reactor already collected. The queue therefore looks
 * unwatched the instant that call returns, while that callback still holds
 * the queue through its own snapshot. The destroy must wait that callback
 * out.
 *
 * The check is structural. It is not a race against a clock. The callback
 * sets callback_done_with_queue only after its last touch of the queue. The
 * test reads that flag on the line after the destroy. A destroy that returns
 * early reads false. By construction, the callback is then still inside a
 * 400 ms hold that it entered before anything called the destroy. Without
 * the wait, that same run is also a use-after-free. The callback commits it
 * a moment later, and AddressSanitizer and valgrind both report it. */
TEST(ccol_event_loop, queue_destroy_waits_for_an_already_collected_dispatch) {
  qdispatch_ctx ctx;
  qdispatch_ctx_init(&ctx);

  ccol_circular_queue *cq =
      ccol_circular_queue_create_with_mprocs(4, NULL, NULL);
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

    /* The block exit here shuts down the scoped loop and joins its reactor
     * thread. Nothing can then still run against ctx below. */
  }

  qdispatch_ctx_destroy(&ctx);

  REQUIRE_TRUE(created);
  REQUIRE_TRUE(registered);
  REQUIRE_TRUE(sent);
  REQUIRE_TRUE(entered);
  REQUIRE_TRUE(done_when_destroy_returned);
}

/* ==========================================================================
 * A ccol_select() waiter that abandons its wait must not swallow the wake
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

/* This polls pred in a bounded loop. It returns false when pred never
 * becomes true. */
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
  return ccol_circq_test_sel_read_waiter_count_for_tests(a->cq) >= a->target;
}

static bool sel_forward_delay_entered(void *arg) {
  (void)arg;
  return ccol_select_test_timed_out_deregister_delay_count() > 0;
}

/* A producer wakes exactly one ccol_select() waiter for each message. That
 * one waiter can already have given up on its wait, because its deadline
 * elapsed. The notification then reaches a thread that never acts on it. The
 * waiter that leaves therefore has to forward it on. Without that forward, a
 * sibling waiter stays parked with a message that sits in the queue.
 *
 * Two steps make this deterministic. First, the test starts the waiters one
 * at a time. It confirms that each one is linked before the next one starts.
 * The later one is therefore the list head, and so it is the target of the
 * single notify of the producer. A fresh rotation starts at the head.
 * Second, the deadline of the head waiter elapses first. The window between
 * that and its deregistration stays open through
 * ccol_select_test_delay_next_timed_out_deregister_us. The producer
 * therefore makes the message squarely inside that window, and not by luck.
 *
 * This test is not vacuous. Without the forward, nothing ever wakes the
 * second waiter. It returns ccol_timed_out on its own much later deadline
 * instead of ccol_success. */
TEST(ccol_select, a_waiter_that_abandons_its_wait_forwards_the_wake) {
  ccol_circular_queue *cq =
      ccol_circular_queue_create_with_mprocs(4, NULL, NULL);
  bool created = (cq != NULL);
  if (!created) {
    REQUIRE_TRUE(created);
    return;
  }

  /* parked_waiter outlives the whole exchange. The forward must wake it.
   * abandoning_waiter is the one that the notify aims at. It is also the one
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
    /* The test arms this before the second waiter starts. That waiter is the
     * only call that can reach its own deadline while this is armed. */
    ccol_select_test_delay_next_timed_out_deregister_us(1000000u);
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

  /* The test joins every thread that started, before any assertion runs. It
   * also clears the armed delay every time. That delay can therefore not
   * leak into a later test. */
  if (abandoning_started) pthread_join(abandoning_tid, NULL);
  if (parked_started) pthread_join(parked_tid, NULL);
  ccol_select_test_delay_next_timed_out_deregister_us(0u);

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

/* An error-only read registration has on_readable NULL and on_error set. It
 * is the shape that a caller uses to say "tell me when this fd dies, I have
 * no interest in reading it". A peer that closes only its write side raises
 * EPOLLIN and EPOLLRDHUP, with no EPOLLHUP. EPOLLHUP needs both directions
 * down. A condition that the loop dispatches as readable, or else not at
 * all, therefore never reaches such a registration.
 *
 * This test is not vacuous. The library must deliver a read-side condition
 * that nothing can consume as an error. Without that, on_error never fires
 * here and the wait below times out. Every epoll_wait also reports that same
 * event again, for as long as nothing dispatches it. The write-direction
 * test below measures that directly. */
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
    /* The block exit joins the reactor thread, so ctx is quiet below. */
  }

  evl_sync_ctx_destroy(&ctx);
  close(sv[0]);
  close(sv[1]);

  REQUIRE_TRUE(registered);
  REQUIRE_TRUE(fired);
}

/* _ccol_event_loop_poller_collect carries its own copy of the dispatch
 * classification. This is the same case against that copy. */
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

/* A healthy connected socket is writable from the instant the loop registers
 * it. Take a registration with no on_writable handler. A write direction
 * armed for EPOLLOUT on behalf of it is therefore reported ready on every
 * single epoll_wait, and nothing can consume it. That is an unbounded spin
 * on the reactor thread. At the default num_reactor_threads of 1, that is
 * the only thread the loop has. The spin then starves every other
 * registration on it too. No action from a peer is needed to trigger it.
 *
 * This test measures with ccol_event_loop_poller_iterations_for_tests, which
 * is a direct count of completed epoll_wait calls. The pause tests above
 * measure their own spin in the same way. This test is not vacuous. Arm
 * EPOLLOUT for a registration with no writer, and the delta below reaches
 * the hundreds of thousands. on_error must also stay silent, because nothing
 * is really wrong with this socket. */
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
      /* This gives a runaway loop a real chance to start before the first
       * sample. The test then measures the delta across a further, wide
       * window. */
      usleep(50000);
      uint64_t before = ccol_event_loop_poller_iterations_for_tests(loop);
      usleep(150000);
      iterations = ccol_event_loop_poller_iterations_for_tests(loop) - before;
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

/* This is the multi-threaded reactor counterpart. EPOLLONESHOT makes the
 * spin a continuous cycle of collect, submit, skip and re-arm. It is not a
 * tight single-thread loop. The same counter catches it in the same way.
 * That counter is the number of completed epoll_wait calls on the one and
 * only poller thread. */
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
      uint64_t before = ccol_event_loop_poller_iterations_for_tests(loop);
      usleep(150000);
      iterations = ccol_event_loop_poller_iterations_for_tests(loop) - before;
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

/* The absolute deadline of a timed wait and the condition variable that it
 * goes to must both use the same clock. The two clocks that a deadline can
 * come from are decades apart. A disagreement between them is therefore
 * never a small error. One direction returns at once with no wait at all.
 * The other blocks for what amounts to forever.
 *
 * The call runs on its own thread. The wait of the main thread bounds it. The
 * second of those two failures therefore shows up as an ordinary test
 * failure, and not as a hung binary. The queue operation that releases a
 * thread that is still blocked is what lets the test join it on either path.
 * This test is not vacuous in both directions. Initialise the condition
 * variables of the queue against a different clock from the deadline. The
 * call then returns in well under the 100 ms that it asked for. Compute the
 * deadline against a different clock from the condition variable. The call
 * then does not return inside the bound below. */
TEST(circular_queues, timed_recv_deadline_matches_its_condition_variable) {
  timed_clock_ctx ctx;
  timed_clock_ctx_init(&ctx);
  ctx.cq = ccol_circular_queue_create_with_mprocs(2, NULL, NULL);
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
      /* This releases a thread that blocks on a deadline that it can never
       * reach. The test can then still join it below. */
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

/* This is the send-direction counterpart. See the receive test above. */
TEST(circular_queues, timed_send_deadline_matches_its_condition_variable) {
  timed_clock_ctx ctx;
  timed_clock_ctx_init(&ctx);
  ctx.cq = ccol_circular_queue_create_with_mprocs(1, NULL, NULL);
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
      /* A thread blocks on a deadline that it can never reach. This frees
       * the slot that it waits for. The test can then still join it
       * below. */
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

/* This covers the receive path of the dynamic queue. See the circular queue
 * receive test above. */
TEST(dynamic_queues, timed_recv_deadline_matches_its_condition_variable) {
  timed_clock_ctx ctx;
  timed_clock_ctx_init(&ctx);
  ctx.dq = ccol_dynamic_queue_create_with_mprocs(NULL, NULL);
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
 * it, so a waiter never burns the quanta that a thread it waits for needs
 * under valgrind. */
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
   * resolved to. The other direction of a channel stays empty throughout. */
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

/* The victim never needs its own callback to do anything. Its dispatch only
 * has to be collected and to sit behind the workers that tear it down. */
static void xqd_victim_on_readable(ccol_event_loop loop, ccol_event_reg reg,
                                   ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  (void)sel;
  (void)arg;
}

/* The trigger callback. It parks until every dispatch worker is inside a
 * trigger callback AND a dispatch for every victim is queued behind them.
 * It then runs the documented teardown of its victim: remove, drain,
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

/* Tears the scenario down on every path. It opens the gate, so that every
 * trigger callback that already runs finishes its own teardown of its
 * victim, and then destroys what is left. When those callbacks never finish,
 * the dispatch workers are deadlocked, and a destroy of the loop would join
 * them for ever. The function then leaves the loop and its queues behind on
 * purpose and returns false, so that the test fails and the binary goes on. */
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

/* Builds the queues of one slot. It returns false with nothing left behind
 * when an allocation fails. */
static bool xqd_build_slot(xqd_scenario *sc, size_t i, int kind) {
  xqd_slot *s = &sc->slots[i];
  s->loop = sc->loop;
  s->kind = kind;
  s->entered = &sc->entered;
  s->done = &sc->done;
  s->gate = &sc->gate;
  s->gate_timed_out = &sc->gate_timed_out;
  s->victim_destroyed = false;
  s->trigger = ccol_circular_queue_create_with_mprocs(4, NULL, NULL);
  if (!s->trigger) return false;
  if (kind == XQD_CIRCQ) {
    s->victim_cq = ccol_circular_queue_create_with_mprocs(4, NULL, NULL);
    if (s->victim_cq)
      s->victim_sel =
          ccol_selectable_from_circq(s->victim_cq, ccol_select_read);
  } else if (kind == XQD_DYNQ) {
    s->victim_dq = ccol_dynamic_queue_create_with_mprocs(NULL, NULL);
    if (s->victim_dq)
      s->victim_sel = ccol_selectable_from_dynq(s->victim_dq, ccol_select_read);
  } else {
    s->victim_ch = ccol_channel_create_with_mprocs(4, NULL, NULL);
    if (s->victim_ch)
      s->victim_sel = ccol_selectable_from_chan(s->victim_ch, ccol_select_read);
  }
  if (!s->victim_cq && !s->victim_dq && !s->victim_ch) {
    ccol_circular_queue_destroy(s->trigger);
    return false;
  }
  return true;
}

/* Runs the whole scenario and returns 0 on success. Every other value names
 * the step that failed; 18 is the deadlock that the test guards against. */
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

  /* Occupy every dispatch worker with one trigger callback. */
  for (size_t i = 0; i < sc->workers && rc == 0; i++)
    if (!xqd_send_one(sc->slots[i].trigger, NULL)) rc = 14;
  if (rc == 0 && !xqd_wait_count(&sc->entered, (int)sc->workers)) rc = 15;

  /* Queue one dispatch for every victim behind those busy workers. The
   * running trigger jobs do not count as pending, so the count reaches
   * workers exactly when every victim job waits in the queue of the pool. */
  for (size_t i = 0; i < sc->workers && rc == 0; i++) {
    xqd_slot *s = &sc->slots[i];
    bool ok = (kind == XQD_DYNQ) ? xqd_send_one(NULL, s->victim_dq)
                                 : xqd_send_one(s->victim_sel.cq, NULL);
    if (!ok) rc = 16;
  }
  for (int waited_ms = 0;
       rc == 0 && ccol_event_loop_dispatch_pool_pending_count_for_tests(
                      sc->loop) < sc->workers;
       waited_ms++) {
    if (waited_ms >= 20000) rc = 17;
    xqd_nap();
  }

  if (rc == 0) {
    atomic_store(&sc->gate, true);
    /* A deadlocked worker never returns, so this is the step that a
     * regression fails at. */
    if (!xqd_wait_count(&sc->done, (int)sc->workers)) rc = 18;
  }
  if (rc == 0 && atomic_load(&sc->gate_timed_out)) rc = 19;

  if (!xqd_teardown(sc)) return rc != 0 ? rc : 20; /* sc leaks on purpose */
  free(sc);
  return rc;
}

/* A callback that follows the documented teardown for a DIFFERENT watched
 * queue must finish, on a loop with a dispatch pool. The header forbids only
 * the destroy of the queue whose own callback runs. A dispatch that the
 * poller collected for the victim, and that waits in the queue of the pool
 * behind the very worker that runs the destroy, must therefore hold nothing
 * that the destroy waits for.
 *
 * This test is non-vacuous. When a collected dispatch claims the reference
 * that a queue destroy waits for at collection time, instead of when the job
 * runs, every worker blocks in the destroy and the scenario fails at the
 * final wait with code 18. With more than one worker the scenario occupies
 * every worker in the same way, so no worker is left to run a queued victim
 * job. A deadlocked loop cannot be destroyed, so on that failure the
 * scenario leaves it behind and the rest of the suite still runs. */
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
  /* A channel selectable resolves to one circular queue. The callback of a
   * channel listener receives on that queue, and not through the channel. */
  return ccol_circq_try_recv_zc(c->sel.cq, &m) == ccol_success;
}

/* Sends one empty message to the queue that the listener watches. A full
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
    c->cq = ccol_circular_queue_create_with_mprocs(64, NULL, NULL);
    if (!c->cq) return false;
    c->sel = ccol_selectable_from_circq(c->cq, ccol_select_read);
  } else if (kind == XQD_DYNQ) {
    c->dq = ccol_dynamic_queue_create_with_mprocs(NULL, NULL);
    if (!c->dq) return false;
    c->sel = ccol_selectable_from_dynq(c->dq, ccol_select_read);
  } else {
    c->ch = ccol_channel_create_with_mprocs(64, NULL, NULL);
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

/* Sends a burst while the one listener of the queue runs its first callback,
 * and returns how many eventfd wakes the burst cost. The dispatch consumed
 * the first wake before it ran the callback, so the first send of the burst
 * delivers one new wake and every later send finds that wake still pending.
 * *delivered_out reports whether every message reached the listener. */
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
      uint64_t before = ccol_select_test_eventfd_wake_count();
      bool sent_all = true;
      for (long i = 0; i < burst && sent_all; i++) {
        /* A 64-slot circular queue cannot hold a longer burst while the
         * callback is parked, so the burst stays below that. */
        sent_all = nwk_send(&c);
      }
      uint64_t after = ccol_select_test_eventfd_wake_count();
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

/* A listener that already has a wake pending costs a send nothing more than
 * the queue operation itself. This test is non-vacuous: when every send
 * writes the eventfd of the listener whatever its state, the burst of 48
 * sends costs 48 wakes instead of 1. */
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
    /* A short pause now and then lets the listener drain the queue to empty,
     * so that many sends land exactly while a dispatch clears the wake. */
    if ((i & 63) == 0) sched_yield();
  }
  return NULL;
}

/* Every message reaches a listener that drains the queue on each callback,
 * while a producer sends exactly as the callbacks run. A lost wake leaves
 * messages in the queue with no dispatch to come, and the bounded wait for
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
       * whose sibling took the last message times out legitimately. */
      a->timed_out = atomic_load(&a->c->received) < a->target;
      return NULL;
    }
    if (rv != ccol_success) return NULL;
    while (nwk_try_recv(a->c)) atomic_fetch_add(&a->c->received, 1);
  }
  return NULL;
}

/* The same property for ccol_select() waiters, in condvar mode and in epoll
 * mode. Two waiters compete for every message, so a waiter that a notify woke
 * often finds the queue empty again, and waits again inside the same call.
 * ccol_select.a_woken_waiter_that_finds_the_queue_empty_waits_again pins that
 * second wait on its own, where a sibling cannot cover for a waiter that
 * sleeps through its wake. */
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
  /* A waiter that is still inside ccol_select_timed returns on one more
   * message, instead of on its own timeout. */
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

/* One waiter that calls ccol_select_timed round after round, and takes every
 * message that it finds. */
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
      /* A message that sits in the queue through a whole timeout is a wake
       * that this waiter slept through. */
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
  return ccol_circq_test_sel_read_waiter_count_for_tests(c->sel.cq) >= 1;
}

/* One waiter, and a producer that takes back the message that woke it before
 * the waiter can look. The waiter then finds the queue empty and waits again
 * inside the same ccol_select_timed call. The next message must wake it. In
 * epoll mode only the wait loop itself clears the wake that the first notify
 * recorded, so this test is non-vacuous: without that clear, the waiter
 * sleeps through the next message and its call times out with that message
 * still in the queue. */
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
    /* Wake the waiter with a message, and take the message straight back. */
    nwk_send(&c);
    (void)nwk_try_recv(&c);
    xqd_nap();
    /* The message that the waiter must see. */
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
    /* One more message ends the current ccol_select_timed call at once. */
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

/* The largest time_t, computed here independently of the library: from the
 * width of the type, not from any constant that the library defines. */
static time_t tsat_time_max(void) {
  return (time_t)(((uintmax_t)1 << (sizeof(time_t) * CHAR_BIT - 1)) - 1);
}

/* A duration past the end of time saturates. UINT64_MAX microseconds is
 * about 584942 years, more than a 32-bit time_t can count from any start,
 * and the sum with a start near the largest time_t passes it on every width.
 * This test is non-vacuous: an unsaturated sum wraps into a negative tv_sec,
 * which every timed wait treats as already elapsed. */
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

/* A timed receive with a timeout past the end of time waits for a message,
 * and does not time out at once. This test is non-vacuous: with a deadline
 * that wraps, the receive returns ccol_timed_out before the send. */
TEST(circular_queues, a_timeout_past_the_end_of_time_waits_for_a_message) {
  ccol_circular_queue *cq =
      ccol_circular_queue_create_with_mprocs(4, NULL, NULL);
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
 * it, waits for its event and does not time out at once. UINT64_MAX is the
 * "no time limit" of ccol_select_timed, so UINT64_MAX - 1 is the value that
 * reaches the saturating deadline of a select. Each case runs on a thread of
 * its own, and the main thread provides the event only after it saw the
 * call still waiting for a while. This test is non-vacuous: a deadline that
 * wraps into the past returns ccol_timed_out before the event. */
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
      a.cq = ccol_circular_queue_create_with_mprocs(1, NULL, NULL);
      ccol_circq_send_zc(a.cq, &filler); /* full */
    } else if (k == HUGE_SELECT_QUEUE || k == HUGE_SELECT_QUEUE_NO_LIMIT) {
      a.cq = ccol_circular_queue_create_with_mprocs(1, NULL, NULL);
    } else if (k == HUGE_DYNMQ_RECV) {
      a.dq = ccol_dynamic_queue_create_with_mprocs(NULL, NULL);
    } else if (k == HUGE_CHAN_RECV || k == HUGE_CHAN_SEND) {
      /* The main thread owns the channel. The waiting thread is a worker:
       * it receives from, and sends to, the queues that the owner reaches
       * with the opposite call. */
      a.ch = ccol_channel_create_with_mprocs(1, NULL, NULL);
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
 * ccol_select_timed on an fd: a short or zero timeout still looks.
 * ------------------------------------------------------------------------ */

/* A timeout of 0 is a poll that does not block, and a timeout of 1000 us is
 * a wait of at most one millisecond. Either one must report an fd that is
 * already readable. This test is non-vacuous: when the wait loop reports a
 * timeout without one epoll_wait call once the remaining time rounds down to
 * 0 ms, both calls return ccol_timed_out. */
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
  /* An empty queue beside the ready fd. The fd is the second selectable. */
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

/* A timed wait on an fd never ends before its deadline. The remaining time
 * rounds UP to a whole millisecond. This test is non-vacuous: with the
 * remaining time truncated, the wait gives up once less than one
 * millisecond remains, and nearly every round below returns early. */
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
 * enough to be the growth of the fd index of a stripe. The index still takes
 * an insert while its table has room, so the first refusals leave the add
 * successful, and a later one makes it fail. During each refusal the
 * allocator makes the fd that is being added readable and then watches the
 * poller for a while. If the entry were already published to epoll at that
 * moment, the poller would wake for it, and on the add that fails it could
 * hold the entry pointer while the add frees the entry. The poller must
 * therefore stay asleep during every refusal. */
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
    uint64_t before = ccol_event_loop_poller_iterations_for_tests(g_afi_loop);
    test_write_retry_eintr(g_afi_signal_fd, "x", 1);
    evl_sleep_ms(50);
    uint64_t after = ccol_event_loop_poller_iterations_for_tests(g_afi_loop);
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

/* This test is non-vacuous: when the add publishes the entry to epoll before
 * the insert into the fd index, the poller wakes during the refused
 * allocation, g_afi_poller_delta is not 0, and under valgrind the poller
 * reads the entry after the failed add freed it. */
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
  g_afi_loop = ccol_event_loop_create_with_mprocs(8, 1, 1, &mp, NULL);
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
      /* The add succeeded after a refusal, and its fd holds the byte that
       * the allocator wrote. Wait until that byte is dispatched, so that the
       * poller is asleep again before the next refusal watches it. */
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

  /* The loop still works: the same fd adds cleanly without the refusal, and
   * the byte that the allocator wrote is dispatched. */
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
 * ccol_event_loop_add: a direction already in use is told apart.
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
 * ccol_event_loop: an event that no handler can take does not spin.
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

/* Counts the epoll_wait calls that the poller completes in a window of
 * 150 ms, after a lead-in of 50 ms that lets a runaway loop start. */
static uint64_t unh_poller_iterations_in_window(ccol_event_loop loop) {
  evl_sleep_ms(50);
  uint64_t before = ccol_event_loop_poller_iterations_for_tests(loop);
  evl_sleep_ms(150);
  return ccol_event_loop_poller_iterations_for_tests(loop) - before;
}

/* A read registration with no handler at all on a socket whose peer closes.
 * The hang-up reaches a registration that can report it to nobody. */
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
      /* A resume arms the registration again, and the hang-up mutes it again
       * without a spin. */
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

/* Reads the fd once. At an end of file it removes its own registration, as
 * the contract of a hang-up asks of a handler that reads it. */
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
 * writer closes. That reports EPOLLHUP without EPOLLIN. With no on_error the
 * hang-up goes to on_readable, whose read() returns 0, and the handler
 * removes the registration. Nothing spins. */
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
     * this point. A registration that the callback did not remove goes away
     * with the loop. */
    ccol_event_loop_destroy(loop);
  }
  *readable = atomic_load(&ctx.readable_count);
  *eofs = atomic_load(&ctx.eof_count);
  close(pfd[0]);
  if (pfd[1] >= 0) close(pfd[1]);
}

/* A muted read registration leaves the write registration of the same fd
 * armed: the hang-up still reaches its on_error, which removes it. */
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
    /* A registration that the callback did not remove goes away with the
     * loop. */
    ccol_event_loop_destroy(loop);
  }
  *errors = atomic_load(&ctx.error_count);
  close(sv[0]);
  if (sv[1] >= 0) close(sv[1]);
}

/* Runs the three cases of this group on a loop with threads reactor
 * threads, before any assertion, so that an early return leaves nothing
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

/* This test is non-vacuous: when a hang-up that no handler can take stays
 * armed, level-triggered epoll reports it on every epoll_wait, and each
 * window below counts thousands of poller iterations instead of a handful.
 * The reader case also fails without the delivery of a hang-up to
 * on_readable: the registration is then muted and readable stays 0. */
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

/* The dispatch-pool counterpart. EPOLLONESHOT turns the spin into a cycle of
 * collect and re-arm, and the same counter catches it. */
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
 * as one ECONNREFUSED, which also clears it. */
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

/* A connected UDP socket with on_readable and no on_error. A datagram sent to
 * a port with no socket makes the kernel queue an ICMP port unreachable on
 * the socket, which epoll reports as EPOLLERR without EPOLLIN. A server then
 * binds that port and sends three datagrams back. */
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
  if (connect(cli, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) != 0) {
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

/* This test is non-vacuous: when an error with no on_error mutes the
 * registration, neither the ECONNREFUSED nor any of the three datagrams that
 * follow it ever reaches on_readable. */
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

typedef struct {
  _Atomic int writable_count;
  _Atomic ccol_event_reg self_reg;
} derr_writer_ctx;

/* Counts the dispatch and removes its own registration; it never writes, so
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
 * whose reader closes. The pipe has no room, so epoll reports EPOLLERR
 * without EPOLLOUT. */
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

/* This test is non-vacuous: when an error with no on_error mutes the
 * registration, on_writable never runs. */
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
 * life of the loop. This test is non-vacuous on a target with a 32-bit
 * size_t: with a size_t epoch the window below counts about ten iterations
 * instead of none. */
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
      /* Let the poller settle in an epoll_wait with no timeout, so that the
       * value below is the one that the removal stamps. */
      evl_sleep_ms(50);
      _ccol_event_loop_set_poller_batch_gen_for_tests(loop,
                                                      (uint64_t)UINT32_MAX);
      removed = ccol_event_loop_remove(loop, reg) == ccol_success;
      notified = derr_wait_count(&removed_calls, 1, 2000);
      evl_sleep_ms(100);
      uint64_t before = ccol_event_loop_poller_iterations_for_tests(loop);
      evl_sleep_ms(500);
      idle_iterations =
          ccol_event_loop_poller_iterations_for_tests(loop) - before;
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

/* One connection with two registrations on the same fd, one for each
 * direction. The application closes it from inside a callback with the
 * documented pattern: remove both registrations, close the fd, release the
 * state. The state stays owned by the test so that a callback that runs
 * after the close is counted instead of reading freed memory. */
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
   * so the close below happens inside the drain of that destroy. */
  for (int i = 0; i < 5000 && !atomic_load(&c->destroy_called); i++)
    evl_sleep_ms(1);
  /* A destroy that makes the handle stop resolving at its start is seen
   * here within this window. A destroy that keeps the handle live for its
   * drain never is, and the window simply runs out. */
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
   * ready together. */
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
 * back into the loop exactly as it does during ccol_event_loop_shutdown. Its
 * ccol_event_loop_remove calls succeed, and no callback of the removed
 * registrations runs after it closed the connection. This test is
 * non-vacuous: a destroy that makes the handle stop resolving before its
 * drain makes both removals fail with ccol_invalid_args. */
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

/* An allocator that fails on demand. fa_fail_at fails exactly the Nth
 * allocation (1-based). fa_fail_size fails every allocation of that size.
 * fa_fail_all fails every allocation. */
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

static void fa_on_readable(ccol_event_loop l, ccol_event_reg reg,
                           ccol_selectable *s, void *arg) {
  (void)reg;
  (void)l;
  uint64_t v;
  if (read(s->fd, &v, sizeof(v)) == (ssize_t)sizeof(v))
    atomic_fetch_add((_Atomic int *)arg, 1);
}

/* The read end of a pipe, non-blocking and close-on-exec, with its write end
 * in *wfd. One 8-byte write makes it readable, and it stays readable until
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

/* Answers whether l dispatches a readable fd within a bounded time.
 * Every allocation succeeds while it runs. */
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
  /* The shutdown joins every thread that could still hold &got. */
  ccol_event_loop_shutdown(l);
  close(efd);
  if (wfd >= 0) close(wfd);
  return ok;
}

/* Every handle that ccol_event_loop_create_with_mprocs returns names a loop
 * that dispatches, whichever allocation fails. The first part fails every
 * allocation of the size of the epoll_wait buffer, and keeps failing it for
 * a while after create returns. This test is non-vacuous: a poller thread
 * that allocates that buffer itself exits on the failure, and the loop that
 * create returned never dispatches. */
TEST(ccol_event_loop, create_under_allocation_failure_never_returns_dead_loop) {
  int dead = 0, created = 0, refused = 0;
  for (size_t threads = 1; threads <= 3; threads += 2) {
    fa_reset();
    atomic_store(&fa_fail_size, (size_t)37 * sizeof(ccol_poll_event));
    char *err = NULL;
    ccol_event_loop l =
        ccol_event_loop_create_with_mprocs(37, 2, threads, &fa_procs, &err);
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

  /* The sweep. One clean create counts the allocations that it makes. */
  fa_reset();
  ccol_event_loop probe =
      ccol_event_loop_create_with_mprocs(16, 2, 3, &fa_procs, NULL);
  long total = atomic_load(&fa_count);
  if (probe != CCOL_EVENT_LOOP_INVALID) ccol_event_loop_destroy(probe);
  for (size_t threads = 1; threads <= 3; threads += 2) {
    for (long n = 1; n <= total + 4; n++) {
      fa_reset();
      atomic_store(&fa_fail_at, n);
      char *err = NULL;
      ccol_event_loop l =
          ccol_event_loop_create_with_mprocs(16, 2, threads, &fa_procs, &err);
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

/* A dispatch that cannot be submitted to the dispatch pool, because every
 * allocation fails, leaves a level-triggered fd ready. The poller backs off
 * instead of spinning, and dispatches normally once allocation works again.
 * This test measures completed epoll_wait calls, not time. It is
 * non-vacuous: without the backoff, the poller re-arms and collects the same
 * event again at once, and the counter races into the thousands inside the
 * window. */
TEST(ccol_event_loop, failed_dispatch_submit_backs_off_instead_of_spinning) {
  fa_reset();
  ccol_event_loop l =
      ccol_event_loop_create_with_mprocs(8, 1, 3, &fa_procs, NULL);
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
    uint64_t before = ccol_event_loop_poller_iterations_for_tests(l);
    evl_sleep_ms(300);
    spins = ccol_event_loop_poller_iterations_for_tests(l) - before;
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
 * thread-local mark, and never by its thread ID.
 * ------------------------------------------------------------------------ */

extern pthread_t _ccol_event_loop_poller_thread_for_tests(ccol_event_loop loop);

typedef struct {
  ccol_event_loop loop;
  bool created;
  pthread_t poller;
  _Atomic bool matched;
  _Atomic int shutdown_rv;
  _Atomic bool destroyed;
} tidr_ctx;

/* Runs only on a thread that carries the ID of the joined poller. A thread
 * of the application may shut the loop down and destroy it. */
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

/* Shuts the loop down, which joins its poller, and then starts batches of
 * threads until one of them carries the ID of that joined poller. glibc
 * hands the cached descriptor of a joined thread to a thread that starts
 * later, so this happens within the first batches. Every started thread is
 * joined before the next batch and before the function returns. */
static void tidr_run(size_t threads, tidr_ctx *c) {
  atomic_init(&c->matched, false);
  atomic_init(&c->shutdown_rv, -1000);
  atomic_init(&c->destroyed, false);
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

/* This test is non-vacuous: when the guards compare ccol_get_thread_id()
 * with the ID of the joined poller, the thread that inherits that ID gets
 * ccol_not_permitted from the shutdown, and a destroy from it ends the
 * process as a self-destroy. The test fails loudly when no thread received
 * the ID, so the case is never left unexamined. */
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

/* on_removed of an ordinary removal runs on the poller thread, with any
 * number of reactor threads. A shutdown of its own loop from there is a
 * self-join, and the guard must refuse it. */
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
 * ccol_event_loop: a queue listener that moves one message for each
 * callback drains a backlog, and one that moves nothing does not spin.
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

/* Puts `count` messages into a queue of the given kind, registers
 * `nregs` read listeners with the given mode on it, and sends nothing more.
 * With BKL_RECV_ONE it waits for the listeners to take every message. It
 * then samples the dispatch count twice, 200 ms apart, so that a listener
 * that is dispatched again and again shows up as a count that still grows. */
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

/* This test is non-vacuous: when no listener is woken again after a dispatch
 * that moved a message, one eventfd wake covers the
 * whole backlog, each callback takes one message, and the listeners stop
 * after one or two messages with the rest stranded in the queue. */
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

/* A listener that ignores a ready queue is not dispatched again for the same
 * wake. With one listener that is exactly one dispatch; with two, the wake
 * reaches each at most twice, and the count stops growing. */
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

/* The write direction. A listener that sends one message for each callback
 * fills a circular queue to its bound with no receive at all, and then stops,
 * because the queue is no longer writable. A listener that sends nothing is
 * dispatched once. */
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

/* This test is non-vacuous: without the re-wake after a dispatch that sent a
 * message, the listener sends one message and is never dispatched again. */
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
 * modify that keeps its direction arms it again: the hang-up is reported
 * once more, which the poller counts as one more epoll_wait, and mutes it
 * again without a spin. */
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
      /* Lets the hang-up arrive and mute the registration. */
      evl_sleep_ms(100);
      uint64_t before = ccol_event_loop_poller_iterations_for_tests(loop);
      bool modified =
          ccol_event_loop_modify(loop, reg, ccol_select_read) == ccol_success;
      for (int i = 0; i < 2000; i++) {
        if (ccol_event_loop_poller_iterations_for_tests(loop) > before) break;
        evl_sleep_ms(1);
      }
      *advanced = ccol_event_loop_poller_iterations_for_tests(loop) - before;
      *window = unh_poller_iterations_in_window(loop);
      *ok = modified && ccol_event_loop_remove(loop, reg) == ccol_success;
    }
    ccol_event_loop_destroy(loop);
  }
  close(sv[0]);
  if (sv[1] >= 0) close(sv[1]);
}

/* This test is non-vacuous: when a modify to the current direction returns
 * without arming the muted registration again, nothing wakes the poller and
 * the iteration count does not advance. */
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
extern bool _ccol_queue_mutex_registry_check_for_tests(size_t *count,
                                                       uint64_t *remove_probes);

/* This test is non-vacuous: a remove that scans the registry for the mutex
 * examines up to every live entry, so the probe count grows with the square
 * of the number of queues, and a swap that does not update the position of
 * the moved entry fails the index check. */
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
  /* A fixed shuffle, so that removes hit the middle, the ends and the
   * last entry in every pattern. */
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
 * dispatches going whatever the position of an idle sibling in the list.
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

/* Registers an idle listener first and an active one second on one queue
 * of the given kind and direction (idle_first), or the other way round.
 * The active writer sends `count` messages, one for each callback; for the
 * read direction the queue holds `count` messages before the listeners
 * exist and the active reader takes one for each callback. Nothing else
 * touches the queue. */
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

/* This test is non-vacuous: when only the last listener of the list may
 * wake a listener again after a dispatch that moved a message, an active
 * listener that registered after an idle one is the head of the list, its
 * progress only reaches the idle tail, and the idle tail ends the chain; the
 * active writer then sends one message and the active reader takes one. */
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
          /* The idle sibling is dispatched a bounded number of times: at
           * most twice for each message that moved (a forward from the
           * active listener, and the count moving under its own dispatch
           * when the two run on different threads), plus the wakes of the
           * registrations, and never again once the traffic ended. */
          REQUIRE_LE(x->idle_settled, 210L);
          REQUIRE_EQ(x->idle_later, x->idle_settled);
        }
      }
    }
  }
}

/* Three listeners that all decline a ready queue pass a wake among
 * themselves a bounded number of times and then stop. */
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
 * ccol_event_loop: a listener that leaves a queue hands on the wake that it
 * holds, so the other listeners of the queue still hear about the message.
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

/* The staying listener is a registration of a second loop (use_select
 * false) or a ccol_select_timed() caller. The leaving listener is a
 * registration of loop L1 that links after it, so it is the head of the
 * list and a send wakes it. The poller of L1 is held in a callback of a pipe,
 * so the leaving registration cannot dispatch before it leaves. It leaves by
 * ccol_event_loop_remove (by_destroy false) or by the destroy of L1.
 * Returns true when the staying listener got the message. */
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
    for (int i = 0;
         ok && i < 5000 && !ccol_circq_test_has_sel_read_waiter_for_tests(c.cq);
         i++)
      evl_sleep_ms(1);
    ok = ok && ccol_circq_test_has_sel_read_waiter_for_tests(c.cq);
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
      /* Lets the destroy claim the loop and ask the poller to stop before
       * the poller leaves the blocking callback. */
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
  /* After a remove, the leaving registration never runs. A destroy may still
   * dispatch it during its drain, which hands the wake on as well. */
  *setup_ok = ok && sent && (!by_destroy || dt_started) &&
              (by_destroy || atomic_load(&c.leaving_calls) == 0);
  return delivered;
}

/* This test is non-vacuous: when a registration that holds the one wake of a
 * send leaves without handing it on, the other listener of the queue is not
 * woken, the registration of the second loop never receives the message and
 * the ccol_select_timed() caller times out with the message still queued. */
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
 * more.
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
 * registration is removed sends one message while the destroy of its queue
 * waits for it. Returns only when the destroy did not assert. */
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
  /* The queue is empty here, and the running callback sends one message
   * before this destroy may free the queue. */
  if (dynq)
    ccol_dynamic_queue_destroy(dq);
  else
    ccol_circular_queue_destroy(cq);
  _exit(0);
}

/* This test is non-vacuous: when the destroy counts the messages before it
 * waits for the running callback, it sees an empty queue, the message that
 * the callback sends is freed with the queue, and the child exits 0. */
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
 * The documented refusals of a send and of an fd that epoll cannot watch.
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

/* epoll(7) refuses a regular file. ccol_select reports that as
 * ccol_unexpected_failure, and ccol_event_loop_add refuses the fd as a
 * failed system call. */
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
