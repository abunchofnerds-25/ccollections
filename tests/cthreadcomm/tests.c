#include <assert.h>
#include <cthreadcomm.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <tau/tau.h>
#include <time.h>
#include <unistd.h>
TAU_MAIN()  // sets up Tau (+ main function)

extern void add_duration_to_timespec(struct timespec *target,
                                     struct timespec *duration);

TEST(add_duration_to_timespec, edge_cases) {
  {
    struct timespec t;
    struct timespec duration;

    t.tv_sec = 1;
    t.tv_nsec = 600000000;

    duration.tv_sec = 2;
    duration.tv_nsec = 400000000;

    add_duration_to_timespec(&t, &duration);

    REQUIRE_EQ(t.tv_sec, 4);
    REQUIRE_EQ(t.tv_nsec, 0);
  }

  {
    struct timespec t;
    struct timespec duration;

    t.tv_sec = 1;
    t.tv_nsec = 599999999;

    duration.tv_sec = 2;
    duration.tv_nsec = 400000000;

    add_duration_to_timespec(&t, &duration);

    REQUIRE_EQ(t.tv_sec, 3);
    REQUIRE_EQ(t.tv_nsec, 999999999);
  }

  {
    struct timespec t;
    struct timespec duration;

    t.tv_sec = 1;
    t.tv_nsec = 599999999;

    duration.tv_sec = 2;
    duration.tv_nsec = 1400000000;

    add_duration_to_timespec(&t, &duration);

    REQUIRE_EQ(t.tv_sec, 4);
    REQUIRE_EQ(t.tv_nsec, 999999999);
  }

  {
    struct timespec t;
    struct timespec duration;

    t.tv_sec = 1;
    t.tv_nsec = 600000000;

    duration.tv_sec = 2;
    duration.tv_nsec = 1400000000;

    add_duration_to_timespec(&t, &duration);

    REQUIRE_EQ(t.tv_sec, 5);
    REQUIRE_EQ(t.tv_nsec, 0);
  }

  {
    struct timespec t;
    struct timespec duration;

    t.tv_sec = 1;
    t.tv_nsec = 1599999999;

    duration.tv_sec = 2;
    duration.tv_nsec = 400000000;

    add_duration_to_timespec(&t, &duration);

    REQUIRE_EQ(t.tv_sec, 4);
    REQUIRE_EQ(t.tv_nsec, 999999999);
  }

  {
    struct timespec t;
    struct timespec duration;

    t.tv_sec = 1;
    t.tv_nsec = 1600000000;

    duration.tv_sec = 2;
    duration.tv_nsec = 400000000;

    add_duration_to_timespec(&t, &duration);

    REQUIRE_EQ(t.tv_sec, 5);
    REQUIRE_EQ(t.tv_nsec, 0);
  }
}

TEST(add_duration_to_timespec, duration_not_mutated) {
  // The function must not modify the caller's duration struct even when
  // duration->tv_nsec is overflowed and would normally be normalised.
  struct timespec t = {.tv_sec = 1, .tv_nsec = 600000000};
  struct timespec d = {.tv_sec = 2, .tv_nsec = 1500000000};
  add_duration_to_timespec(&t, &d);
  // Verify the result is correct (function was actually called).
  REQUIRE_EQ(t.tv_sec, 5);
  REQUIRE_EQ(t.tv_nsec, 100000000);
  // Duration struct must be unchanged.
  REQUIRE_EQ(d.tv_sec, 2);
  REQUIRE_EQ(d.tv_nsec, 1500000000);
}

// CIRCULAR_QUEUE TESTS

TEST(circular_queues, create_fails) {
  char *err_str = NULL;

  circular_queue *cq = circular_queue_create_with_mprocs(0, NULL, &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  cq = circular_queue_create_with_mprocs(-1, NULL, &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  cq = circular_queue_create_with_mprocs(
      (size_t)INT32_MAX,
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = NULL},
      &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);
}

TEST(circular_queues, create_and_destroy_no_mem_procs) {
  char *err_str = "";

  circular_queue *cq = circular_queue_create_with_mprocs(1, NULL, &err_str);
  REQUIRE_NE((void *)cq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  circular_queue_destroy(cq);
  REQUIRE_EQ((void *)cq, NULL);
}

TEST(circular_queues, create_and_destroy_with_mem_procs) {
  char *err_str = "";

  circular_queue *cq = circular_queue_create_with_mprocs(
      1,
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      &err_str);
  REQUIRE_NE((void *)cq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  circular_queue_destroy(cq);
  REQUIRE_EQ((void *)cq, NULL);
}

TEST(circular_queues, basic_send_and_receive_no_mem_procs) {
  circular_queue *cq = circular_queue_create_with_mprocs(1, NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(circq_send_zc(cq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);  // The ownership of the message is lost.

  c_message_t m2;
  REQUIRE_EQ(circq_recv_zc(cq, &m2), ccol_success);

  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');
  REQUIRE_EQ(m2.size, 16);

  free(m2.data);
  circular_queue_destroy(cq);
}

TEST(circular_queues, basic_send_and_receive_with_mem_procs) {
  circular_queue *cq = circular_queue_create_with_mprocs(
      1,
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(circq_send_zc(cq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);  // The ownership of the message is lost.

  c_message_t m2;
  REQUIRE_EQ(circq_recv_zc(cq, &m2), ccol_success);

  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');
  REQUIRE_EQ(m2.size, 16);

  free(m2.data);
  circular_queue_destroy(cq);
}

TEST(circular_queues, msg_count) {
  circular_queue *cq = circular_queue_create_with_mprocs(3, NULL, NULL);

  c_message_t m1 = {.data = NULL, .size = 0};

  for (size_t i = 0; i < 3; ++i) {
    REQUIRE_EQ(circq_msg_count(cq), i);
    circq_send_zc(cq, &m1);
    REQUIRE_EQ(circq_msg_count(cq), i + 1);
  }

  for (size_t i = 3; i > 0; --i) {
    REQUIRE_EQ(circq_msg_count(cq), i);
    circq_recv_zc(cq, &m1);
    REQUIRE_EQ(circq_msg_count(cq), i - 1);
  }

  circular_queue_destroy(cq);
}

TEST(circular_queues, basic_send_and_receive_NULL_msg) {
  circular_queue *cq = circular_queue_create_with_mprocs(3, NULL, NULL);

  c_message_t m1 = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_send_zc(cq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  m1.data = malloc(sizeof(char));
  REQUIRE_EQ(circq_send_zc(cq, &m1), ccol_invalid_args);
  REQUIRE_NE(m1.data, NULL);
  free(m1.data);
  m1.data = NULL;

  c_message_t m2 = {.data = (void *)0xabcdef01, .size = 0x35};
  REQUIRE_EQ(circq_recv_zc(cq, &m2), ccol_success);
  REQUIRE_EQ(m2.data, NULL);
  REQUIRE_EQ(m2.size, 0);

  circular_queue_destroy(cq);
}

TEST(circular_queues, try_send_and_try_receive) {
  circular_queue *cq = circular_queue_create_with_mprocs(1, NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(circq_try_send_zc(cq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  m1.data = malloc(sizeof(char));
  m1.size = 1;
  REQUIRE_EQ(circq_try_send_zc(cq, &m1), ccol_container_full);
  REQUIRE_NE(m1.data, NULL);
  free(m1.data);
  m1.data = NULL;

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_try_recv_zc(cq, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  REQUIRE_EQ(circq_try_recv_zc(cq, &m1), ccol_container_empty);
  REQUIRE_EQ(m1.data, NULL);

  free(m2.data);
  circular_queue_destroy(cq);
}

#define getWallTime(A) clock_gettime(CLOCK_REALTIME, &A);
#define diffTimeUSec(A, B) \
  (B.tv_sec - A.tv_sec) * 1000000 + (B.tv_nsec - A.tv_nsec) / 1000

TEST(circular_queues, timed_send_and_timed_receive) {
  circular_queue *cq = circular_queue_create_with_mprocs(1, NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  struct timespec timeout;
  timeout.tv_sec = 0;           // 0  secs
  timeout.tv_nsec = 100000000;  // 100 msecs

  struct timespec before;
  struct timespec after;

  getWallTime(before);
  REQUIRE_EQ(circq_timed_send_zc(cq, &m1, &timeout), ccol_success);
  getWallTime(after);
  REQUIRE_LT(diffTimeUSec(before, after), 10000);
  REQUIRE_EQ(m1.data, NULL);

  m1.data = malloc(sizeof(char));
  m1.size = 1;
  getWallTime(before);
  REQUIRE_EQ(circq_timed_send_zc(cq, &m1, &timeout), ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 100000);
  REQUIRE_NE(m1.data, NULL);
  free(m1.data);
  m1.data = NULL;

  c_message_t m2 = {.data = NULL, .size = 0};

  getWallTime(before);
  REQUIRE_EQ(circq_timed_recv_zc(cq, &m2, &timeout), ccol_success);
  getWallTime(after);
  REQUIRE_LT(diffTimeUSec(before, after), 10000);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  getWallTime(before);
  REQUIRE_EQ(circq_timed_recv_zc(cq, &m1, &timeout), ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 100000);
  REQUIRE_EQ(m1.data, NULL);

  free(m2.data);
  circular_queue_destroy(cq);
}

TEST(circular_queues, enable_disable_sending) {
  circular_queue *cq = circular_queue_create_with_mprocs(1, NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  circq_disable_sending(cq);

  REQUIRE_EQ(circq_send_zc(cq, &m1), ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  REQUIRE_EQ(circq_try_send_zc(cq, &m1), ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  REQUIRE_EQ(circq_timed_send_zc(cq, &m1,
                                 &(struct timespec){.tv_sec = 1, .tv_nsec = 0}),
             ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  circq_enable_sending(cq);

  REQUIRE_EQ(circq_send_zc(cq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  c_message_t m2 = {.data = NULL, .size = 0};

  REQUIRE_EQ(circq_recv_zc(cq, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  free(m2.data);
  circular_queue_destroy(cq);
}

TEST(circular_queues, ring_wrap_around) {
  // With capacity 2, sending and receiving 5 messages forces head and tail
  // indices to wrap around the ring boundary multiple times.
  circular_queue *cq = circular_queue_create_with_mprocs(2, NULL, NULL);

  for (int i = 0; i < 5; ++i) {
    char *buf = malloc(sizeof(char));
    *buf = 'A' + i;
    c_message_t out = {.data = buf, .size = 1};
    REQUIRE_EQ(circq_send_zc(cq, &out), ccol_success);
    REQUIRE_EQ(out.data, NULL);

    c_message_t in = {.data = NULL, .size = 0};
    REQUIRE_EQ(circq_recv_zc(cq, &in), ccol_success);
    REQUIRE_EQ(*(char *)in.data, 'A' + i);
    free(in.data);
  }

  circular_queue_destroy(cq);
}

TEST(circular_queues, timed_args_null_timeout) {
  // NULL timeout must return ccol_invalid_args without crashing.
  circular_queue *cq = circular_queue_create_with_mprocs(1, NULL, NULL);

  c_message_t m = {.data = malloc(sizeof(char)), .size = 1};
  REQUIRE_EQ(circq_timed_send_zc(cq, &m, NULL), ccol_invalid_args);
  REQUIRE_NE(m.data, NULL);  // caller retains ownership on failure

  c_message_t recv_m = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_timed_recv_zc(cq, &recv_m, NULL), ccol_invalid_args);

  free(m.data);
  circular_queue_destroy(cq);
}

TEST(circular_queues, recv_drains_successfully_after_disable) {
  // Messages already in the queue must still be receivable after sending is
  // disabled. Once the queue is empty a timed recv must time out rather than
  // return ccol_not_permitted (disabling only affects senders).
  circular_queue *cq = circular_queue_create_with_mprocs(2, NULL, NULL);

  c_message_t m = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_send_zc(cq, &m), ccol_success);
  REQUIRE_EQ(circq_send_zc(cq, &m), ccol_success);

  circq_disable_sending(cq);

  // Existing messages must still drain successfully.
  REQUIRE_EQ(circq_recv_zc(cq, &m), ccol_success);
  REQUIRE_EQ(circq_recv_zc(cq, &m), ccol_success);

  // Queue is now empty with sending disabled: timed recv must time out,
  // not return ccol_not_permitted.
  struct timespec timeout = {.tv_sec = 0, .tv_nsec = 50000000};  // 50 ms
  REQUIRE_EQ(circq_timed_recv_zc(cq, &m, &timeout), ccol_timed_out);

  circular_queue_destroy(cq);
}

typedef struct {
  circular_queue *cq;
  ccol_retval_t result;
} cq_disable_recv_args;

void *cq_blocking_recv_thread(void *raw) {
  cq_disable_recv_args *a = (cq_disable_recv_args *)raw;
  c_message_t m = {.data = NULL, .size = 0};
  a->result = circq_recv_zc(a->cq, &m);
  return NULL;
}

TEST(circular_queues, disable_sending_does_not_unblock_recv) {
  // A thread blocked in circq_recv_zc must NOT be woken by
  // circq_disable_sending. It must stay blocked and only return once sending
  // is re-enabled and a message arrives.
  circular_queue *cq = circular_queue_create_with_mprocs(1, NULL, NULL);

  cq_disable_recv_args args = {.cq = cq, .result = ccol_unexpected_failure};
  pthread_t tid;
  pthread_create(&tid, NULL, cq_blocking_recv_thread, &args);

  usleep(20000);  // let the receiver block on the empty queue
  circq_disable_sending(cq);
  usleep(20000);  // receiver must still be blocked at this point

  // Re-enable and send a message to unblock the receiver.
  circq_enable_sending(cq);
  c_message_t m = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_send_zc(cq, &m), ccol_success);
  pthread_join(tid, NULL);

  REQUIRE_EQ(args.result, ccol_success);

  circular_queue_destroy(cq);
}

TEST(circular_queues, timed_recv_times_out_when_disabled) {
  // circq_timed_recv_zc must return ccol_timed_out (not ccol_not_permitted)
  // when the queue is empty and sending is disabled. The disable state must not
  // cause an early return before the deadline.
  circular_queue *cq = circular_queue_create_with_mprocs(1, NULL, NULL);

  circq_disable_sending(cq);

  struct timespec timeout = {.tv_sec = 0, .tv_nsec = 100000000};  // 100 ms
  struct timespec before, after;
  c_message_t m = {.data = NULL, .size = 0};

  getWallTime(before);
  REQUIRE_EQ(circq_timed_recv_zc(cq, &m, &timeout), ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 100000);  // must wait out the timeout

  circular_queue_destroy(cq);
}

void *cq_helper_thread(void *args) {
  circular_queue *cq = (circular_queue *)args;
  // Let's make the sender block while sending the second message.
  usleep(50000);

  c_message_t m = {.data = NULL, .size = 0};
  assert(circq_recv_zc(cq, &m) == ccol_success);
  assert(((char *)(m.data))[0] == 'A');
  assert(((char *)(m.data))[1] == '\0');
  free(m.data);
  m.data = NULL;

  assert(circq_recv_zc(cq, &m) == ccol_success);
  assert(((char *)(m.data))[0] == 'B');
  assert(((char *)(m.data))[1] == '\0');
  free(m.data);
  m.data = NULL;

  return NULL;
}

TEST(circular_queues, send_and_receive_thread) {
  circular_queue *cq = circular_queue_create_with_mprocs(1, NULL, NULL);

  pthread_t tid;
  pthread_create(&tid, NULL, cq_helper_thread, cq);

  c_message_t m = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m.data))[0] = 'A';
  ((char *)(m.data))[1] = '\0';

  REQUIRE_EQ(circq_send_zc(cq, &m), ccol_success);
  REQUIRE_EQ(m.data, NULL);  // The ownership of the message is lost.

  m = (c_message_t){.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m.data))[0] = 'B';
  ((char *)(m.data))[1] = '\0';

  REQUIRE_EQ(circq_send_zc(cq, &m), ccol_success);
  REQUIRE_EQ(m.data, NULL);  // The ownership of the message is lost.

  pthread_join(tid, NULL);

  circular_queue_destroy(cq);
}

// DYNAMIC_QUEUE TESTS

TEST(dynamic_queues, create_fails) {
  char *err_str = "";

  dynamic_queue *dq = dynamic_queue_create_with_mprocs(
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = NULL},
      &err_str);
  REQUIRE_EQ((void *)dq, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  dynamic_queue_destroy(dq);
  REQUIRE_EQ((void *)dq, NULL);
}

TEST(dynamic_queues, create_and_destroy_no_mprocs) {
  char *err_str = "";

  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, &err_str);
  REQUIRE_NE((void *)dq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  dynamic_queue_destroy(dq);
  REQUIRE_EQ((void *)dq, NULL);
}

TEST(dynamic_queues, create_and_destroy_with_mprocs) {
  char *err_str = "";

  dynamic_queue *dq = dynamic_queue_create_with_mprocs(
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      &err_str);
  REQUIRE_NE((void *)dq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  dynamic_queue_destroy(dq);
  REQUIRE_EQ((void *)dq, NULL);
}

TEST(dynamic_queues, basic_send_and_receive_no_mprocs) {
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);  // The ownership of the message is lost.

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(dynmq_recv_zc(dq, &m2), ccol_success);

  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  free(m2.data);
  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, basic_send_and_receive_with_mprocs) {
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);  // The ownership of the message is lost.

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(dynmq_recv_zc(dq, &m2), ccol_success);

  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  free(m2.data);
  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, msg_count) {
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  c_message_t m1 = {.data = NULL, .size = 0};

  for (size_t i = 0; i < 3; ++i) {
    REQUIRE_EQ(dynmq_msg_count(dq), i);
    dynmq_send_zc(dq, &m1);
    REQUIRE_EQ(dynmq_msg_count(dq), i + 1);
  }

  for (size_t i = 3; i > 0; --i) {
    REQUIRE_EQ(dynmq_msg_count(dq), i);
    dynmq_recv_zc(dq, &m1);
    REQUIRE_EQ(dynmq_msg_count(dq), i - 1);
  }

  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, basic_send_and_receive_NULL_msg) {
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  c_message_t m1 = {.data = NULL, .size = 0};
  REQUIRE_EQ(dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  REQUIRE_EQ(dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  m1.data = malloc(sizeof(char));
  m1.size = 0;
  REQUIRE_EQ(dynmq_send_zc(dq, &m1), ccol_invalid_args);
  REQUIRE_NE(m1.data, NULL);
  free(m1.data);
  m1.data = NULL;

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(dynmq_recv_zc(dq, &m2), ccol_success);
  REQUIRE_EQ(m2.data, NULL);

  REQUIRE_EQ(dynmq_recv_zc(dq, &m2), ccol_success);
  REQUIRE_EQ(m2.data, NULL);

  REQUIRE_EQ(dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  REQUIRE_EQ(dynmq_recv_zc(dq, &m2), ccol_success);
  REQUIRE_EQ(m2.data, NULL);

  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, fifo_ordering) {
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  const char labels[] = {'A', 'B', 'C'};
  for (int i = 0; i < 3; ++i) {
    char *buf = malloc(sizeof(char));
    *buf = labels[i];
    c_message_t m = {.data = buf, .size = 1};
    REQUIRE_EQ(dynmq_send_zc(dq, &m), ccol_success);
    REQUIRE_EQ(m.data, NULL);
  }
  REQUIRE_EQ(dynmq_msg_count(dq), 3);

  for (int i = 0; i < 3; ++i) {
    c_message_t m = {.data = NULL, .size = 0};
    REQUIRE_EQ(dynmq_recv_zc(dq, &m), ccol_success);
    REQUIRE_EQ(*(char *)m.data, labels[i]);
    free(m.data);
  }

  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, send_and_try_receive) {
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(dynmq_try_recv_zc(dq, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  REQUIRE_EQ(dynmq_try_recv_zc(dq, &m1), ccol_container_empty);
  REQUIRE_EQ(m1.data, NULL);

  free(m2.data);
  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, send_and_timed_receive) {
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  struct timespec timeout;
  timeout.tv_sec = 0;           // 0  secs
  timeout.tv_nsec = 100000000;  // 100 msecs

  struct timespec before;
  struct timespec after;

  REQUIRE_EQ(dynmq_send_zc(dq, &m1), ccol_success);

  c_message_t m2 = {.data = NULL, .size = 0};

  getWallTime(before);
  REQUIRE_EQ(dynmq_timed_recv_zc(dq, &m2, &timeout), ccol_success);
  getWallTime(after);
  REQUIRE_LT(diffTimeUSec(before, after), 10000);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  getWallTime(before);
  REQUIRE_EQ(dynmq_timed_recv_zc(dq, &m1, &timeout), ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 100000);
  REQUIRE_EQ(m1.data, NULL);

  free(m2.data);
  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, enable_disable_sending) {
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  dynmq_disable_sending(dq);

  REQUIRE_EQ(dynmq_send_zc(dq, &m1), ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  dynmq_enable_sending(dq);

  REQUIRE_EQ(dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  c_message_t m2 = {.data = NULL, .size = 0};

  REQUIRE_EQ(dynmq_recv_zc(dq, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  free(m2.data);
  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, timed_recv_null_timeout) {
  // NULL timeout must return ccol_invalid_args without crashing.
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  c_message_t m = {.data = NULL, .size = 0};
  REQUIRE_EQ(dynmq_timed_recv_zc(dq, &m, NULL), ccol_invalid_args);

  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, recv_drains_successfully_after_disable) {
  // Messages already in the queue must still be receivable after sending is
  // disabled. Once the queue is empty a timed recv must time out rather than
  // return ccol_not_permitted (disabling only affects senders).
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  c_message_t m = {.data = NULL, .size = 0};
  REQUIRE_EQ(dynmq_send_zc(dq, &m), ccol_success);
  REQUIRE_EQ(dynmq_send_zc(dq, &m), ccol_success);

  dynmq_disable_sending(dq);

  // Existing messages must still drain successfully.
  REQUIRE_EQ(dynmq_recv_zc(dq, &m), ccol_success);
  REQUIRE_EQ(dynmq_recv_zc(dq, &m), ccol_success);

  // Queue is now empty with sending disabled: timed recv must time out,
  // not return ccol_not_permitted.
  struct timespec timeout = {.tv_sec = 0, .tv_nsec = 50000000};  // 50 ms
  REQUIRE_EQ(dynmq_timed_recv_zc(dq, &m, &timeout), ccol_timed_out);

  dynamic_queue_destroy(dq);
}

typedef struct {
  dynamic_queue *dq;
  ccol_retval_t result;
} dq_disable_recv_args;

void *dq_blocking_recv_thread(void *raw) {
  dq_disable_recv_args *a = (dq_disable_recv_args *)raw;
  c_message_t m = {.data = NULL, .size = 0};
  a->result = dynmq_recv_zc(a->dq, &m);
  return NULL;
}

TEST(dynamic_queues, disable_sending_does_not_unblock_recv) {
  // A thread blocked in dynmq_recv_zc must NOT be woken by
  // dynmq_disable_sending. It must stay blocked and only return once sending
  // is re-enabled and a message arrives.
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  dq_disable_recv_args args = {.dq = dq, .result = ccol_unexpected_failure};
  pthread_t tid;
  pthread_create(&tid, NULL, dq_blocking_recv_thread, &args);

  usleep(20000);  // let the receiver block on the empty queue
  dynmq_disable_sending(dq);
  usleep(20000);  // receiver must still be blocked at this point

  // Re-enable and send a message to unblock the receiver.
  dynmq_enable_sending(dq);
  c_message_t m = {.data = NULL, .size = 0};
  REQUIRE_EQ(dynmq_send_zc(dq, &m), ccol_success);
  pthread_join(tid, NULL);

  REQUIRE_EQ(args.result, ccol_success);

  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, timed_recv_times_out_when_disabled) {
  // dynmq_timed_recv_zc must return ccol_timed_out (not ccol_not_permitted)
  // when the queue is empty and sending is disabled. The disable state must not
  // cause an early return before the deadline.
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  dynmq_disable_sending(dq);

  struct timespec timeout = {.tv_sec = 0, .tv_nsec = 100000000};  // 100 ms
  struct timespec before, after;
  c_message_t m = {.data = NULL, .size = 0};

  getWallTime(before);
  REQUIRE_EQ(dynmq_timed_recv_zc(dq, &m, &timeout), ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 100000);  // must wait out the timeout

  dynamic_queue_destroy(dq);
}

void *dq_helper_thread(void *args) {
  dynamic_queue *dq = (dynamic_queue *)args;

  c_message_t m = {.data = NULL, .size = 0};
  assert(dynmq_recv_zc(dq, &m) == ccol_success);
  assert(((char *)(m.data))[0] == 'A');
  assert(((char *)(m.data))[1] == '\0');
  free(m.data);
  m.data = NULL;

  return NULL;
}

TEST(dynamic_queues, send_and_receive_thread) {
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  pthread_t tid;
  pthread_create(&tid, NULL, dq_helper_thread, dq);

  usleep(50000);  // Let's make the receiver wait

  c_message_t m = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m.data))[0] = 'A';
  ((char *)(m.data))[1] = '\0';

  REQUIRE_EQ(dynmq_send_zc(dq, &m), ccol_success);
  REQUIRE_EQ(m.data, NULL);  // The ownership of the message is lost.

  pthread_join(tid, NULL);

  dynamic_queue_destroy(dq);
}

// CHANNEL TESTS

TEST(channels, create_fails) {
  char *err_str = NULL;

  channel *ch = channel_create_with_mprocs(0, NULL, &err_str);
  REQUIRE_EQ((void *)ch, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  ch = channel_create_with_mprocs(-1, NULL, &err_str);
  REQUIRE_EQ((void *)ch, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  ch = channel_create_with_mprocs(
      (size_t)INT32_MAX,
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = NULL, .malloc = malloc, .realloc = realloc},
      &err_str);
  REQUIRE_EQ((void *)ch, NULL);
  REQUIRE_NE((void *)err_str, NULL);
}

TEST(channels, create_and_destroy_no_mprocs) {
  char *err_str = "";

  channel *ch = channel_create_with_mprocs(1, NULL, &err_str);
  REQUIRE_NE((void *)ch, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  channel_destroy(ch);
  REQUIRE_EQ((void *)ch, NULL);
}

TEST(channels, create_and_destroy_with_mprocs) {
  char *err_str = "";

  channel *ch = channel_create_with_mprocs(
      1,
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      &err_str);
  REQUIRE_NE((void *)ch, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  channel_destroy(ch);
  REQUIRE_EQ((void *)ch, NULL);
}

void *thr_for_channels_basic_send_and_receive(void *args) {
  // Using direct assertions in helper threads
  channel *ch = (channel *)args;

  c_message_t msg = {.data = NULL, .size = 0};

  assert(chan_recv_zc(ch, &msg) == ccol_success);

  assert(*((char *)msg.data) == 'A');

  *((char *)msg.data) = 'B';

  assert(chan_send_zc(ch, &msg) == ccol_success);

  assert(msg.data == NULL);

  return NULL;
}

TEST(channels, basic_send_and_receive_no_mprocs) {
  channel *ch = channel_create_with_mprocs(1, NULL, NULL);

  pthread_t tid;
  pthread_create(&tid, NULL, thr_for_channels_basic_send_and_receive, ch);

  c_message_t m1 = {.data = malloc(sizeof(char)), .size = 1};
  *((char *)m1.data) = 'A';
  REQUIRE_EQ(chan_send_zc(ch, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(chan_recv_zc(ch, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(*((char *)m2.data), 'B');

  free(m2.data);
  pthread_join(tid, NULL);
  channel_destroy(ch);
}

TEST(channels, basic_send_and_receive_with_mprocs) {
  channel *ch = channel_create_with_mprocs(
      1,
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      NULL);

  pthread_t tid;
  pthread_create(&tid, NULL, thr_for_channels_basic_send_and_receive, ch);

  c_message_t m1 = {.data = malloc(sizeof(char)), .size = 1};
  *((char *)m1.data) = 'A';
  REQUIRE_EQ(chan_send_zc(ch, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(chan_recv_zc(ch, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(*((char *)m2.data), 'B');

  free(m2.data);
  pthread_join(tid, NULL);
  channel_destroy(ch);
}

void *thr_for_channels_msg_count(void *args) {
  // Using direct assertions in helper threads
  channel *ch = (channel *)args;

  c_message_t msg = {.data = NULL, .size = 0};

  for (size_t i = 3; i > 0; --i) {
    assert(chan_msg_count(ch, owner_to_workers) == i);
    chan_recv_zc(ch, &msg);
    assert(chan_msg_count(ch, owner_to_workers) == i - 1);
  }

  for (size_t i = 0; i < 3; ++i) {
    assert(chan_msg_count(ch, workers_to_owner) == i);
    chan_send_zc(ch, &msg);
    assert(chan_msg_count(ch, workers_to_owner) == i + 1);
  }

  return NULL;
}

TEST(channels, msg_count) {
  channel *ch = channel_create_with_mprocs(3, NULL, NULL);

  c_message_t m1 = {.data = NULL, .size = 0};

  for (size_t i = 0; i < 3; ++i) {
    REQUIRE_EQ(chan_msg_count(ch, owner_to_workers), i);
    chan_send_zc(ch, &m1);
    REQUIRE_EQ(chan_msg_count(ch, owner_to_workers), i + 1);
  }

  pthread_t tid;
  pthread_create(&tid, NULL, thr_for_channels_msg_count, ch);

  usleep(100000);

  for (size_t i = 3; i > 0; --i) {
    REQUIRE_EQ(chan_msg_count(ch, workers_to_owner), i);
    chan_recv_zc(ch, &m1);
    REQUIRE_EQ(chan_msg_count(ch, workers_to_owner), i - 1);
  }

  pthread_join(tid, NULL);
  channel_destroy(ch);
}

void *thr_for_channels_try_send_and_try_receive(void *args) {
  channel *ch = (channel *)args;

  usleep(50000);  // 50 msecs

  c_message_t msg = {.data = NULL, .size = 0};
  assert(chan_try_recv_zc(ch, &msg) == ccol_success);
  assert(msg.data != NULL);
  assert(*((char *)msg.data) == 'A');

  *((char *)msg.data) = 'B';

  c_message_t m2 = {.data = NULL, .size = 0};
  assert(chan_try_recv_zc(ch, &m2) == ccol_container_empty);
  assert(m2.data == NULL);

  assert(chan_try_send_zc(ch, &msg) == ccol_success);
  assert(msg.data == NULL);

  m2 = (c_message_t){.data = malloc(sizeof(char)), .size = 1};
  assert(chan_try_send_zc(ch, &m2) == ccol_container_full);
  assert(m2.data != NULL);
  free(m2.data);

  return NULL;
}

TEST(channels, try_send_and_try_receive) {
  channel *ch = channel_create_with_mprocs(1, NULL, NULL);

  pthread_t tid;
  pthread_create(&tid, NULL, thr_for_channels_try_send_and_try_receive, ch);

  c_message_t m1 = {.data = malloc(sizeof(char)), .size = 1};
  *(char *)m1.data = 'A';
  REQUIRE_EQ(chan_try_send_zc(ch, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  m1.data = malloc(sizeof(char));
  m1.size = 1;
  REQUIRE_EQ(chan_try_send_zc(ch, &m1), ccol_container_full);
  REQUIRE_NE(m1.data, NULL);
  free(m1.data);
  m1.data = NULL;

  usleep(100000);  // 100 msecs

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(chan_try_recv_zc(ch, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(*(char *)m2.data, 'B');

  REQUIRE_EQ(chan_try_recv_zc(ch, &m1), ccol_container_empty);
  REQUIRE_EQ(m1.data, NULL);

  free(m2.data);
  pthread_join(tid, NULL);
  channel_destroy(ch);
}

void *thr_for_channels_timed_send_and_timed_receive(void *args) {
  channel *ch = (channel *)args;

  struct timespec timeout;
  timeout.tv_sec = 0;
  timeout.tv_nsec = 10000000;  // 10 msecs

  usleep(40000);  // 40 msecs

  struct timespec before;
  struct timespec after;

  c_message_t msg = {.data = NULL, .size = 0};
  getWallTime(before);
  assert(chan_timed_recv_zc(ch, &msg, &timeout) == ccol_success);
  getWallTime(after);
  assert(diffTimeUSec(before, after) < 4000);
  assert(msg.data != NULL);
  assert(*((char *)msg.data) == 'A');

  *((char *)msg.data) = 'B';

  c_message_t m2 = {.data = NULL, .size = 0};
  getWallTime(before);
  assert(chan_timed_recv_zc(ch, &m2, &timeout) == ccol_timed_out);
  getWallTime(after);
  assert(diffTimeUSec(before, after) >= 10000);
  assert(m2.data == NULL);

  getWallTime(before);
  assert(chan_timed_send_zc(ch, &msg, &timeout) == ccol_success);
  getWallTime(after);
  assert(diffTimeUSec(before, after) < 4000);
  assert(msg.data == NULL);

  m2 = (c_message_t){.data = malloc(sizeof(char)), .size = 1};
  getWallTime(before);
  assert(chan_timed_send_zc(ch, &m2, &timeout) == ccol_timed_out);
  getWallTime(after);
  assert(diffTimeUSec(before, after) >= 10000);
  assert(m2.data != NULL);
  free(m2.data);

  return NULL;
}

TEST(channels, timed_send_and_timed_receive) {
  channel *ch = channel_create_with_mprocs(1, NULL, NULL);

  pthread_t tid;
  pthread_create(&tid, NULL, thr_for_channels_timed_send_and_timed_receive, ch);

  c_message_t m1 = {.data = malloc(sizeof(char)), .size = 1};
  *(char *)m1.data = 'A';

  struct timespec before;
  struct timespec after;

  struct timespec timeout;
  timeout.tv_sec = 0;
  timeout.tv_nsec = 10000000;  // 10 msecs

  getWallTime(before);
  REQUIRE_EQ(chan_timed_send_zc(ch, &m1, &timeout), ccol_success);
  getWallTime(after);
  REQUIRE_LT(diffTimeUSec(before, after), 4000);
  REQUIRE_EQ(m1.data, NULL);

  m1.data = malloc(sizeof(char));
  m1.size = 1;
  getWallTime(before);
  REQUIRE_EQ(chan_timed_send_zc(ch, &m1, &timeout), ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 10000);
  REQUIRE_NE(m1.data, NULL);
  free(m1.data);
  m1.data = NULL;

  usleep(90000);  // 90 msecs

  c_message_t m2 = {.data = NULL, .size = 0};
  getWallTime(before);
  REQUIRE_EQ(chan_timed_recv_zc(ch, &m2, &timeout), ccol_success);
  getWallTime(after);
  REQUIRE_LT(diffTimeUSec(before, after), 4000);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(*(char *)m2.data, 'B');

  getWallTime(before);
  REQUIRE_EQ(chan_timed_recv_zc(ch, &m1, &timeout), ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 10000);
  REQUIRE_EQ(m1.data, NULL);

  free(m2.data);
  pthread_join(tid, NULL);
  channel_destroy(ch);
}

void *thr_for_enable_disable_sending(void *args) {
  channel *ch = (channel *)args;

  c_message_t msg = {.data = NULL, .size = 0};
  assert(chan_recv_zc(ch, &msg) == ccol_success);
  assert(msg.data != NULL);
  assert(*((char *)msg.data) == 'A');
  *((char *)msg.data) = 'B';

  chan_disable_sending(ch, workers_to_owner);
  assert(chan_send_zc(ch, &msg) == ccol_not_permitted);
  assert(msg.data != NULL);

  chan_enable_sending(ch, workers_to_owner);
  assert(chan_send_zc(ch, &msg) == ccol_success);
  assert(msg.data == NULL);

  return NULL;
}

TEST(channels, enable_disable_sending) {
  channel *ch = channel_create_with_mprocs(1, NULL, NULL);

  pthread_t tid;
  pthread_create(&tid, NULL, thr_for_enable_disable_sending, ch);

  c_message_t m1 = {.data = malloc(sizeof(char)), .size = 1};
  m1.size = 1;
  ((char *)(m1.data))[0] = 'A';

  chan_disable_sending(ch, owner_to_workers);

  REQUIRE_EQ(chan_send_zc(ch, &m1), ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  chan_enable_sending(ch, owner_to_workers);

  REQUIRE_EQ(chan_send_zc(ch, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  c_message_t m2 = {.data = NULL, .size = 0};

  REQUIRE_EQ(chan_recv_zc(ch, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(*(char *)m2.data, 'B');

  free(m2.data);
  pthread_join(tid, NULL);
  channel_destroy(ch);
}

// CCOL_SELECT TESTS

// --- helpers ---

typedef struct {
  circular_queue *cq;
  int delay_us;
  int value;
} sel_circq_args;

typedef struct {
  dynamic_queue *dq;
  int delay_us;
  int value;
} sel_dynq_args;

typedef struct {
  circular_queue *cq0;
  circular_queue *cq1;
  int delay_us;
  int value;
} sel_disable_reenable_args;

typedef struct {
  channel *ch;
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
  assert(circq_send_zc(a->cq, &msg) == ccol_success);
  return NULL;
}

static void *thr_send_to_dynq(void *arg) {
  sel_dynq_args *a = (sel_dynq_args *)arg;
  usleep((useconds_t)a->delay_us);
  int *data = malloc(sizeof(int));
  assert(data);
  *data = a->value;
  c_message_t msg = {.data = data, .size = sizeof(int)};
  assert(dynmq_send_zc(a->dq, &msg) == ccol_success);
  return NULL;
}

/* Disables both queues, waits another delay, then re-enables cq0 and sends
 * one message to it.  Used to verify ccol_select stays blocked through the
 * disable phase and only wakes on the subsequent message. */
static void *thr_disable_then_reenable_and_send(void *arg) {
  sel_disable_reenable_args *a = (sel_disable_reenable_args *)arg;
  usleep((useconds_t)a->delay_us);
  circq_disable_sending(a->cq0);
  circq_disable_sending(a->cq1);
  usleep((useconds_t)a->delay_us);
  circq_enable_sending(a->cq0);
  int *data = malloc(sizeof(int));
  assert(data);
  *data = a->value;
  c_message_t msg = {.data = data, .size = sizeof(int)};
  assert(circq_send_zc(a->cq0, &msg) == ccol_success);
  return NULL;
}

static void *thr_send_via_channel(void *arg) {
  sel_chan_args *a = (sel_chan_args *)arg;
  usleep((useconds_t)a->delay_us);
  int *data = malloc(sizeof(int));
  assert(data);
  *data = a->value;
  c_message_t msg = {.data = data, .size = sizeof(int)};
  assert(chan_send_zc(a->ch, &msg) == ccol_success);
  return NULL;
}

// --- tests ---

TEST(ccol_select, returns_invalid_args_on_bad_inputs) {
  circular_queue *cq = circular_queue_create(4, NULL);

  size_t idx = 0;
  ccol_selectable sel = selectable_from_circq(cq, ccol_select_read);

  REQUIRE_EQ(ccol_select(NULL, 1, &sel), ccol_invalid_args);
  REQUIRE_EQ(ccol_select(&idx, 0, &sel), ccol_invalid_args);
  REQUIRE_EQ(ccol_select(&idx, 1, NULL), ccol_invalid_args);

  ccol_selectable null_sel = selectable_from_circq(NULL, ccol_select_read);
  REQUIRE_EQ(ccol_select(&idx, 1, &null_sel), ccol_invalid_args);

  circular_queue_destroy(cq);
}

TEST(ccol_select, receives_from_first_queue_when_message_already_present) {
  circular_queue *q0 = circular_queue_create(4, NULL);
  circular_queue *q1 = circular_queue_create(4, NULL);

  int *data = malloc(sizeof(int));
  *data = 7;
  c_message_t send_msg = {.data = data, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(q0, &send_msg), ccol_success);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_circq(q0, ccol_select_read),
                            selectable_from_circq(q1, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 0);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_try_recv_zc(q0, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 7);

  free(recv_msg.data);
  circular_queue_destroy(q0);
  circular_queue_destroy(q1);
}

TEST(ccol_select, receives_from_second_queue_when_message_already_present) {
  circular_queue *q0 = circular_queue_create(4, NULL);
  circular_queue *q1 = circular_queue_create(4, NULL);

  int *data = malloc(sizeof(int));
  *data = 42;
  c_message_t send_msg = {.data = data, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(q1, &send_msg), ccol_success);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_circq(q0, ccol_select_read),
                            selectable_from_circq(q1, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 1);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_try_recv_zc(q1, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 42);

  free(recv_msg.data);
  circular_queue_destroy(q0);
  circular_queue_destroy(q1);
}

TEST(ccol_select, blocks_until_message_arrives_on_circq) {
  circular_queue *q0 = circular_queue_create(4, NULL);
  circular_queue *q1 = circular_queue_create(4, NULL);

  sel_circq_args args = {.cq = q1, .delay_us = 15000, .value = 99};
  pthread_t tid;
  pthread_create(&tid, NULL, thr_send_to_circq, &args);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_circq(q0, ccol_select_read),
                            selectable_from_circq(q1, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 1);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_try_recv_zc(q1, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 99);

  free(recv_msg.data);
  pthread_join(tid, NULL);
  circular_queue_destroy(q0);
  circular_queue_destroy(q1);
}

TEST(ccol_select, timed_returns_timed_out_when_all_queues_disabled) {
  // Disabling sending must not affect read-direction waiters: ccol_select_timed
  // must wait out the full timeout rather than returning ccol_not_permitted.
  circular_queue *q0 = circular_queue_create(4, NULL);
  circular_queue *q1 = circular_queue_create(4, NULL);

  circq_disable_sending(q0);
  circq_disable_sending(q1);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_timed_va(&idx, 50 /* ms */,
                                  selectable_from_circq(q0, ccol_select_read),
                                  selectable_from_circq(q1, ccol_select_read)),
             ccol_timed_out);

  circular_queue_destroy(q0);
  circular_queue_destroy(q1);
}

TEST(ccol_select,
     stays_blocked_through_disable_then_wakes_after_reenable_and_send) {
  // ccol_select must remain blocked when sending is disabled on all queues.
  // Once sending is re-enabled and a message arrives, it must return success.
  circular_queue *q0 = circular_queue_create(4, NULL);
  circular_queue *q1 = circular_queue_create(4, NULL);

  sel_disable_reenable_args args = {
      .cq0 = q0, .cq1 = q1, .delay_us = 15000, .value = 42};
  pthread_t tid;
  pthread_create(&tid, NULL, thr_disable_then_reenable_and_send, &args);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_circq(q0, ccol_select_read),
                            selectable_from_circq(q1, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 0);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_try_recv_zc(q0, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 42);
  free(recv_msg.data);

  pthread_join(tid, NULL);
  circular_queue_destroy(q0);
  circular_queue_destroy(q1);
}

TEST(ccol_select, blocks_until_message_arrives_on_dynq) {
  circular_queue *q0 = circular_queue_create(4, NULL);
  dynamic_queue *dq = dynamic_queue_create(NULL);

  sel_dynq_args args = {.dq = dq, .delay_us = 15000, .value = 55};
  pthread_t tid;
  pthread_create(&tid, NULL, thr_send_to_dynq, &args);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_circq(q0, ccol_select_read),
                            selectable_from_dynq(dq, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 1);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(dynmq_try_recv_zc(dq, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 55);

  free(recv_msg.data);
  pthread_join(tid, NULL);
  circular_queue_destroy(q0);
  dynamic_queue_destroy(dq);
}

TEST(ccol_select, channel_direction_resolved_correctly_for_owner_thread) {
  channel *ch = channel_create_with_mprocs(4, NULL, NULL);

  /* Worker thread sends via chan_send_zc, which routes to workers_to_owner_cq.
   * selectable_from_chan() called from the owner thread here also
   * resolves to workers_to_owner_cq, so ccol_select watches the correct queue.
   */
  sel_chan_args args = {.ch = ch, .delay_us = 15000, .value = 77};
  pthread_t tid;
  pthread_create(&tid, NULL, thr_send_via_channel, &args);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_chan(ch, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 0);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(chan_try_recv_zc(ch, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 77);

  free(recv_msg.data);
  pthread_join(tid, NULL);
  channel_destroy(ch);
}

// --- write-wait helpers ---

typedef struct {
  circular_queue *cq;
  int delay_us;
} sel_recv_circq_args;

static void *thr_recv_from_circq(void *arg) {
  sel_recv_circq_args *a = (sel_recv_circq_args *)arg;
  usleep((useconds_t)a->delay_us);
  c_message_t msg = {.data = NULL, .size = 0};
  assert(circq_recv_zc(a->cq, &msg) == ccol_success);
  free(msg.data);
  return NULL;
}

typedef struct {
  circular_queue *cq;
  int delay_us;
} sel_enable_circq_args;

static void *thr_enable_circq_sending(void *arg) {
  sel_enable_circq_args *a = (sel_enable_circq_args *)arg;
  usleep((useconds_t)a->delay_us);
  circq_enable_sending(a->cq);
  return NULL;
}

typedef struct {
  dynamic_queue *dq;
  int delay_us;
} sel_enable_dynq_args;

static void *thr_enable_dynq_sending(void *arg) {
  sel_enable_dynq_args *a = (sel_enable_dynq_args *)arg;
  usleep((useconds_t)a->delay_us);
  dynmq_enable_sending(a->dq);
  return NULL;
}

// --- write-wait tests ---

TEST(ccol_select, write_circq_writable_immediately) {
  circular_queue *cq = circular_queue_create(4, NULL);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_circq(cq, ccol_select_write)),
             ccol_success);
  REQUIRE_EQ(idx, 0);

  circular_queue_destroy(cq);
}

TEST(ccol_select, write_circq_blocks_until_reader_frees_space) {
  circular_queue *cq = circular_queue_create(2, NULL);

  /* Fill the queue to capacity */
  int *d0 = malloc(sizeof(int));
  assert(d0);
  *d0 = 10;
  int *d1 = malloc(sizeof(int));
  assert(d1);
  *d1 = 20;
  c_message_t m0 = {.data = d0, .size = sizeof(int)};
  c_message_t m1 = {.data = d1, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(cq, &m0), ccol_success);
  REQUIRE_EQ(circq_send_zc(cq, &m1), ccol_success);

  sel_recv_circq_args args = {.cq = cq, .delay_us = 15000};
  pthread_t tid;
  pthread_create(&tid, NULL, thr_recv_from_circq, &args);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_circq(cq, ccol_select_write)),
             ccol_success);
  REQUIRE_EQ(idx, 0);

  pthread_join(tid, NULL);

  /* Drain the remaining message to satisfy destroy's assert */
  c_message_t drain = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_recv_zc(cq, &drain), ccol_success);
  free(drain.data);

  circular_queue_destroy(cq);
}

TEST(ccol_select, write_circq_wakes_when_sending_reenabled) {
  circular_queue *cq = circular_queue_create(4, NULL);
  circq_disable_sending(cq);

  sel_enable_circq_args args = {.cq = cq, .delay_us = 15000};
  pthread_t tid;
  pthread_create(&tid, NULL, thr_enable_circq_sending, &args);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_circq(cq, ccol_select_write)),
             ccol_success);
  REQUIRE_EQ(idx, 0);

  pthread_join(tid, NULL);
  circular_queue_destroy(cq);
}

TEST(ccol_select, write_dynq_writable_immediately) {
  dynamic_queue *dq = dynamic_queue_create(NULL);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_dynq(dq, ccol_select_write)),
             ccol_success);
  REQUIRE_EQ(idx, 0);

  dynamic_queue_destroy(dq);
}

TEST(ccol_select, write_dynq_wakes_when_sending_reenabled) {
  dynamic_queue *dq = dynamic_queue_create(NULL);
  dynmq_disable_sending(dq);

  sel_enable_dynq_args args = {.dq = dq, .delay_us = 15000};
  pthread_t tid;
  pthread_create(&tid, NULL, thr_enable_dynq_sending, &args);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_dynq(dq, ccol_select_write)),
             ccol_success);
  REQUIRE_EQ(idx, 0);

  pthread_join(tid, NULL);
  dynamic_queue_destroy(dq);
}

TEST(ccol_select, write_mixed_full_circq_and_readable_circq) {
  /* q_write is full; q_read is empty and will receive a message.
   * ccol_select should pick up q_read (read-direction) first. */
  circular_queue *q_write = circular_queue_create(1, NULL);
  circular_queue *q_read = circular_queue_create(4, NULL);

  int *fill = malloc(sizeof(int));
  assert(fill);
  *fill = 42;
  c_message_t fill_msg = {.data = fill, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(q_write, &fill_msg), ccol_success);

  sel_circq_args args = {.cq = q_read, .delay_us = 15000, .value = 88};
  pthread_t tid;
  pthread_create(&tid, NULL, thr_send_to_circq, &args);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, selectable_from_circq(q_write, ccol_select_write),
                     selectable_from_circq(q_read, ccol_select_read)),
      ccol_success);
  REQUIRE_EQ(idx, 1);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_try_recv_zc(q_read, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 88);

  free(recv_msg.data);
  pthread_join(tid, NULL);

  /* Drain q_write's fill message */
  c_message_t drain = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_recv_zc(q_write, &drain), ccol_success);
  free(drain.data);

  circular_queue_destroy(q_write);
  circular_queue_destroy(q_read);
}

TEST(ccol_select, write_channel_owner_resolves_send_direction) {
  channel *ch = channel_create_with_mprocs(4, NULL, NULL);

  /* Owner calls selectable_from_chan with ccol_select_write: resolves
   * to owner_to_workers_cq.  That queue is empty and writable, so ccol_select
   * must return immediately. */
  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_chan(ch, ccol_select_write)),
             ccol_success);
  REQUIRE_EQ(idx, 0);

  channel_destroy(ch);
}

// --- fd selectable helpers ---

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

// --- fd selectable tests ---

TEST(ccol_select, fd_readable_immediately) {
  /* Write data to the pipe before calling ccol_select; it must return without
   * blocking and report the correct index. The caller then reads the fd
   * itself. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  int val = 42;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_fd(pfd[0], ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 0);
  int got = 0;
  REQUIRE_EQ((ssize_t)sizeof(got), read(pfd[0], &got, sizeof(got)));
  REQUIRE_EQ(got, 42);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_select, fd_blocks_until_data_arrives) {
  /* Thread writes to pipe after a delay; ccol_select must block and then wake
   * up when data arrives. The caller then reads the fd itself. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  sel_fd_write_args args = {.write_fd = pfd[1], .delay_us = 15000, .value = 77};
  pthread_t tid;
  pthread_create(&tid, NULL, thr_write_to_fd, &args);

  size_t idx = 99;
  struct timespec before, after;
  getWallTime(before);
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_fd(pfd[0], ccol_select_read)),
             ccol_success);
  getWallTime(after);
  REQUIRE_EQ(idx, 0);
  REQUIRE_GE(diffTimeUSec(before, after), 10000); /* actually blocked */

  int got = 0;
  REQUIRE_EQ((ssize_t)sizeof(got), read(pfd[0], &got, sizeof(got)));
  REQUIRE_EQ(got, 77);

  pthread_join(tid, NULL);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_select, fd_writable_immediately) {
  /* A fresh pipe write-end is always writable; must return without blocking. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, selectable_from_fd(pfd[1], ccol_select_write)),
      ccol_success);
  REQUIRE_EQ(idx, 0);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_select, fd_and_queue_fd_wins) {
  /* Pipe already has data; circular queue is empty.  fd must win at index 0. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  int val = 55;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

  circular_queue *cq = circular_queue_create(4, NULL);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_fd(pfd[0], ccol_select_read),
                            selectable_from_circq(cq, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 0);
  int got = 0;
  REQUIRE_EQ((ssize_t)sizeof(got), read(pfd[0], &got, sizeof(got)));
  REQUIRE_EQ(got, 55);

  close(pfd[0]);
  close(pfd[1]);
  circular_queue_destroy(cq);
}

TEST(ccol_select, fd_and_queue_queue_wins) {
  /* Pipe has no data; a thread sends to the queue after a delay.  The queue
   * must win, demonstrating that the eventfd bridge correctly wakes epoll_wait
   * for a queue event in a mixed fd+queue selectable array. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  circular_queue *cq = circular_queue_create(4, NULL);

  sel_circq_args args = {.cq = cq, .delay_us = 15000, .value = 33};
  pthread_t tid;
  pthread_create(&tid, NULL, thr_send_to_circq, &args);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_fd(pfd[0], ccol_select_read),
                            selectable_from_circq(cq, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 1);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_try_recv_zc(cq, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 33);

  free(recv_msg.data);
  pthread_join(tid, NULL);
  close(pfd[0]);
  close(pfd[1]);
  circular_queue_destroy(cq);
}

TEST(ccol_select, fd_and_dynq_dynq_wins) {
  /* Same as above but with a dynamic_queue, ensuring the eventfd bridge works
   * for dynq selectables in epoll mode. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  dynamic_queue *dq = dynamic_queue_create(NULL);

  sel_dynq_args args = {.dq = dq, .delay_us = 15000, .value = 99};
  pthread_t tid;
  pthread_create(&tid, NULL, thr_send_to_dynq, &args);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_fd(pfd[0], ccol_select_read),
                            selectable_from_dynq(dq, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 1);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(dynmq_try_recv_zc(dq, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 99);

  free(recv_msg.data);
  pthread_join(tid, NULL);
  close(pfd[0]);
  close(pfd[1]);
  dynamic_queue_destroy(dq);
}

TEST(ccol_select, fd_invalid_fd_returns_invalid_args) {
  size_t idx = 99;
  ccol_selectable bad = selectable_from_fd(-1, ccol_select_read);
  REQUIRE_EQ(ccol_select(&idx, 1, &bad), ccol_invalid_args);
}

TEST(ccol_select, timed_circq_returns_timed_out) {
  /* Queue is empty with writing enabled; ccol_select_timed must return
   * ccol_timed_out after the deadline, not block indefinitely. */
  circular_queue *cq = circular_queue_create(4, NULL);
  size_t idx = 99;
  REQUIRE_EQ(ccol_select_timed_va(&idx, 50 /* ms */,
                                  selectable_from_circq(cq, ccol_select_read)),
             ccol_timed_out);
  /* idx must be untouched on timeout */
  REQUIRE_EQ(idx, (size_t)99);
  circular_queue_destroy(cq);
}

TEST(ccol_select, timed_fd_returns_timed_out) {
  /* Read end of a pipe with no data written; must time out. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  size_t idx = 99;
  REQUIRE_EQ(ccol_select_timed_va(&idx, 50 /* ms */,
                                  selectable_from_fd(pfd[0], ccol_select_read)),
             ccol_timed_out);
  REQUIRE_EQ(idx, (size_t)99);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_select, timed_poll_zero_ms_circq_empty) {
  /* timeout_ms == 0: non-blocking poll; empty queue → immediate timed_out. */
  circular_queue *cq = circular_queue_create(4, NULL);
  size_t idx = 99;
  REQUIRE_EQ(ccol_select_timed_va(&idx, 0,
                                  selectable_from_circq(cq, ccol_select_read)),
             ccol_timed_out);
  circular_queue_destroy(cq);
}

typedef struct helper_thread_args {
  circular_queue *cq;
  c_message_t msg;
} helper_thread_args;

void *helper_thread_main(void *arg) {
  helper_thread_args *a = arg;
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 20 * 1000000L};
  nanosleep(&ts, NULL);
  circq_send_zc(a->cq, &a->msg);
  return NULL;
}

TEST(ccol_select, timed_succeeds_before_deadline) {
  /* Producer sends before the 500 ms deadline; select must return success. */
  circular_queue *cq = circular_queue_create(4, NULL);

  pthread_t tid;
  c_message_t send_msg = {.data = malloc(4), .size = 4};
  *(int *)send_msg.data = 1234;

  /* A helper thread that sleeps 20 ms then sends. */
  helper_thread_args args = {cq, send_msg};

  pthread_create(&tid, NULL, helper_thread_main, &args);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_timed_va(&idx, 500,
                                  selectable_from_circq(cq, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, (size_t)0);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_try_recv_zc(cq, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 1234);
  free(recv_msg.data);

  pthread_join(tid, NULL);
  circular_queue_destroy(cq);
}

TEST(ccol_select, timed_out_deregisters_waiter_node) {
  /* Regression test for the use-after-free bug: after ccol_select_timed times
   * out, the waiter node must be removed from the queue's waiter list before
   * the node is freed.  If deregistration is missing, a subsequent send will
   * dereference freed memory, which Valgrind or ASan would catch.  Running
   * cleanly here confirms that deregistration happens on the timeout path. */
  circular_queue *cq = circular_queue_create(4, NULL);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_timed_va(&idx, 30 /* ms */,
                                  selectable_from_circq(cq, ccol_select_read)),
             ccol_timed_out);

  /* Send to the queue AFTER the timed-out select has returned.  If the waiter
   * node was not deregistered, circq_send_zc -> notify_one_sel_waiter will
   * dereference the freed node here. */
  c_message_t msg = {.data = malloc(4), .size = 4};
  *(int *)msg.data = 42;
  REQUIRE_EQ(circq_send_zc(cq, &msg), ccol_success);

  /* Drain so the queue is empty before destroy. */
  c_message_t drain = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_recv_zc(cq, &drain), ccol_success);
  free(drain.data);

  circular_queue_destroy(cq);
}

TEST(ccol_select, write_circq_timed_out_when_sending_disabled) {
  /* A disabled queue must cause a write-direction ccol_select_timed to wait
   * out the full timeout rather than return ccol_not_permitted immediately.
   * Disabling only blocks new sends; it must not short-circuit the select. */
  circular_queue *cq = circular_queue_create(4, NULL);
  circq_disable_sending(cq);

  size_t idx = 99;
  struct timespec before, after;
  getWallTime(before);
  REQUIRE_EQ(ccol_select_timed_va(&idx, 50 /* ms */,
                                  selectable_from_circq(cq, ccol_select_write)),
             ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 50000);
  REQUIRE_EQ(idx, (size_t)99);

  circular_queue_destroy(cq);
}

TEST(ccol_select, write_dynq_timed_out_when_sending_disabled) {
  /* Same contract as above for dynamic_queue. */
  dynamic_queue *dq = dynamic_queue_create(NULL);
  dynmq_disable_sending(dq);

  size_t idx = 99;
  struct timespec before, after;
  getWallTime(before);
  REQUIRE_EQ(ccol_select_timed_va(&idx, 50 /* ms */,
                                  selectable_from_dynq(dq, ccol_select_write)),
             ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 50000);
  REQUIRE_EQ(idx, (size_t)99);

  dynamic_queue_destroy(dq);
}

// --- concurrent write-waiters helpers ---

typedef struct {
  circular_queue *cq;
  ccol_retval_t result;
} sel_write_wait_result;

static void *thr_circq_write_wait(void *arg) {
  sel_write_wait_result *a = (sel_write_wait_result *)arg;
  size_t idx = 0;
  a->result = ccol_select_timed_va(
      &idx, 500 /* ms */, selectable_from_circq(a->cq, ccol_select_write));
  return NULL;
}

TEST(ccol_select, write_circq_two_concurrent_waiters_both_wake_on_slot_free) {
  /* Regression test for the cascade bug: when two threads are simultaneously
   * waiting for write-readiness on a full capacity-1 queue, a single dequeue
   * must cascade through the waiter list and wake BOTH threads, not just the
   * head waiter. Without the fix the second waiter would time out. */
  circular_queue *cq = circular_queue_create(1, NULL);

  int *fill = malloc(sizeof(int));
  assert(fill);
  *fill = 0;
  c_message_t fill_msg = {.data = fill, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(cq, &fill_msg), ccol_success);

  sel_write_wait_result a1 = {.cq = cq, .result = ccol_unexpected_failure};
  sel_write_wait_result a2 = {.cq = cq, .result = ccol_unexpected_failure};
  pthread_t t1, t2;
  pthread_create(&t1, NULL, thr_circq_write_wait, &a1);
  pthread_create(&t2, NULL, thr_circq_write_wait, &a2);

  usleep(30000); /* let both threads register as write-waiters */

  c_message_t drain = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_recv_zc(cq, &drain), ccol_success);
  free(drain.data);

  pthread_join(t1, NULL);
  pthread_join(t2, NULL);

  REQUIRE_EQ(a1.result, ccol_success);
  REQUIRE_EQ(a2.result, ccol_success);

  circular_queue_destroy(cq);
}

// --- concurrent read-waiters helper ---

typedef struct {
  circular_queue *cq;
  ccol_retval_t result;
} sel_read_wait_result;

static void *thr_circq_read_wait(void *arg) {
  sel_read_wait_result *a = (sel_read_wait_result *)arg;
  size_t idx = 0;
  a->result = ccol_select_timed_va(
      &idx, 500 /* ms */, selectable_from_circq(a->cq, ccol_select_read));
  if (a->result == ccol_success) {
    c_message_t msg = {.data = NULL, .size = 0};
    ccol_retval_t rv = circq_try_recv_zc(a->cq, &msg);
    a->result = rv;
    if (rv == ccol_success) free(msg.data);
  }
  return NULL;
}

TEST(ccol_select,
     read_circq_two_concurrent_waiters_both_wake_on_message_available) {
  /* Regression test for the read-direction cascade: once ccol_select() stops
   * consuming internally (peek-only, mirroring the write-direction branch's
   * existing peek+forward-notify shape), a thread that finds itself ready
   * must still forward the notify to the next waiter -- otherwise, with two
   * threads simultaneously waiting to read from the same empty queue, only
   * the first ever wakes even though two messages are actually available. */
  circular_queue *cq = circular_queue_create(4, NULL);

  sel_read_wait_result a1 = {.cq = cq, .result = ccol_unexpected_failure};
  sel_read_wait_result a2 = {.cq = cq, .result = ccol_unexpected_failure};
  pthread_t t1, t2;
  pthread_create(&t1, NULL, thr_circq_read_wait, &a1);
  pthread_create(&t2, NULL, thr_circq_read_wait, &a2);

  usleep(30000); /* let both threads register as read waiters */

  int *d0 = malloc(sizeof(int));
  assert(d0);
  *d0 = 1;
  int *d1 = malloc(sizeof(int));
  assert(d1);
  *d1 = 2;
  c_message_t m0 = {.data = d0, .size = sizeof(int)};
  c_message_t m1 = {.data = d1, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(cq, &m0), ccol_success);
  REQUIRE_EQ(circq_send_zc(cq, &m1), ccol_success);

  pthread_join(t1, NULL);
  pthread_join(t2, NULL);

  REQUIRE_EQ(a1.result, ccol_success);
  REQUIRE_EQ(a2.result, ccol_success);

  circular_queue_destroy(cq);
}

// --- event_loop test helpers ---

typedef struct evl_sync_ctx {
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  int readable_count;
  int writable_count;
  int error_count;
  c_message_t last_msg;
  bool last_msg_valid;
  ccol_select_dir last_dir_seen;
  event_reg *self_reg; /* for self-removal tests */
  event_loop self_loop;
} evl_sync_ctx;

static void evl_sync_ctx_init(evl_sync_ctx *c) {
  pthread_mutex_init(&c->mtx, NULL);
  pthread_cond_init(&c->cond, NULL);
  c->readable_count = 0;
  c->writable_count = 0;
  c->error_count = 0;
  c->last_msg = (c_message_t){.data = NULL, .size = 0};
  c->last_msg_valid = false;
  c->self_reg = NULL;
  c->self_loop = NULL;
}

static void evl_sync_ctx_destroy(evl_sync_ctx *c) {
  if (c->last_msg_valid && c->last_msg.data) free(c->last_msg.data);
  pthread_mutex_destroy(&c->mtx);
  pthread_cond_destroy(&c->cond);
}

/* event_loop never performs the receive itself, for any selectable type
 * (mirrors _event_loop_run_callback's own contract): a queue-backed
 * registration must perform its own explicit circq_try_recv_zc/
 * dynmq_try_recv_zc here, exactly as a real caller would. A non-success
 * result (a concurrent consumer already claimed the message, the same
 * TOCTOU write-direction wins have always had) leaves last_msg_valid
 * false rather than asserting. */
static void evl_on_readable(event_loop loop, ccol_selectable *sel, void *arg) {
  (void)loop;
  evl_sync_ctx *c = (evl_sync_ctx *)arg;
  c_message_t msg = {.data = NULL, .size = 0};
  bool got_msg = false;
  if (sel->type == ccol_selectable_circq) {
    got_msg = (circq_try_recv_zc(sel->cq, &msg) == ccol_success);
  } else if (sel->type == ccol_selectable_dynq) {
    got_msg = (dynmq_try_recv_zc(sel->dq, &msg) == ccol_success);
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

static void evl_on_writable(event_loop loop, ccol_selectable *sel, void *arg) {
  (void)loop;
  evl_sync_ctx *c = (evl_sync_ctx *)arg;
  pthread_mutex_lock(&c->mtx);
  c->writable_count++;
  c->last_dir_seen = sel->dir;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

static void evl_on_error(event_loop loop, ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)sel;
  evl_sync_ctx *c = (evl_sync_ctx *)arg;
  pthread_mutex_lock(&c->mtx);
  c->error_count++;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

/* Self-removing on_readable: removes its own registration from inside the
 * callback, then behaves exactly like evl_on_readable. */
static void evl_on_readable_self_remove(event_loop loop, ccol_selectable *sel,
                                        void *arg) {
  evl_sync_ctx *c = (evl_sync_ctx *)arg;
  REQUIRE_EQ(event_loop_remove(c->self_loop, c->self_reg), ccol_success);
  evl_on_readable(loop, sel, arg);
}

/* Waits until *counter_field >= target or timeout_ms elapses. Returns true
 * if the target was reached before the deadline. */
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

// --- event_loop tests ---

TEST(event_loop, create_destroy) {
  char *err = NULL;
  event_loop loop = event_loop_create(8, 1, &err);
  REQUIRE_NE((void *)loop, NULL);
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)0);
  event_loop_destroy(loop);
  REQUIRE_EQ((void *)loop, NULL);
}

TEST(event_loop, create_destroy_scoped) {
  {
    event_loop_construct_scoped(loop, 8, 1);
    REQUIRE_NE((void *)loop, NULL);
  }
  /* loop was destroyed at scope exit; nothing to assert beyond "no crash,
   * clean under valgrind" (checked by the memtest target). */
}

TEST(event_loop, fd_on_readable_fires) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 1);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg *reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE((void *)reg, NULL);
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)1);

  int val = 42;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));
  /* fd selectables never get msg populated -- caller reads sel->fd itself. */
  REQUIRE_FALSE(ctx.last_msg_valid);
  char rbuf[16];
  ssize_t n = read(pfd[0], rbuf, sizeof(rbuf));
  REQUIRE_EQ(n, (ssize_t)sizeof(val));
  REQUIRE_EQ(*(int *)rbuf, 42);

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, fd_on_writable_fires) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 1);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {
      .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
  char *err = NULL;
  event_reg *reg =
      event_loop_add(loop, selectable_from_fd(pfd[1], ccol_select_write),
                     handlers, &ctx, &err);
  REQUIRE_NE((void *)reg, NULL);

  /* A fresh pipe write end is always immediately writable. */
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, 1, 2000));

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, fd_both_directions_combine_and_recombine) {
  /* A socketpair fd gives a genuinely bidirectional fd, unlike a pipe --
   * needed to register both read and write interest on the SAME fd, which
   * exercises the EPOLL_CTL_ADD-then-MOD combining path (and MOD-back-down
   * on partial removal). */
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  event_loop_construct_scoped(loop, 8, 1);

  evl_sync_ctx read_ctx, write_ctx;
  evl_sync_ctx_init(&read_ctx);
  evl_sync_ctx_init(&write_ctx);
  event_handlers_t rh = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  event_handlers_t wh = {
      .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
  char *err = NULL;
  event_reg *rreg = event_loop_add(
      loop, selectable_from_fd(sv[0], ccol_select_read), rh, &read_ctx, &err);
  REQUIRE_NE((void *)rreg, NULL);
  event_reg *wreg = event_loop_add(
      loop, selectable_from_fd(sv[0], ccol_select_write), wh, &write_ctx, &err);
  REQUIRE_NE((void *)wreg, NULL);
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)2);

  /* sv[0] is immediately writable (empty send buffer). */
  REQUIRE_TRUE(evl_wait_for(&write_ctx, &write_ctx.writable_count, 1, 2000));

  /* Remove the write direction (MOD-back-down path); read direction must
   * keep working afterward. */
  REQUIRE_EQ(event_loop_remove(loop, wreg), ccol_success);
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)1);

  int val = 7;
  REQUIRE_EQ((ssize_t)sizeof(val), write(sv[1], &val, sizeof(val)));
  REQUIRE_TRUE(evl_wait_for(&read_ctx, &read_ctx.readable_count, 1, 2000));

  event_loop_remove(loop, rreg);
  evl_sync_ctx_destroy(&read_ctx);
  evl_sync_ctx_destroy(&write_ctx);
  close(sv[0]);
  close(sv[1]);
}

TEST(event_loop, fd_simultaneous_readable_and_writable) {
  /* Both directions registered on the same fd; write from the peer so sv[0]
   * becomes readable while it is still writable -- both callbacks must fire
   * for the SAME epoll_wait batch. An earlier version of the dispatch design
   * stored a bare event_reg* in ev.data.ptr instead of the shared
   * event_entry, which would have silently delivered only one of the two. */
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  event_loop_construct_scoped(loop, 8, 1);

  evl_sync_ctx read_ctx, write_ctx;
  evl_sync_ctx_init(&read_ctx);
  evl_sync_ctx_init(&write_ctx);
  event_handlers_t rh = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  event_handlers_t wh = {
      .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
  char *err = NULL;
  event_reg *rreg = event_loop_add(
      loop, selectable_from_fd(sv[0], ccol_select_read), rh, &read_ctx, &err);
  REQUIRE_NE((void *)rreg, NULL);
  event_reg *wreg = event_loop_add(
      loop, selectable_from_fd(sv[0], ccol_select_write), wh, &write_ctx, &err);
  REQUIRE_NE((void *)wreg, NULL);

  /* Drain the initial "immediately writable" dispatch before writing data,
   * so the later wait unambiguously observes the combined batch. */
  REQUIRE_TRUE(evl_wait_for(&write_ctx, &write_ctx.writable_count, 1, 2000));

  int val = 99;
  REQUIRE_EQ((ssize_t)sizeof(val), write(sv[1], &val, sizeof(val)));

  REQUIRE_TRUE(evl_wait_for(&read_ctx, &read_ctx.readable_count, 1, 2000));
  /* sv[0] remains writable the whole time (nothing filled its send buffer),
   * so the writable callback should have fired again too. */
  REQUIRE_TRUE(evl_wait_for(&write_ctx, &write_ctx.writable_count, 2, 2000));

  event_loop_remove(loop, rreg);
  event_loop_remove(loop, wreg);
  evl_sync_ctx_destroy(&read_ctx);
  evl_sync_ctx_destroy(&write_ctx);
  close(sv[0]);
  close(sv[1]);
}

TEST(event_loop, fd_on_error_fires_for_both_directions) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  event_loop_construct_scoped(loop, 8, 1);

  evl_sync_ctx read_ctx, write_ctx;
  evl_sync_ctx_init(&read_ctx);
  evl_sync_ctx_init(&write_ctx);
  event_handlers_t rh = {
      .on_readable = NULL, .on_writable = NULL, .on_error = evl_on_error};
  event_handlers_t wh = {
      .on_readable = NULL, .on_writable = NULL, .on_error = evl_on_error};
  char *err = NULL;
  event_reg *rreg = event_loop_add(
      loop, selectable_from_fd(sv[0], ccol_select_read), rh, &read_ctx, &err);
  REQUIRE_NE((void *)rreg, NULL);
  event_reg *wreg = event_loop_add(
      loop, selectable_from_fd(sv[0], ccol_select_write), wh, &write_ctx, &err);
  REQUIRE_NE((void *)wreg, NULL);

  close(sv[1]); /* peer hangs up */

  REQUIRE_TRUE(evl_wait_for(&read_ctx, &read_ctx.error_count, 1, 2000));
  REQUIRE_TRUE(evl_wait_for(&write_ctx, &write_ctx.error_count, 1, 2000));

  event_loop_remove(loop, rreg);
  event_loop_remove(loop, wreg);
  evl_sync_ctx_destroy(&read_ctx);
  evl_sync_ctx_destroy(&write_ctx);
  close(sv[0]);
}

TEST(event_loop, fd_duplicate_direction_rejected) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 1);

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg *reg1 = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE((void *)reg1, NULL);

  err = NULL;
  event_reg *reg2 = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL, &err);
  REQUIRE_EQ((void *)reg2, NULL);
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)1);

  event_loop_remove(loop, reg1);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, fd_modify_rejects_occupied_direction) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  event_loop_construct_scoped(loop, 8, 1);

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg *rreg = event_loop_add(
      loop, selectable_from_fd(sv[0], ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE((void *)rreg, NULL);
  event_reg *wreg = event_loop_add(
      loop, selectable_from_fd(sv[0], ccol_select_write), handlers, NULL, &err);
  REQUIRE_NE((void *)wreg, NULL);

  /* Flipping rreg to write would collide with wreg. */
  REQUIRE_EQ(event_loop_modify(loop, rreg, ccol_select_write),
             ccol_not_permitted);

  event_loop_remove(loop, rreg);
  event_loop_remove(loop, wreg);
  close(sv[0]);
  close(sv[1]);
}

TEST(event_loop, fd_modify_flips_direction_and_updates_sel) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  event_loop_construct_scoped(loop, 8, 1);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {.on_readable = evl_on_readable,
                               .on_writable = evl_on_writable,
                               .on_error = NULL};
  char *err = NULL;
  event_reg *reg = event_loop_add(
      loop, selectable_from_fd(sv[0], ccol_select_write), handlers, &ctx, &err);
  REQUIRE_NE((void *)reg, NULL);
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, 1, 2000));

  REQUIRE_EQ(event_loop_modify(loop, reg, ccol_select_read), ccol_success);

  int val = 5;
  REQUIRE_EQ((ssize_t)sizeof(val), write(sv[1], &val, sizeof(val)));
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));
  REQUIRE_EQ((int)ctx.last_dir_seen, (int)ccol_select_read);

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  close(sv[0]);
  close(sv[1]);
}

TEST(event_loop, fd_modify_after_remove_returns_invalid_args) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 1);

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg *reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE((void *)reg, NULL);

  REQUIRE_EQ(event_loop_remove(loop, reg), ccol_success);
  REQUIRE_EQ(event_loop_modify(loop, reg, ccol_select_write),
             ccol_invalid_args);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, queue_circq_readable_delivers_message) {
  event_loop_construct_scoped(loop, 8, 1);
  circular_queue *cq = circular_queue_create(4, NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg *reg = event_loop_add(
      loop, selectable_from_circq(cq, ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE((void *)reg, NULL);

  int *payload = malloc(sizeof(int));
  *payload = 123;
  c_message_t msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(cq, &msg), ccol_success);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));
  REQUIRE_TRUE(ctx.last_msg_valid);
  REQUIRE_EQ(ctx.last_msg.size, sizeof(int));
  REQUIRE_EQ(*(int *)ctx.last_msg.data, 123);

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  circular_queue_destroy(cq);
}

TEST(event_loop, queue_circq_writable_fires_without_consuming) {
  event_loop_construct_scoped(loop, 8, 1);
  circular_queue *cq = circular_queue_create(1, NULL);

  /* Fill the queue so write-direction isn't immediately satisfiable. */
  int *filler = malloc(sizeof(int));
  *filler = 1;
  c_message_t fmsg = {.data = filler, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(cq, &fmsg), ccol_success);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {
      .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
  char *err = NULL;
  event_reg *reg = event_loop_add(
      loop, selectable_from_circq(cq, ccol_select_write), handlers, &ctx, &err);
  REQUIRE_NE((void *)reg, NULL);

  /* Free the slot; on_writable must fire, but must NOT have consumed
   * anything (queue still has room, not a message) -- the callback itself
   * is responsible for the actual send. */
  c_message_t recvd;
  REQUIRE_EQ(circq_recv_zc(cq, &recvd), ccol_success);
  free(recvd.data);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, 1, 2000));

  int *payload = malloc(sizeof(int));
  *payload = 55;
  c_message_t msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(circq_try_send_zc(cq, &msg), ccol_success);
  REQUIRE_EQ(circq_msg_count(cq), (size_t)1);

  c_message_t out;
  REQUIRE_EQ(circq_recv_zc(cq, &out), ccol_success);
  REQUIRE_EQ(*(int *)out.data, 55);
  free(out.data);

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  circular_queue_destroy(cq);
}

TEST(event_loop, queue_dynq_readable_delivers_message) {
  event_loop_construct_scoped(loop, 8, 1);
  dynamic_queue *dq = dynamic_queue_create(NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg *reg = event_loop_add(
      loop, selectable_from_dynq(dq, ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE((void *)reg, NULL);

  int *payload = malloc(sizeof(int));
  *payload = 321;
  c_message_t msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(dynmq_send_zc(dq, &msg), ccol_success);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));
  REQUIRE_TRUE(ctx.last_msg_valid);
  REQUIRE_EQ(*(int *)ctx.last_msg.data, 321);

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  dynamic_queue_destroy(dq);
}

typedef struct evl_chan_sender_args {
  channel *ch;
  int value;
} evl_chan_sender_args;

static void *evl_chan_sender_thread(void *arg) {
  evl_chan_sender_args *a = (evl_chan_sender_args *)arg;
  int *payload = malloc(sizeof(int));
  *payload = a->value;
  c_message_t msg = {.data = payload, .size = sizeof(int)};
  /* Called from a spawned thread, so get_thread_id() differs from the
   * channel's owner (the test thread that called channel_create) -- this
   * routes to workers_to_owner_cq, exactly what the owner-side
   * ccol_select_read registration below watches. */
  chan_send_zc(a->ch, &msg);
  return NULL;
}

TEST(event_loop, queue_channel_selectable) {
  event_loop_construct_scoped(loop, 8, 1);
  channel *ch = channel_create(4, NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  /* This thread is the channel's owner; owner reads from workers_to_owner,
   * so a worker (a separate thread) must send for the owner-side read
   * registration to fire. */
  event_reg *reg = event_loop_add(
      loop, selectable_from_chan(ch, ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE((void *)reg, NULL);

  evl_chan_sender_args sargs = {.ch = ch, .value = 88};
  pthread_t tid;
  pthread_create(&tid, NULL, evl_chan_sender_thread, &sargs);
  pthread_join(tid, NULL);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));
  REQUIRE_TRUE(ctx.last_msg_valid);
  REQUIRE_EQ(*(int *)ctx.last_msg.data, 88);

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  channel_destroy(ch);
}

TEST(event_loop, queue_persistent_across_multiple_cycles) {
  /* The core "persistent, not per-call" property: a single registration
   * keeps firing across many independent send/recv cycles without ever
   * being re-added. */
  event_loop_construct_scoped(loop, 8, 1);
  circular_queue *cq = circular_queue_create(4, NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg *reg = event_loop_add(
      loop, selectable_from_circq(cq, ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE((void *)reg, NULL);

  for (int i = 0; i < 5; i++) {
    int *payload = malloc(sizeof(int));
    *payload = i;
    c_message_t msg = {.data = payload, .size = sizeof(int)};
    REQUIRE_EQ(circq_send_zc(cq, &msg), ccol_success);
    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, i + 1, 2000));
    REQUIRE_EQ(*(int *)ctx.last_msg.data, i);
  }

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  circular_queue_destroy(cq);
}

TEST(event_loop, queue_already_pending_message_at_registration_time) {
  /* A message sent BEFORE event_loop_add is called must still be delivered
   * -- the bridge eventfd has no prior notify to rely on, so event_loop_add
   * must self-trigger when the queue is already in the target state at
   * registration time. */
  event_loop_construct_scoped(loop, 8, 1);
  circular_queue *cq = circular_queue_create(4, NULL);

  int *payload = malloc(sizeof(int));
  *payload = 999;
  c_message_t msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(cq, &msg), ccol_success);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg *reg = event_loop_add(
      loop, selectable_from_circq(cq, ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE((void *)reg, NULL);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));
  REQUIRE_EQ(*(int *)ctx.last_msg.data, 999);

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  circular_queue_destroy(cq);
}

TEST(event_loop, remove_from_within_callback) {
  event_loop_construct_scoped(loop, 8, 1);
  circular_queue *cq = circular_queue_create(4, NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ctx.self_loop = loop;
  event_handlers_t handlers = {.on_readable = evl_on_readable_self_remove,
                               .on_writable = NULL,
                               .on_error = NULL};
  char *err = NULL;
  event_reg *reg = event_loop_add(
      loop, selectable_from_circq(cq, ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE((void *)reg, NULL);
  ctx.self_reg = reg;

  int *payload = malloc(sizeof(int));
  *payload = 1;
  c_message_t msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(cq, &msg), ccol_success);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)0);

  /* A second message must NOT be delivered (registration removed itself). */
  int *payload2 = malloc(sizeof(int));
  *payload2 = 2;
  c_message_t msg2 = {.data = payload2, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(cq, &msg2), ccol_success);
  usleep(50000);
  REQUIRE_EQ(ctx.readable_count, 1);

  c_message_t drained;
  REQUIRE_EQ(circq_recv_zc(cq, &drained), ccol_success);
  free(drained.data);

  evl_sync_ctx_destroy(&ctx);
  circular_queue_destroy(cq);
}

typedef struct evl_remover_args {
  event_loop loop;
  event_reg *reg;
  int delay_us;
} evl_remover_args;

static void *evl_remover_thread(void *arg) {
  evl_remover_args *a = (evl_remover_args *)arg;
  usleep((useconds_t)a->delay_us);
  event_loop_remove(a->loop, a->reg);
  return NULL;
}

TEST(event_loop, remove_from_different_thread_concurrent_with_dispatch) {
  /* Repeated add -> notify -> concurrent-remove-from-another-thread cycles,
   * targeted stress for the refcount design; must never crash or leak
   * (verified separately under valgrind by the memtest target). */
  event_loop_construct_scoped(loop, 8, 1);
  circular_queue *cq = circular_queue_create(4, NULL);

  for (int iter = 0; iter < 50; iter++) {
    evl_sync_ctx ctx;
    evl_sync_ctx_init(&ctx);
    event_handlers_t handlers = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    char *err = NULL;
    event_reg *reg =
        event_loop_add(loop, selectable_from_circq(cq, ccol_select_read),
                       handlers, &ctx, &err);
    REQUIRE_NE((void *)reg, NULL);

    evl_remover_args rargs = {.loop = loop, .reg = reg, .delay_us = 0};
    pthread_t rtid;
    pthread_create(&rtid, NULL, evl_remover_thread, &rargs);

    int *payload = malloc(sizeof(int));
    *payload = iter;
    c_message_t msg = {.data = payload, .size = sizeof(int)};
    circq_send_zc(cq, &msg); /* may or may not be delivered; that's fine */

    pthread_join(rtid, NULL);
    evl_sync_ctx_destroy(&ctx);

    /* Drain whatever's left so the queue doesn't grow unbounded across
     * iterations (the message may not have been consumed if removal won
     * the race before dispatch). */
    c_message_t leftover;
    while (circq_try_recv_zc(cq, &leftover) == ccol_success) {
      free(leftover.data);
    }
  }

  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)0);
  circular_queue_destroy(cq);
}

TEST(event_loop, shutdown_with_pending_registrations) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  char *err = NULL;
  event_loop loop = event_loop_create(8, 1, &err);
  REQUIRE_NE((void *)loop, NULL);

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  event_reg *reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE((void *)reg, NULL);

  REQUIRE_EQ(event_loop_shutdown(loop), ccol_success);
  /* Idempotent: calling again must not hang or double-join. */
  REQUIRE_EQ(event_loop_shutdown(loop), ccol_success);

  event_loop_destroy(loop);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, destroy_while_queue_registration_pending_queue_outlives_loop) {
  /* Regression test for the destroy-path fix: freeing a queue-backed
   * event_reg without first unlinking its waiter_node from the queue's own
   * waiter list would leave the queue holding a dangling pointer, a
   * use-after-free the next time anyone sends/receives on it -- especially
   * dangerous here since the queue is intentionally NOT destroyed until
   * after the event_loop is. */
  circular_queue *cq = circular_queue_create(4, NULL);

  char *err = NULL;
  event_loop loop = event_loop_create(8, 1, &err);
  REQUIRE_NE((void *)loop, NULL);

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  event_reg *reg = event_loop_add(
      loop, selectable_from_circq(cq, ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE((void *)reg, NULL);

  /* Destroy the loop with the registration still pending -- must unlink
   * from cq's waiter list before freeing, not after. */
  event_loop_destroy(loop);

  /* The queue must still be perfectly usable afterward. */
  int *payload = malloc(sizeof(int));
  *payload = 42;
  c_message_t msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(cq, &msg), ccol_success);
  c_message_t out;
  REQUIRE_EQ(circq_recv_zc(cq, &out), ccol_success);
  REQUIRE_EQ(*(int *)out.data, 42);
  free(out.data);

  circular_queue_destroy(cq);
}

TEST(event_loop, reg_count_tracks_add_remove) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 1);

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)0);
  event_reg *reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE((void *)reg, NULL);
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)1);
  REQUIRE_EQ(event_loop_remove(loop, reg), ccol_success);
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)0);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, high_add_remove_churn_stress) {
  event_loop_construct_scoped(loop, 32, 1);
  const int n = 200;
  int pfds[200][2];
  event_reg *regs[200];

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};

  for (int i = 0; i < n; i++) {
    REQUIRE_EQ(pipe(pfds[i]), 0);
    char *err = NULL;
    regs[i] =
        event_loop_add(loop, selectable_from_fd(pfds[i][0], ccol_select_read),
                       handlers, NULL, &err);
    REQUIRE_NE((void *)regs[i], NULL);
  }
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)n);

  for (int i = 0; i < n; i++) {
    REQUIRE_EQ(event_loop_remove(loop, regs[i]), ccol_success);
    close(pfds[i][0]);
    close(pfds[i][1]);
  }
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)0);
}

TEST(event_loop, multiple_independent_instances) {
  /* No shared global state between separate event_loop instances, unlike
   * facio's process-wide singleton reactor -- including their lock-stripe
   * arrays: loop_a uses 1 stripe (the original single-lock-equivalent
   * behavior) and loop_b uses 8, deliberately different, to confirm
   * num_lock_stripes is a genuinely per-instance setting with no
   * cross-instance interference. */
  int pfd_a[2], pfd_b[2];
  REQUIRE_EQ(pipe(pfd_a), 0);
  REQUIRE_EQ(pipe(pfd_b), 0);

  event_loop_construct_scoped(loop_a, 8, 1);
  event_loop_construct_scoped(loop_b, 8, 8);

  evl_sync_ctx ctx_a, ctx_b;
  evl_sync_ctx_init(&ctx_a);
  evl_sync_ctx_init(&ctx_b);
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg *reg_a =
      event_loop_add(loop_a, selectable_from_fd(pfd_a[0], ccol_select_read),
                     handlers, &ctx_a, &err);
  REQUIRE_NE((void *)reg_a, NULL);
  event_reg *reg_b =
      event_loop_add(loop_b, selectable_from_fd(pfd_b[0], ccol_select_read),
                     handlers, &ctx_b, &err);
  REQUIRE_NE((void *)reg_b, NULL);

  int val_a = 1, val_b = 2;
  REQUIRE_EQ((ssize_t)sizeof(val_a), write(pfd_a[1], &val_a, sizeof(val_a)));
  REQUIRE_TRUE(evl_wait_for(&ctx_a, &ctx_a.readable_count, 1, 2000));
  /* loop_b's registration must not have fired for loop_a's fd. */
  REQUIRE_EQ(ctx_b.readable_count, 0);

  REQUIRE_EQ((ssize_t)sizeof(val_b), write(pfd_b[1], &val_b, sizeof(val_b)));
  REQUIRE_TRUE(evl_wait_for(&ctx_b, &ctx_b.readable_count, 1, 2000));

  event_loop_remove(loop_a, reg_a);
  event_loop_remove(loop_b, reg_b);
  evl_sync_ctx_destroy(&ctx_a);
  evl_sync_ctx_destroy(&ctx_b);
  close(pfd_a[0]);
  close(pfd_a[1]);
  close(pfd_b[0]);
  close(pfd_b[1]);
}

// --- lock-striping tests ---

TEST(event_loop, num_lock_stripes_zero_returns_null) {
  char *err = NULL;
  event_loop loop = event_loop_create(8, 0, &err);
  REQUIRE_EQ((void *)loop, NULL);
}

TEST(event_loop, fd_both_directions_combine_and_recombine_multi_stripe) {
  /* Same scenario as fd_both_directions_combine_and_recombine, but with
   * num_lock_stripes > 1: read and write directions on the SAME fd must
   * hash to (_stripe_index_for_fd is a pure function of the fd) and stay
   * serialized within the SAME stripe, so the EPOLL_CTL_ADD-then-MOD
   * combining path (and MOD-back-down on partial removal) must behave
   * identically to the single-stripe case. */
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  event_loop_construct_scoped(loop, 8, 8);

  evl_sync_ctx read_ctx, write_ctx;
  evl_sync_ctx_init(&read_ctx);
  evl_sync_ctx_init(&write_ctx);
  event_handlers_t rh = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  event_handlers_t wh = {
      .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
  char *err = NULL;
  event_reg *rreg = event_loop_add(
      loop, selectable_from_fd(sv[0], ccol_select_read), rh, &read_ctx, &err);
  REQUIRE_NE((void *)rreg, NULL);
  event_reg *wreg = event_loop_add(
      loop, selectable_from_fd(sv[0], ccol_select_write), wh, &write_ctx, &err);
  REQUIRE_NE((void *)wreg, NULL);
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)2);

  REQUIRE_TRUE(evl_wait_for(&write_ctx, &write_ctx.writable_count, 1, 2000));

  REQUIRE_EQ(event_loop_remove(loop, wreg), ccol_success);
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)1);

  int val = 7;
  REQUIRE_EQ((ssize_t)sizeof(val), write(sv[1], &val, sizeof(val)));
  REQUIRE_TRUE(evl_wait_for(&read_ctx, &read_ctx.readable_count, 1, 2000));

  event_loop_remove(loop, rreg);
  evl_sync_ctx_destroy(&read_ctx);
  evl_sync_ctx_destroy(&write_ctx);
  close(sv[0]);
  close(sv[1]);
}

TEST(event_loop, fd_modify_after_remove_returns_invalid_args_multi_stripe) {
  /* Same scenario as fd_modify_after_remove_returns_invalid_args, but with
   * num_lock_stripes > 1: event_loop_modify must still key its stripe
   * lookup off reg->stripe_idx (read lock-free, safe unconditionally for
   * any reg* the caller legitimately holds) and gracefully return
   * ccol_invalid_args, not crash or misbehave, when reg is already removed.
   *
   * Deliberately does NOT try to force reg's own underlying memory to
   * actually be freed before calling modify (e.g. by forcing several
   * reactor drain cycles to elapse first): reg is only guaranteed to
   * remain valid, stale, memory across the *short* window before the next
   * drain claims it (see _event_loop_defer_reg_free's doc comment) --
   * forcing extra drain cycles here would let that window close and make
   * reg itself genuinely freed, which is no longer "gracefully handled
   * stale reg*" territory but real, out-of-contract use-after-free. An
   * earlier draft of this test tried exactly that and was caught by
   * valgrind flagging a real UAF -- correctly so, since the bug was in the
   * test's own premise, not in event_loop_modify/_remove. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 8);

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg *reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE((void *)reg, NULL);

  REQUIRE_EQ(event_loop_remove(loop, reg), ccol_success);
  REQUIRE_EQ(event_loop_modify(loop, reg, ccol_select_write),
             ccol_invalid_args);

  close(pfd[0]);
  close(pfd[1]);
}

typedef struct {
  event_loop loop;
  int thread_id;
  int iterations;
} evl_stripe_stress_args;

static void *evl_stripe_stress_fd_worker(void *arg) {
  evl_stripe_stress_args *a = (evl_stripe_stress_args *)arg;
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  for (int i = 0; i < a->iterations; i++) {
    int pfd[2];
    if (pipe(pfd) != 0) continue;
    evl_sync_ctx ctx;
    evl_sync_ctx_init(&ctx);
    char *err = NULL;
    event_reg *reg =
        event_loop_add(a->loop, selectable_from_fd(pfd[0], ccol_select_read),
                       handlers, &ctx, &err);
    if (reg) {
      int val = a->thread_id;
      (void)write(pfd[1], &val, sizeof(val));
      evl_wait_for(&ctx, &ctx.readable_count, 1, 2000);
      event_loop_remove(a->loop, reg);
    }
    evl_sync_ctx_destroy(&ctx);
    close(pfd[0]);
    close(pfd[1]);
  }
  return NULL;
}

TEST(event_loop, multi_threaded_multi_fd_stress_with_stripes) {
  /* Many threads, many DISTINCT fds (unlike high_add_remove_churn_stress,
   * which is single-threaded and stripe-count 1), num_lock_stripes well
   * above 1 -- exercises genuinely concurrent event_loop_add/_remove/
   * dispatch across different stripes running in parallel, not just churn
   * on a single lock. */
  event_loop_construct_scoped(loop, 32, 16);
  const int n_threads = 8;
  const int iterations = 25;
  pthread_t threads[8];
  evl_stripe_stress_args args[8];

  for (int i = 0; i < n_threads; i++) {
    args[i].loop = loop;
    args[i].thread_id = i;
    args[i].iterations = iterations;
    pthread_create(&threads[i], NULL, evl_stripe_stress_fd_worker, &args[i]);
  }
  for (int i = 0; i < n_threads; i++) {
    pthread_join(threads[i], NULL);
  }

  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)0);
}

static void *evl_stripe_stress_queue_worker(void *arg) {
  evl_stripe_stress_args *a = (evl_stripe_stress_args *)arg;
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  for (int i = 0; i < a->iterations; i++) {
    circular_queue *cq = circular_queue_create(4, NULL);
    evl_sync_ctx ctx;
    evl_sync_ctx_init(&ctx);
    char *err = NULL;
    event_reg *reg =
        event_loop_add(a->loop, selectable_from_circq(cq, ccol_select_read),
                       handlers, &ctx, &err);
    if (reg) {
      int *payload = malloc(sizeof(int));
      *payload = a->thread_id;
      c_message_t msg = {.data = payload, .size = sizeof(int)};
      circq_send_zc(cq, &msg);
      evl_wait_for(&ctx, &ctx.readable_count, 1, 2000);
      event_loop_remove(a->loop, reg);
    }
    evl_sync_ctx_destroy(&ctx);
    circular_queue_destroy(cq);
  }
  return NULL;
}

TEST(event_loop, multi_threaded_multi_queue_stress_with_stripes) {
  /* Queue-selectable equivalent of the fd stress test above: many distinct
   * queues -- hence many distinct bridge_efds, each round-robin-assigned to
   * a stripe via loop->next_queue_stripe -- registered/removed concurrently
   * across threads. */
  event_loop_construct_scoped(loop, 32, 16);
  const int n_threads = 8;
  const int iterations = 25;
  pthread_t threads[8];
  evl_stripe_stress_args args[8];

  for (int i = 0; i < n_threads; i++) {
    args[i].loop = loop;
    args[i].thread_id = i;
    args[i].iterations = iterations;
    pthread_create(&threads[i], NULL, evl_stripe_stress_queue_worker, &args[i]);
  }
  for (int i = 0; i < n_threads; i++) {
    pthread_join(threads[i], NULL);
  }

  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)0);
}

TEST(event_loop, destroy_frees_registrations_across_multiple_stripes) {
  /* Registers enough distinct fds and queues, with num_lock_stripes > 1, to
   * spread live registrations across multiple stripes, then destroys the
   * loop WITHOUT removing them first -- exercises __event_loop_destroy's
   * per-stripe walk (fd_index chmap + queue_regs_head unlinking) for every
   * stripe, not just stripe 0. */
  const int n = 20;
  int pfds[20][2];
  circular_queue *queues[20];
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};

  {
    event_loop_construct_scoped(loop, 32, 8);
    char *err = NULL;

    for (int i = 0; i < n; i++) {
      REQUIRE_EQ(pipe(pfds[i]), 0);
      event_reg *reg =
          event_loop_add(loop, selectable_from_fd(pfds[i][0], ccol_select_read),
                         handlers, NULL, &err);
      REQUIRE_NE((void *)reg, NULL);

      queues[i] = circular_queue_create(4, NULL);
      event_reg *qreg = event_loop_add(
          loop, selectable_from_circq(queues[i], ccol_select_read), handlers,
          NULL, &err);
      REQUIRE_NE((void *)qreg, NULL);
    }
    REQUIRE_EQ(event_loop_reg_count(loop), (size_t)(2 * n));

    /* loop destroyed here (scope exit) with every registration still live;
     * __event_loop_destroy must walk and free all of them across every
     * stripe. */
  }

  for (int i = 0; i < n; i++) {
    close(pfds[i][0]);
    close(pfds[i][1]);
    circular_queue_destroy(queues[i]);
  }
}
