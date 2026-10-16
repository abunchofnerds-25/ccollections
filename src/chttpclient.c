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
#include <chashmap.h>
#include <chttpclient.h>
#include <cthreadcomm.h>
#include <cthreadpool.h>
#include <ctype.h>
#include <cvector.h>
#include <errno.h>
#include <fcntl.h>
#include <internal/cascii.h>
#include <internal/catfork.h>
#include <internal/cdeadline.h>
#include <internal/chashinsert.h>
#include <internal/chttp1_parser.h>
#include <internal/clogderive.h>
#include <internal/cpintable.h>
#include <internal/csock.h>
#include <internal/cstrutil.h>
#include <internal/ctls.h>
#include <internal/test_helpers/chttpclient.h>
#include <internal/test_helpers/common.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ========================================================================== */
/*                         CONSTANTS                                          */
/* ========================================================================== */

#define CHTTP_MAX_REDIRECTS 50
#define CHTTP_MAX_IDLE_PER_ORIGIN 4
#define CHTTP_MAX_IDLE_TOTAL 32
/* The number of DISTINCT origin keys that the idle pool's chmap may hold an
 * entry for. It has nothing to do with CHTTP_MAX_IDLE_TOTAL and
 * CHTTP_MAX_IDLE_PER_ORIGIN above, which bound the live pooled CONNECTIONS
 * and not how many distinct origins ever had one.
 * A crawler, or a service that fans out to many hosts, talks to lots of
 * origins and never comes back to most of them. Without this cap such a
 * client keeps a chmap entry for every origin it has ever seen, for good.
 * Once the pool of an origin is capped out or aged out, nothing else removes
 * its entry, which is empty by then or belongs to an origin the client won't
 * visit again. The prune in _idle_pool_take and _async_idle_pool_take frees
 * an origin's entry only when the client goes back to THAT origin and drains
 * its pool to zero, and an origin visited exactly once never gets there.
 * When the cap is full we don't pool a connection for a genuinely new origin
 * and the map doesn't grow any more. It's the same policy as the other two
 * caps in this file (if it doesn't fit, don't pool it), so what we lose is
 * an optimisation and correctness isn't affected. */
#define CHTTP_MAX_IDLE_ORIGINS 128
#ifdef RUNNING_UNIT_TESTS
/* Lets a test override the effective CHTTP_MAX_IDLE_ORIGINS, and 0 means we
 * use the real constant. Testing the real cap of 128 directly would take 128
 * genuinely distinct origins, which is too much for an ordinary functional
 * test, so a test sets a small value here through
 * _chttpclient_set_max_idle_origins_for_tests (it sits with the other
 * white-box test helpers). */
static _Atomic size_t g_max_idle_origins_override_for_tests = 0;
#endif /* RUNNING_UNIT_TESTS */
static size_t _effective_max_idle_origins(void) {
#ifdef RUNNING_UNIT_TESTS
  size_t override = atomic_load(&g_max_idle_origins_override_for_tests);
  if (override > 0) return override;
#endif
  return CHTTP_MAX_IDLE_ORIGINS;
}
#define CHTTP_IDLE_MAX_AGE_MS 60000L
/* How long chttp_do_internal waits for a "100 Continue" interim response
 * (the server may also skip it and send its real final response). When this
 * runs out we stop waiting and send the body anyway. The value is the same
 * as the default of curl's CURLOPT_EXPECT_100_TIMEOUT_MS, and the doc
 * comment of chttp_request_t.expect_continue has the rest. */
#define CHTTP_100_CONTINUE_WAIT_US UINT64_C(1000000)
/* Happy Eyeballs' Connection Attempt Delay (RFC 8305 section 5). When a
 * host has several addresses and the attempts in flight give no result for
 * this long, we start on the next address. 250 ms is what the RFC
 * recommends and what Go's net.Dialer uses. */
#define CHTTP_CONNECT_ATTEMPT_DELAY_MS 250L
#ifdef RUNNING_UNIT_TESTS
/* A non-zero value takes the place of CHTTP_CONNECT_ATTEMPT_DELAY_MS. With a
 * long delay here a test can tell a hand-over that waits for the delay from
 * one that doesn't, by a margin no instrumentation can eat up. */
static _Atomic long g_connect_attempt_delay_ms_for_tests = 0;
/* Each attempt delay that starts while this is above 0 takes one from it and
 * lasts only 1 ms. That way a test gets its second attempt started at once
 * while that second attempt gets the long delay above. */
static _Atomic int g_connect_short_delays_for_tests = 0;
/* While this is above 0, every in-progress attempt that completes in Tier 1
 * takes one from it and is treated as ECONNREFUSED, i.e. as an attempt that
 * failed after the connect had started, the way a refusal from a remote host
 * looks. A loopback connect can complete inside connect(2) itself, so while
 * this is above 0 we count such an attempt as in progress too, otherwise the
 * hook would never see it complete. */
static _Atomic int g_connect_async_failures_for_tests = 0;

void _chttp_set_connect_attempt_delay_ms_for_tests(long ms) {
  atomic_store(&g_connect_attempt_delay_ms_for_tests, ms);
}

void _chttp_set_connect_short_delays_for_tests(int n) {
  atomic_store(&g_connect_short_delays_for_tests, n);
}

void _chttp_set_connect_async_failures_for_tests(int n) {
  atomic_store(&g_connect_async_failures_for_tests, n);
}

/* Takes one from *counter if it is above 0 and tells whether it did. */
static bool _take_one_for_tests(_Atomic int *counter) {
  int v = atomic_load(counter);
  while (v > 0)
    if (atomic_compare_exchange_weak(counter, &v, v - 1)) return true;
  return false;
}
#endif

static inline long _connect_attempt_delay_ms(void) {
#ifdef RUNNING_UNIT_TESTS
  if (_take_one_for_tests(&g_connect_short_delays_for_tests)) return 1;
  long ms = atomic_load(&g_connect_attempt_delay_ms_for_tests);
  if (ms > 0) return ms;
#endif
  return CHTTP_CONNECT_ATTEMPT_DELAY_MS;
}
/* A soft cap on the interim responses that _chttp_read_message_loop throws
 * away without telling the caller, i.e. every 1xx except the one the loop is
 * waiting for. A broken or malicious server can keep sending them forever
 * (an endless stream of "103 Early Hints", say), and since the default
 * request_timeout_us == 0 means no timeout at all, without this cap such a
 * server pins the caller's thread and its slot in the concurrency limiter
 * for good. Every other loop in this file that could repeat without bound
 * has a cap of its own (redirects, the header count and byte budgets of
 * chttp1_parser), and this one is the cap for this loop. */
#define CHTTP_MAX_INTERIM_RESPONSES 64

/* ========================================================================== */
/*                         INTERNAL TYPES                                     */
/* ========================================================================== */

/* A parsed request-target. Every field is an owned copy from the mp
 * allocator.
 *
 * A "http+unix://" URL fills is_unix and unix_socket_path instead of host,
 * port and is_ipv6 (see _parse_chttp_url), so for a unix target host is
 * NULL, port is 0 and is_ipv6 is false. origin_key is always filled, and a
 * unix target gets its own "unix://<path>" prefix there. A TCP origin_key
 * starts with "http://" or "https://", so the two can't collide and the idle
 * pool's chmaps key a unix-socket target exactly like a TCP one. */
typedef struct {
  bool is_https;
  bool is_ipv6; /* host is a raw IPv6 literal without brackets, which is
                 * what connect() and TLS want; the Host header, origin_key
                 * and a rebuilt redirect URL put the brackets back. */
  bool is_unix;
  char *unix_socket_path; /* owned, percent-decoded; NULL unless is_unix */
  char *host;
  uint16_t port;
  char *path_and_query;
  char *origin_key; /* "scheme://host:port" or "unix://<path>". We use it
                     * for SNI (TCP only) and as the idle pool's key (both
                     * kinds). */
  char *userinfo_authorization; /* Owned "Basic <b64>" built from the
                                 * "user:pass@" part of THIS URL, or NULL
                                 * when the URL has none. */
} chttp_url_t;

/* An absolute-time deadline; `active == false` means no limit. */
typedef struct {
  bool active;
  struct timespec deadline;
} chttp_deadline_t;

/* One connection, the thing the idle pool holds. We always keep it by value,
 * on the stack or inside another object, and never give it a heap block of
 * its own, so the idle pool's cvector stores it by value with no extra
 * allocation in between. */
typedef struct {
  int fd;
  ctls_conn_t *tls; /* NULL for plain HTTP */
  char *origin_key; /* owned copy, matches chttp_url_t.origin_key */
  struct timespec last_used;
  /* chttpclient.tls_generation as it was when this connection did its
   * handshake. The idle pool is keyed by the origin alone, which tells
   * nothing about the TLS policy a pooled connection was made with, so a TLS
   * connection can be reused only while this matches its client's current
   * generation (see _idle_pool_take). It means nothing for a plaintext
   * connection and we never read it there. */
  uint64_t tls_generation;
} chttp_conn_t;

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  ccol_memmgmt_procs_t *mp;
  bool oom;
  /* max_size == 0 means no limit, the default of
   * chttpclient_set_max_response_body_size. Every append checks it, and the
   * first append that would take len past max_size sets too_large instead of
   * growing the buffer, so an oversized body from a bad or malicious server
   * is cut off long before we'd hold all of it. Only the buffered sink looks
   * at this; a caller that streams the body bounds its own memory through
   * the return value of chttpcli_write_fn. */
  size_t max_size;
  bool too_large;
} chttp_bodybuf_t;

/* Every occurrence, in wire order, of each field name that appears more than
 * once in one response header block. data has one record per occurrence: a
 * chttp_field_rec_t followed by "name\0value\0" with the name lower-cased,
 * padded to a multiple of 4 bytes. A name that appears once gets no record
 * because the header map already holds its value, and a response with no
 * repeated name allocates none of this.
 *
 * The records of one name are chained in wire order through `next`. index
 * is an open-addressing table with a slot per repeated name that keeps the
 * first and last record of its chain. Adding an occurrence, combining all
 * names and looking one up then cost time in proportion to the records they
 * touch, and we never rescan the block. Offsets are stored plus one so that
 * zero can mean "none", and the parser's caps keep every offset far below
 * UINT32_MAX. */
typedef struct {
  uint32_t name_len;
  uint32_t value_len;
  uint32_t next;
} chttp_field_rec_t;

typedef struct {
  uint32_t head;
  uint32_t tail;
} chttp_field_slot_t;

typedef struct {
  size_t len;
  size_t cap;
  chttp_field_slot_t *index; /* index_cap slots, a power of two */
  size_t index_cap;
  size_t names; /* used slots of index */
  char data[];
} chttp_field_lines_t;

/* Drives the parse of one HTTP/1.1 response, i.e. one hop. Each hop of a
 * redirect chain gets a fresh instance, so there's no shared header map
 * being reset in place and the headers of an intermediate redirect have
 * nowhere to leak into the final response from. */
typedef struct {
  ccol_memmgmt_procs_t *mp;
  chmap headers; /* chmap(char* -> char*), owned here until we hand it on
                  * or destroy it. */
  chttp_field_lines_t *field_lines; /* Owned like headers, and NULL until
                                     * some name repeats. */

  bool is_head_request;
  bool will_redirect;
  char *location; /* Owned, and set only when will_redirect is
                   * true. */

  bool message_complete;
  bool trailing_garbage;
  bool error;     /* Some allocation inside a callback failed. */
  bool aborted;   /* sink_fn returned a short count, i.e. the caller that
                   * streams the body asked to stop the transfer. */
  bool too_large; /* The buffered body went over the cap of
                   * chttpclient_set_max_response_body_size. A declared
                   * Content-Length is refused up front by
                   * _on_headers_complete, and the body that piles up is
                   * refused by _sink_buffered as it arrives. A request that
                   * streams its body has no cap, so it never gets this
                   * flag. See the comment of chttp_bodybuf_t.max_size. */
  int status_code;

  chttpcli_write_fn requested_sink_fn;
  void *requested_sink_ctx;
  chttpcli_write_fn sink_fn; /* Picked once the headers are
                              * complete. */
  void *sink_ctx;
} chttp_parse_ctx_t;

/* ========================================================================== */
/*                         CHTTPCLI HANDLE SLOT TABLE                         */
/* ========================================================================== */

/* chttpcli is an opaque value handle, with the slot index in the top 32 bits
 * and the generation in the bottom 32 (see the doc comment on the typedef in
 * include/chttpclient.h). Every handle goes through this table before we
 * touch the struct chttpclient* behind it, and that's what lets
 * __chttpclient_destroy turn two mistakes into a _ccol_fatal_err instead of
 * a use-after-free or a double free: a concurrent double destroy racing
 * another destroy of the same live handle, and a sequential one where the
 * handle is stale because an earlier destroy has finished. A slot is marked
 * not-in-use at the moment it's released and its generation goes up on every
 * reuse, so a stale handle can never alias some later, unrelated client in
 * the same slot index.
 *
 * The lock is a read-write lock rather than a plain mutex because
 * _chttpcli_resolve only reads (it bounds-checks idx, compares the
 * generation and reads slot->ptr) and it runs once per outbound request in
 * every API tier (chttpclient_do, _do_async and _do_pooled). The writers are
 * _chttpcli_handle_slot_acquire and __chttpclient_destroy, and each runs
 * once per client lifetime, not per request. It's the same shape as the
 * slot-table rwlocks of cthreadcomm.c and cthreadpool.c with one difference:
 * our fork handlers don't take this lock. Only an application thread that
 * creates or destroys a client ever holds it, and forking with a client
 * created before the fork isn't a supported pattern, so the writer-TID
 * tracking and child re-initialization of those two files have no job
 * here. */
typedef struct {
  struct chttpclient *ptr; /* NULL when the slot is free */
  uint32_t generation;     /* A new value on every acquire, and for one
                               slot index it only goes up: 0 before the
                               first use, 1 after the first acquire. */
  bool in_use;
} chttpcli_slot_t;

static struct {
  _ccol_rw_lock_t rwlock;
  _ccol_once_flag_t once;
  cvec slots;        /* cvec of chttpcli_slot_t. It only grows through
                         push_back, so once an index exists it is
                         permanent. */
  cvec free_indices; /* cvec of uint32_t, a LIFO free list for O(1)
                         reuse. */
  /* Set when the process-exit destructor finds a live client and must
     leave this table alone. The destroy that later releases the last slot
     then does the release the destructor couldn't. Only touched under the
     write lock. */
  bool release_deferred;
  /* How many released slots have an index that a failed push kept off
     free_indices. When an acquire finds free_indices empty and this above
     zero, it scans for such a slot, so a failed push doesn't strand an
     index for the rest of the process. Only touched under the write
     lock. */
  size_t lost_indices;
} chttpcli_slot_table = {0};

#ifdef RUNNING_UNIT_TESTS
/* When true, the next push onto free_indices acts as if its allocation had
 * failed, and the flag clears itself. */
static _Atomic bool g_chttpcli_fail_next_free_index_push = false;

void _chttpcli_force_next_free_index_push_failure_for_tests(void) {
  atomic_store(&g_chttpcli_fail_next_free_index_push, true);
}
#endif

/* Gives idx back for reuse; the caller holds the write lock. If the push
 * fails we count the index in lost_indices rather than drop it: the slot is
 * released already, and an index that's simply lost would make every later
 * create grow the table again. */
static void _chttpcli_free_index_release_locked(uint32_t idx) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_chttpcli_fail_next_free_index_push, false)) {
    chttpcli_slot_table.lost_indices++;
    return;
  }
#endif
  if (cvector_push_back(chttpcli_slot_table.free_indices, &idx) != ccol_success)
    chttpcli_slot_table.lost_indices++;
}

/* Finds one slot whose index got stranded by a failed push. The caller holds
 * the write lock and has seen free_indices empty, and only that makes "ptr
 * is NULL and in_use is false" mean lost rather than just free: a slot
 * released with a successful push sits on that list, and a slot whose
 * destroy is in progress keeps a non-NULL ptr until the destroy's very last
 * step. When the scan finds nothing we zero the counter, otherwise a stale
 * count would make every later acquire scan the table. */
static __attribute__((noinline)) bool _chttpcli_reclaim_lost_index_locked(
    uint32_t *out_idx) {
  size_t slot_count = cvector_elem_count(chttpcli_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++) {
    chttpcli_slot_t *slot =
        (chttpcli_slot_t *)cvector_at(chttpcli_slot_table.slots, i);
    if (slot->ptr == NULL && !slot->in_use) {
      *out_idx = (uint32_t)i;
      chttpcli_slot_table.lost_indices--;
      return true;
    }
  }
  chttpcli_slot_table.lost_indices = 0;
  return false;
}

/* Defined with the process-exit teardown below. We need them up here because
   the last locked section of __chttpclient_destroy does the release that the
   destructor left over for it. */
static bool _chttpcli_any_slot_live_locked(void);
static void _chttpcli_release_slot_table_locked(void);
static void _chttpcli_release_slot_table_if_deferred_locked(void);

/* The hot half of the table above. It maps a handle to a pointer and holds
 * the pin that keeps a client alive during a call. It's kept apart because
 * a resolve runs on every request and mustn't write anything that another
 * thread reads, while the rest of the table's work (slot reuse, the sweep
 * at exit) is cold and can live under the read-write lock. */
static ccol_pintable chttpcli_pintable;

static void _chttpcli_slot_table_init_globals(void) {
  if (_ccol_rw_lock_init(chttpcli_slot_table.rwlock) != 0)
    _ccol_fatal_err("chttpcli slot table: failed to initialize rwlock");
  chttpcli_slot_table.slots = cvector_create(sizeof(chttpcli_slot_t), NULL);
  if (!chttpcli_slot_table.slots)
    _ccol_fatal_err("chttpcli slot table: failed to allocate slots vector");
  chttpcli_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!chttpcli_slot_table.free_indices)
    _ccol_fatal_err(
        "chttpcli slot table: failed to allocate free-index vector");
}

struct chttpclient {
  _ccol_mutex_t lock;
  _ccol_cond_var_t available;

  /* The concurrency limiter, which bounds how many requests can be in
   * flight at once. */
  size_t pool_cap;
  size_t configured_pool_size;
  size_t in_flight_count;
  bool pool_initialized;
  bool destroying;

  /* The keep-alive idle pool, a chmap(char *origin_key -> cvec of
   * chttp_conn_t), which has nothing to do with the limiter above. */
  chmap idle_pools;
  size_t idle_total_count;

  /* Tier 2's keep-alive idle pool, a chmap(char *origin_key -> cvec of
   * chttp_async_ctx_t*). It's separate from idle_pools above because it
   * stores something else: idle_pools holds a plain fd this thread owns, and
   * this one holds connections attached to the shared async reactor (the
   * "ASYNC IDLE POOL" section further down describes the design). Whenever
   * idle_total_count_async gets to 0 we broadcast idle_async_drained, so
   * __chttpclient_destroy can wait for every in-flight teardown of an idle
   * connection before freeing this struct. Some of those teardowns come from
   * its own drain and the rest from connections dying on their own. */
  chmap idle_pools_async;
  size_t idle_total_count_async;
  _ccol_cond_var_t idle_async_drained;

  /* The active Tier 2 and Tier 3 async chains of THIS client, where active
   * means the idle pool doesn't hold it yet. _async_chain_create adds one
   * per chain and _async_chain_release takes it off once the chain's
   * refcount hits zero, which is when the last hop is either torn down or
   * joins the idle pool above. It's a separate count from in_flight_count
   * (Tier 1 only) and from idle_total_count_async, since an idle pooled
   * connection is the OPPOSITE of an in-flight one.
   *
   * On purpose it has a DEDICATED leaf lock and condition variable and we
   * never use cli->lock or cli->available for it. The only place that
   * decrements it is _async_chain_release, which runs from inside a
   * ccol_event_loop dispatch callback as often as from a safe synchronous
   * context. In the callback it is holding that registration's
   * dispatch_lock, so taking cli->lock there would add a dispatch_lock ->
   * cli->lock order this module has no other use for, and we gain nothing
   * over a lock that nobody holds across another call.
   *
   * __chttpclient_destroy waits on async_count_drained for this to reach
   * zero before it frees cli. Without that wait we'd have a real
   * use-after-free, because code dereferences chain->cli and ctx->cli (for
   * cli->lock, cli->idle_pools_async and cli->m_procs) for the whole life of
   * an active hop, long after chttpclient_do_async/_streaming has returned
   * to the caller. */
  _ccol_mutex_t async_count_lock;
  _ccol_cond_var_t async_count_drained;
  size_t async_in_flight_count;

  uint64_t connect_timeout_us;
  uint64_t request_timeout_us;
  /* Cap on a buffered response body in all three tiers, with the default 0
   * meaning no limit (see the doc comment of
   * chttpclient_set_max_response_body_size). Like connect_timeout_us and
   * request_timeout_us above, it's copied under cli->lock into a snapshot
   * per request or per chain (a local of chttp_do_internal, or
   * chttp_async_chain_t.max_response_body_size), and nobody reads this field
   * again while the request is in flight. */
  size_t max_response_body_size;
  chttp_tls_config_t tls;
  char *owned_cert_path;
  char *owned_key_path;
  char *owned_ca_bundle_path;
  ctls_ctx_t *tls_ctx; /* Rebuilt on every chttpclient_set_tls
                          call. */
  bool tls_ctx_usable; /* false when we couldn't read the configured
                          cert, key or ca paths (see
                          _rebuild_tls_ctx_locked). */

  ccol_memmgmt_procs_t *m_procs;

  /* This client's own handle, so an unpin can find the slot holding its pin
   * without the caller passing one in. Written once before the handle is
   * published and never again. It sits down here rather than with the
   * fields above so that nothing a request path reads changes offset. */
  chttpcli self_handle;

  /* Goes up on every _rebuild_tls_ctx_locked call, which gives each TLS
   * configuration of this client an identity of its own. Both idle pools
   * are keyed by the origin alone ("scheme://host:port" or "unix://<path>")
   * and the origin carries nothing about a pooled connection's TLS policy.
   * So without this, after a chttpclient_set_tls call that pins a private
   * CA, turns verification back on or rotates the client certificate, we'd
   * keep serving requests for up to CHTTP_IDLE_MAX_AGE_MS over connections
   * made under the old configuration. Each pooled TLS connection carries
   * the generation of its handshake and we won't reuse it once that stops
   * matching. Read and written under cli->lock like everything else here.
   * It starts at 1, so a connection carrying 0 never did a handshake and
   * can never match. */
  uint64_t tls_generation;
};

/* One frame of the stack of clients that have a request running on the
 * calling thread: a Tier 1 call for as long as it lasts, or the write_fn of
 * a streaming Tier 2 or Tier 3 request while it runs on an engine thread.
 * Each frame lives on the C stack of whoever pushed it, and it needs to be a
 * stack because a write_fn may make a request on another client itself.
 *
 * __chttpclient_destroy walks it. Destroying a client that has a frame on
 * the calling thread would mean waiting for that very call to end (a Tier 1
 * call holds a pin, a Tier 2 or Tier 3 chain counts as in flight), i.e.
 * waiting forever, so we make that misuse fatal, the same way destroying a
 * thread pool from one of its own tasks is.
 *
 * on_engine marks a frame pushed by a Tier 2 or Tier 3 write_fn on an engine
 * thread. Destroying any client and chttpcli_engine_wait both wait for work
 * that the engine threads run, so on such a thread they're fatal as well
 * (see _chttpcli_runs_on_engine_thread). */
typedef struct _chttpcli_call_frame {
  const struct chttpclient *cli;
  struct _chttpcli_call_frame *prev;
  bool on_engine;
} _chttpcli_call_frame_t;

static __thread _chttpcli_call_frame_t *g_chttpcli_call_top = NULL;

static inline void _chttpcli_call_push(_chttpcli_call_frame_t *f,
                                       const struct chttpclient *cli,
                                       bool on_engine) {
  f->cli = cli;
  f->prev = g_chttpcli_call_top;
  f->on_engine = on_engine;
  g_chttpcli_call_top = f;
}

static inline void _chttpcli_call_pop(_chttpcli_call_frame_t *f) {
  g_chttpcli_call_top = f->prev;
}

static bool _chttpcli_call_runs_on_this_thread(const struct chttpclient *cli) {
  for (const _chttpcli_call_frame_t *f = g_chttpcli_call_top; f; f = f->prev)
    if (f->cli == cli) return true;
  return false;
}

/* True on an engine thread while it is running one of the application's
 * write_fn callbacks, which is the only application code that ever runs on
 * such a thread. */
static bool _chttpcli_runs_on_engine_thread(void) {
  for (const _chttpcli_call_frame_t *f = g_chttpcli_call_top; f; f = f->prev)
    if (f->on_engine) return true;
  return false;
}

/* ========================================================================== */
/*                    CHTTPCLI HANDLE RESOLVE / UNPIN                         */
/* ========================================================================== */

/* Resolves h and pins the result so that a concurrent destroy can't free it.
 * We return NULL when h is 0 or garbage, when its slot is free, or when the
 * slot has been reused for another client (the generation won't match). On
 * success the caller MUST call _chttpcli_resolve_unpin(result) exactly
 * once, either as soon as its tier's own protection (in_flight_count or
 * async_in_flight_count) takes over, or right away for a short call that
 * doesn't block. */
static struct chttpclient *_chttpcli_resolve(chttpcli h) {
  /* No lock and no shared write here: every public entry point of the
   * module comes through this function, so a write to memory that other
   * threads read would be paid on every single request. The pin index is a
   * zeroed static, which means a handle that shows up before any client
   * exists finds no chunk and resolves to NULL, the same answer the index
   * gives for any handle it doesn't know. */
  return (struct chttpclient *)ccol_pintable_pin(&chttpcli_pintable, h);
}

static void _chttpcli_resolve_unpin(struct chttpclient *raw) {
  /* Drops the pin and nothing else. Locking the client here would have every
   * request take that lock twice, the second time right after the first
   * unlock, and that's exactly how a convoy keeps itself going when many
   * callers run at once. There's no wakeup to send either, because
   * __chttpclient_destroy polls the pin count rather than sleeping on a
   * condition variable until we get here.
   *
   * The pin lives in the slot and not in raw, and that's what makes the
   * lock-free release safe. We dereference nothing of the object, so a
   * destroy that sees the count hit zero may free raw immediately without
   * racing anything here.
   *
   * Reading raw->self_handle before the release is fine since we hold the
   * pin at that point. Once it's released the client can be freed at any
   * instant, so nothing may touch raw after this call. */
  ccol_pintable_unpin(&chttpcli_pintable, raw->self_handle);
}

/* Gives cli a new slot or a freed one and returns its handle, or 0 when the
 * allocation fails. chttpclient_create_mp calls this once, after everything
 * else in the object is built. */
static chttpcli _chttpcli_handle_slot_acquire(struct chttpclient *cli) {
  _ccol_call_once(chttpcli_slot_table.once, _chttpcli_slot_table_init_globals);
  _ccol_rw_lock_wrlock(chttpcli_slot_table.rwlock);
  uint32_t idx;
  chttpcli_slot_t *slot;
  if (cvector_elem_count(chttpcli_slot_table.free_indices) > 0) {
    cvector_pop_back(chttpcli_slot_table.free_indices, &idx);
    slot = (chttpcli_slot_t *)cvector_at(chttpcli_slot_table.slots, idx);
  } else if (chttpcli_slot_table.lost_indices > 0 &&
             _chttpcli_reclaim_lost_index_locked(&idx)) {
    slot = (chttpcli_slot_t *)cvector_at(chttpcli_slot_table.slots, idx);
  } else {
    /* A slot whose index is past what the pin table can hold could never be
     * published, so we refuse it here instead of claiming it and rolling
     * back. The rollback would push an index that no later publish can use
     * onto the free list every acquire pops from. For the caller it's just
     * an ordinary failure, the same as a table that can't grow. */
    if (cvector_elem_count(chttpcli_slot_table.slots) >= CCOL_PIN_MAX_SLOTS) {
      _ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
      return 0;
    }
    chttpcli_slot_t fresh = {0};
    if (cvector_push_back(chttpcli_slot_table.slots, &fresh) != ccol_success) {
      _ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
      return 0; /* ordinary, non-fatal OOM */
    }
    idx = (uint32_t)cvector_elem_count(chttpcli_slot_table.slots) - 1;
    slot = (chttpcli_slot_t *)cvector_at(chttpcli_slot_table.slots, idx);
  }
  slot->generation++;
  if (slot->generation == 0)
    slot->generation++; /* Skip the one value that would collide with
                         * CHTTPCLI_INVALID after about 2^32 reuses of
                         * this exact slot index, so that case can't
                         * happen at all. */
  chttpcli h = ((chttpcli)idx << 32) | (chttpcli)slot->generation;

  /* Written before the publish, so any resolver that finds this client also
   * finds the handle its unpin will need. */
  cli->self_handle = h;

  /* The publish may allocate a first-use chunk or a stripe block. If that
   * fails, the handle could never be resolved, so the slot goes back on the
   * free list. We hold off writing slot->ptr and slot->in_use until the
   * publish has succeeded on purpose, which leaves this path with only the
   * index to undo. */
  if (!ccol_pintable_publish(&chttpcli_pintable, idx, slot->generation, cli)) {
    cli->self_handle = 0;
    _chttpcli_free_index_release_locked(idx);
    _ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
    return 0;
  }

  slot->ptr = cli;
  slot->in_use = true;
  _ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
  return h;
}

/* ========================================================================== */
/*                         DEFAULT CLIENT                                     */
/* ========================================================================== */

/* client is _Atomic so that __chttpclient_destroy can clear it with a
 * compare-and-swap when the handle it was given is this singleton (its
 * comment there explains why), and chttp_default_client reads it with a
 * plain atomic_load. It's the same _Atomic(ccol_event_reg) pattern used
 * elsewhere in this file and spares us a lock for a single value. once is
 * never reset because we never re-initialize the default client, on purpose
 * (see the doc comment of chttp_default_client). */
static struct {
  _Atomic(chttpcli) client;
  _ccol_once_flag_t once;
} default_client_bundler = {0};

/* ========================================================================== */
/*                         URL PARSING                                        */
/* ========================================================================== */

static void _url_free(ccol_memmgmt_procs_t *mp, chttp_url_t *u) {
  if (!u) return;
  _ccol_mem_free(mp, u->unix_socket_path);
  _ccol_mem_free(mp, u->host);
  _ccol_mem_free(mp, u->path_and_query);
  _ccol_mem_free(mp, u->origin_key);
  _ccol_mem_free(mp, u->userinfo_authorization);
  memset(u, 0, sizeof(*u));
}

/* Percent-encodes a unix socket path into a newly allocated, NUL-terminated
 * string, which we need whenever we rebuild a "http+unix://<path>" URL
 * string (origin_key, or a redirect target resolved against a unix-socket
 * base). Every byte outside RFC 3986's unreserved set (letters, digits,
 * '-', '.', '_' and '~') gets escaped, '/' and all sub-delims included. The
 * scheme squeezes a path that has '/' in it into a single authority
 * component, and the strictest encoding is what keeps that component
 * unambiguous. */
/* The worker behind _percent_encode_unix_path. It takes `len` explicitly
 * rather than calling strlen(path) so a white-box test can hit the overflow
 * guard below with a made-up `len`, without having to build a path string
 * of many exabytes (see
 * _chttp_percent_encode_unix_path_overflow_guard_for_tests below). */
static char *_percent_encode_unix_path_len(ccol_memmgmt_procs_t *mp,
                                           const char *path, size_t len) {
  static const char *unreserved =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
  /* Overflow guard for the size computation below, the same SIZE_MAX-relative
   * check that the other growable and percent-encoded buffers of this file
   * (chttp_base64_encode_mp and _ob_append) do for the same kind of
   * computation. Without it, a path long enough for len * 3 + 1 to wrap past
   * SIZE_MAX gets too small an `out`, and the loop below writes past its
   * end. No real input gets here, since it takes a path string of many
   * exabytes sitting in memory before the call, but we treat an unguarded
   * overflow in a size computation as a real bug however big the input
   * that would trigger it. */
  if (len > (SIZE_MAX - 1) / 3) return NULL;
  char *out = (char *)_ccol_mem_alloc(mp, len * 3 + 1);
  if (!out) return NULL;
  size_t w = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)path[i];
    if (strchr(unreserved, (char)c)) {
      out[w++] = (char)c;
    } else {
      static const char *hex = "0123456789ABCDEF";
      out[w++] = '%';
      out[w++] = hex[(c >> 4) & 0xF];
      out[w++] = hex[c & 0xF];
    }
  }
  out[w] = '\0';
  return out;
}

/* Turns a unix socket path into a newly allocated, NUL-terminated,
 * percent-encoded string for rebuilding a "http+unix://<path>" URL string,
 * such as origin_key or a redirect target resolved against a unix-socket
 * base. We escape every byte that isn't in RFC 3986's unreserved set
 * (letters, digits, '-', '.', '_' and '~'), so '/' and every sub-delim get
 * escaped too: the scheme puts a path that itself contains '/' into one
 * authority component, and only the strictest encoding keeps that
 * component unambiguous. */
static char *_percent_encode_unix_path(ccol_memmgmt_procs_t *mp,
                                       const char *path) {
  return _percent_encode_unix_path_len(mp, path, strlen(path));
}

#ifdef RUNNING_UNIT_TESTS
/*
 * White-box test helper that gives a test direct access to the overflow
 * guard of _percent_encode_unix_path_len, following the pattern of
 * _chttp_ob_append_overflow_guard_for_tests, which does the same for this
 * kind of guard. fake_len stands in for strlen(path), so no test needs to
 * build a real path string of many exabytes.
 *
 * Only call it with a fake_len above the guard's threshold of
 * (SIZE_MAX - 1) / 3. The guard then refuses the call before the loop
 * starts, so `path` only needs to be readable for the small real prefix the
 * test passes, whatever fake_len pretends.
 *
 * With a fake_len at or below the threshold the loop would index `path` for
 * up to fake_len bytes, which is a real out-of-bounds read on any ordinary
 * small test buffer, so don't. The helper is there for the rejection path
 * only; the ordinary success path is covered by every other unix-socket test
 * built on _chttp_resolve_redirect_url_for_tests. It isn't public API, and
 * like every other white-box helper in this file a gate keeps the symbol out
 * of a production build of libccollections.so.
 */
bool _chttp_percent_encode_unix_path_overflow_guard_for_tests(
    ccol_memmgmt_procs_t *mp, const char *path, size_t fake_len) {
  char *r = _percent_encode_unix_path_len(mp, path, fake_len);
  bool rejected = (r == NULL);
  _ccol_mem_free(mp, r);
  return rejected;
}
#endif /* RUNNING_UNIT_TESTS */

/* Percent-decodes [start, start+len) into a newly allocated, NUL-terminated
 * string. We only use it for userinfo components; path and query bytes are
 * never decoded and reach the server with their escapes as they are (see
 * _build_request_target). A malformed escape (a '%' without 2 hex digits
 * after it) is refused, and so is a NUL that decodes inside the component,
 * because quietly cutting a password at that NUL would give a slightly wrong
 * Authorization header instead of a clear error. */
static ccol_retval_t _percent_decode_component(ccol_memmgmt_procs_t *mp,
                                               const char *start, size_t len,
                                               char **out) {
  char *buf = (char *)_ccol_mem_alloc(mp, len + 1);
  if (!buf) return ccol_not_enough_memory;
  size_t w = 0;
  for (size_t i = 0; i < len; i++) {
    char c = start[i];
    if (c == '%') {
      if (i + 2 >= len || !isxdigit((unsigned char)start[i + 1]) ||
          !isxdigit((unsigned char)start[i + 2])) {
        _ccol_mem_free(mp, buf);
        return ccol_http_invalid_url;
      }
      char hex[3] = {start[i + 1], start[i + 2], '\0'};
      int v = (int)strtol(hex, NULL, 16);
      if (v == 0) {
        _ccol_mem_free(mp, buf);
        return ccol_http_invalid_url;
      }
      buf[w++] = (char)v;
      i += 2;
    } else {
      buf[w++] = c;
    }
  }
  buf[w] = '\0';
  *out = buf;
  return ccol_success;
}

/* True for the bytes RFC 3986 allows as they are in the path or query of a
 * request-target: unreserved, sub-delims, ':', '@', '/' and '?'. The caller
 * deals with '%' separately. */
static bool _is_target_literal_byte(unsigned char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9'))
    return true;
  /* Unreserved marks, sub-delims, then ':', '@', '/', '?'. Without the test
   * for 0, strchr would match the string's terminator. */
  return c != 0 && strchr("-._~!$&'()*+,;=:@/?", (int)c) != NULL;
}

/*
 * Builds the request-target for the request line out of a URL's path and
 * query, pq[0..pq_len), and the result is always a valid RFC 3986
 * origin-form "/path[?query]". Both the caller's URL and every resolved
 * redirect target go through the URL parser, so both end up here.
 *
 * Any byte RFC 3986 doesn't allow as is in a path or query becomes "%XX"
 * with upper-case hex digits: the space, control bytes other than CR and
 * LF, DEL, everything from 0x80 up, and the characters RFC 3986 leaves out
 * such as '"', '<', '>', '\', '^', '`', '{', '|', '}', '[' and ']'. A "%XX"
 * escape that is already there is kept as it is, so we never encode twice.
 * A '%' without two hex digits after it isn't a valid URL and gives
 * ccol_http_invalid_url, which is what Go's net/url does with the same
 * input. CR and LF are refused by the caller before we get here.
 *
 * A target that doesn't start with '/' (an empty path, or a query with no
 * path) gets one added in front.
 */
static ccol_retval_t _build_request_target(ccol_memmgmt_procs_t *mp,
                                           const char *pq, size_t pq_len,
                                           char **out) {
  *out = NULL;
  bool add_slash = !(pq_len > 0 && pq[0] == '/');
  size_t need = add_slash ? 1 : 0;
  for (size_t i = 0; i < pq_len; i++) {
    unsigned char c = (unsigned char)pq[i];
    /* need grows by 3 per byte at most and the pq_len bytes are in memory
     * already, so this one guard covers every overflow the sum can have. */
    if (need > SIZE_MAX - 4) return ccol_not_enough_memory;
    if (c == '%') {
      if (i + 2 >= pq_len || !isxdigit((unsigned char)pq[i + 1]) ||
          !isxdigit((unsigned char)pq[i + 2]))
        return ccol_http_invalid_url;
      need += 3;
      i += 2;
    } else if (_is_target_literal_byte(c)) {
      need += 1;
    } else {
      need += 3;
    }
  }
  char *t = (char *)_ccol_mem_alloc(mp, need + 1);
  if (!t) return ccol_not_enough_memory;
  static const char hex[] = "0123456789ABCDEF";
  size_t w = 0;
  if (add_slash) t[w++] = '/';
  for (size_t i = 0; i < pq_len; i++) {
    unsigned char c = (unsigned char)pq[i];
    if (c == '%') {
      t[w++] = '%';
      t[w++] = pq[i + 1];
      t[w++] = pq[i + 2];
      i += 2;
    } else if (_is_target_literal_byte(c)) {
      t[w++] = (char)c;
    } else {
      t[w++] = '%';
      t[w++] = hex[c >> 4];
      t[w++] = hex[c & 0xF];
    }
  }
  t[w] = '\0';
  *out = t;
  return ccol_success;
}

/*
 * Parses the authority of a "http+unix://" URL. p points right after the
 * prefix, at a percent-encoded filesystem path like
 * "%2Fvar%2Frun%2Fapp.sock", the convention Python's requests-unixsocket
 * uses. We only know "http+unix://" and not "https+unix://", since TLS over
 * a local socket has no real use case; that prefix matches nothing we know
 * and ends up at the caller's ccol_http_invalid_url return. There's no
 * userinfo ("user:pass@") for this scheme either, because no established
 * convention combines the two and the decoded path may legitimately have
 * '@' bytes in it.
 */
static ccol_retval_t _parse_chttp_unix_url(ccol_memmgmt_procs_t *mp,
                                           const char *p, chttp_url_t *out) {
  const char *authority_end = p;
  while (*authority_end && *authority_end != '/' && *authority_end != '?' &&
         *authority_end != '#')
    authority_end++;
  size_t enc_len = (size_t)(authority_end - p);
  if (enc_len == 0) return ccol_http_invalid_url;

  char *unix_path = NULL;
  ccol_retval_t drv = _percent_decode_component(mp, p, enc_len, &unix_path);
  if (drv != ccol_success) return drv;
  if (unix_path[0] == '\0') {
    _ccol_mem_free(mp, unix_path);
    return ccol_http_invalid_url;
  }

  const char *q = authority_end;
  const char *pq = (*q && *q != '#') ? q : "/";
  size_t pq_raw_len = strlen(pq);
  const char *frag = memchr(pq, '#', pq_raw_len);
  size_t pq_len = frag ? (size_t)(frag - pq) : pq_raw_len;

  /* Raw CR or LF is refused here exactly as in _parse_chttp_url, whose
   * comment explains why. */
  if (memchr(pq, '\r', pq_len) || memchr(pq, '\n', pq_len)) {
    _ccol_mem_free(mp, unix_path);
    return ccol_http_invalid_url;
  }

  char *path_and_query = NULL;
  ccol_retval_t trv = _build_request_target(mp, pq, pq_len, &path_and_query);
  if (trv != ccol_success) {
    _ccol_mem_free(mp, unix_path);
    return trv;
  }

  int needed = snprintf(NULL, 0, "unix://%s", unix_path);
  if (needed < 0) {
    _ccol_mem_free(mp, unix_path);
    _ccol_mem_free(mp, path_and_query);
    return ccol_unexpected_failure;
  }
  char *origin_key = (char *)_ccol_mem_alloc(mp, (size_t)needed + 1);
  if (!origin_key) {
    _ccol_mem_free(mp, unix_path);
    _ccol_mem_free(mp, path_and_query);
    return ccol_not_enough_memory;
  }
  snprintf(origin_key, (size_t)needed + 1, "unix://%s", unix_path);

  out->is_https = false;
  out->is_ipv6 = false;
  out->is_unix = true;
  out->unix_socket_path = unix_path;
  out->host = NULL;
  out->port = 0;
  out->path_and_query = path_and_query;
  out->origin_key = origin_key;
  out->userinfo_authorization = NULL;
  return ccol_success;
}

/* Returns an owned "[host]" for an IPv6 literal and a plain strdup for any
 * other host. We need it wherever a host makes a round trip through a URL
 * string (origin_key, a rebuilt redirect URL). connect() and TLS take
 * chttp_url_t.host directly, which has no brackets. */
static char *_format_bracketed_host(ccol_memmgmt_procs_t *mp, const char *host,
                                    bool is_ipv6) {
  if (!is_ipv6) return ccol_strdup(mp, host);
  size_t hlen = strlen(host);
  /* hlen + 3 needs an overflow guard like every other size computation in
   * the URL and redirect helpers here (_percent_encode_unix_path_len,
   * _merge_ref_path and _concat_len). No real input is that large, but our
   * standing policy is that an unguarded overflow in a size computation is a
   * real bug however large the input needs to be. Without it an hlen of
   * SIZE_MAX - 2 or more wraps the allocation size, `out` comes out too
   * small, and the memcpy below writes past its end. */
  if (hlen > SIZE_MAX - 3) return NULL;
  char *out = (char *)_ccol_mem_alloc(mp, hlen + 3);
  if (!out) return NULL;
  out[0] = '[';
  memcpy(out + 1, host, hlen);
  out[hlen + 1] = ']';
  out[hlen + 2] = '\0';
  return out;
}

/* RFC 3986 SS2.2 and SS2.3 unreserved and sub-delims, plain ASCII, whatever
 * the locale is. */
static bool _uri_unreserved_or_sub_delim(unsigned char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9'))
    return true;
  switch (c) {
    case '-':
    case '.':
    case '_':
    case '~':
    case '!':
    case '$':
    case '&':
    case '\'':
    case '(':
    case ')':
    case '*':
    case '+':
    case ',':
    case ';':
    case '=':
      return true;
    default:
      return false;
  }
}

static bool _uri_hexdig(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

/* Whether s[0..len) is a non-empty RFC 3986 SS3.2.2 reg-name, i.e. only
 * unreserved, sub-delims and pct-encoded. Character-wise an IPv4address is
 * a reg-name too. Anything else (whitespace, a control byte, a byte at or
 * above 0x80, a backslash, a '[', or a '%' without two hex digits after it)
 * names an authority that different recipients read differently, and it
 * would go as it is into the Host header, the idle pool's key, SNI and the
 * resolver. A non-ASCII name must come from the caller in its ASCII
 * (punycode, RFC 5891) form. */
static bool _uri_reg_name_is_valid(const char *s, size_t len) {
  if (len == 0) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '%') {
      if (len - i < 3 || !_uri_hexdig((unsigned char)s[i + 1]) ||
          !_uri_hexdig((unsigned char)s[i + 2]))
        return false;
      i += 2;
      continue;
    }
    if (!_uri_unreserved_or_sub_delim(c)) return false;
  }
  return true;
}

/* Whether s[0..len), the text inside an IP-literal's brackets, is an RFC
 * 3986 SS3.2.2 IPv6address or IPvFuture:
 *
 *   IPvFuture = "v" 1*HEXDIG "." 1*( unreserved / sub-delims / ":" )
 *
 * We let inet_pton() check an IPv6address, since its text form (RFC 4291
 * SS2.2) is exactly RFC 3986's IPv6address rule. A zone identifier
 * (RFC 6874) isn't part of that rule, so we refuse it. Only hex digits, ':'
 * and '.' ever get to inet_pton(), because macOS's inet_pton() accepts a
 * zone suffix like "%lo" and the grammar mustn't depend on the C library. */
static bool _uri_ip_literal_is_valid(const char *s, size_t len) {
  if (len == 0) return false;
  if (s[0] == 'v' || s[0] == 'V') {
    size_t j = 1;
    while (j < len && _uri_hexdig((unsigned char)s[j])) j++;
    if (j == 1 || j >= len || s[j] != '.') return false;
    j++;
    if (j >= len) return false;
    for (; j < len; j++) {
      unsigned char c = (unsigned char)s[j];
      if (!_uri_unreserved_or_sub_delim(c) && c != ':') return false;
    }
    return true;
  }
  char text[INET6_ADDRSTRLEN];
  if (len >= sizeof(text)) return false;
  for (size_t j = 0; j < len; j++) {
    unsigned char c = (unsigned char)s[j];
    if (!_uri_hexdig(c) && c != ':' && c != '.') return false;
  }
  memcpy(text, s, len);
  text[len] = '\0';
  struct in6_addr addr;
  return inet_pton(AF_INET6, text, &addr) == 1;
}

/* Whether s[0..len) is RFC 3986 SS3.2.1 userinfo (unreserved, pct-encoded,
 * sub-delims and ":"). We also let an unescaped '@' through, because the
 * authority is split at the LAST '@' and a password that isn't fully
 * encoded may contain one, which Go's net/url accepts as well. Any other
 * byte is refused, backslash included: with a backslash before the '@' our
 * parser takes the host after the '@', while a WHATWG reader treats the
 * backslash as '/' and takes the one before it. The '%' escapes get checked
 * by the decode that comes next. */
static bool _uri_userinfo_is_valid(const char *s, size_t len) {
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (!_uri_unreserved_or_sub_delim(c) && c != ':' && c != '@' && c != '%')
      return false;
  }
  return true;
}

/*
 * The URL parser. It takes "http://" and "https://" with an optional
 * "user:pass@" userinfo prefix, which it turns into a ready-to-send
 * "Basic <base64>" Authorization value ("user@" and ":pass@" are valid RFC
 * 3986 too and work the same). After that comes a plain reg-name or IPv4
 * host or a bracketed IPv6 literal like "[::1]", an optional ":port", and
 * the path and query, and any trailing "#fragment" is thrown away. A client
 * never sends a fragment to the server (RFC 3986 SS3.5), so letting one into
 * the request line would be a real correctness bug and not a scope choice
 * we could document.
 *
 * The host has to match RFC 3986 SS3.2.2's uri-host rule and the userinfo
 * its userinfo rule (plus '@'), otherwise it's ccol_http_invalid_url (see
 * _uri_reg_name_is_valid, _uri_ip_literal_is_valid and
 * _uri_userinfo_is_valid). A host that fits the grammar but names nothing
 * fails later at connect time as ccol_http_host_resolution_failed.
 *
 * _build_request_target turns the path and query into the request-target,
 * percent-encoding every byte that can't appear in them as is and refusing
 * a '%' without two hex digits after it. Redirect targets go through this
 * same parser, so a Location header gets the same rule.
 *
 * "http+unix://<percent-encoded-path>[/path][?query]" is handled as well
 * (see _parse_chttp_unix_url). "https+unix://" isn't: like any scheme we
 * don't know, it falls through to the ccol_http_invalid_url below.
 */
static ccol_retval_t _parse_chttp_url(ccol_memmgmt_procs_t *mp, const char *url,
                                      chttp_url_t *out) {
  memset(out, 0, sizeof(*out));
  if (!url) return ccol_http_invalid_url;

  if (ccol_ascii_ncasecmp(url, "http+unix://", 12) == 0)
    return _parse_chttp_unix_url(mp, url + 12, out);

  bool https;
  const char *p;
  if (ccol_ascii_ncasecmp(url, "http://", 7) == 0) {
    https = false;
    p = url + 7;
  } else if (ccol_ascii_ncasecmp(url, "https://", 8) == 0) {
    https = true;
    p = url + 8;
  } else {
    return ccol_http_invalid_url;
  }

  /* Authority = [ userinfo "@" ] host [ ":" port ], ending at the first '/',
   * '?' or '#', or at the end of the string. */
  const char *authority_end = p;
  while (*authority_end && *authority_end != '/' && *authority_end != '?' &&
         *authority_end != '#')
    authority_end++;

  /* We look for the LAST unescaped '@' of the authority, so that a password
   * that isn't fully encoded can have a raw '@' in it, which the common real
   * parsers allow as well. */
  const char *last_at = NULL;
  for (const char *s = p; s < authority_end; s++)
    if (*s == '@') last_at = s;

  char *userinfo_authorization = NULL;
  const char *host_scan_start = p;
  if (last_at) {
    /* The '@' and ':' delimiters are searched in the RAW, percent-encoded
     * string and only then is each half decoded on its own. Decoding first
     * would make us split on a decoded '@' or ':' that is really part of
     * the password. */
    if (!_uri_userinfo_is_valid(p, (size_t)(last_at - p)))
      return ccol_http_invalid_url;
    const char *colon = NULL;
    for (const char *s = p; s < last_at; s++)
      if (*s == ':') {
        colon = s;
        break;
      }
    const char *user_start = p;
    size_t user_len = colon ? (size_t)(colon - p) : (size_t)(last_at - p);
    const char *pass_start = colon ? colon + 1 : last_at;
    size_t pass_len = colon ? (size_t)(last_at - colon - 1) : 0;

    char *user_dec = NULL, *pass_dec = NULL;
    ccol_retval_t drv =
        _percent_decode_component(mp, user_start, user_len, &user_dec);
    if (drv == ccol_success)
      drv = _percent_decode_component(mp, pass_start, pass_len, &pass_dec);
    if (drv != ccol_success) {
      _ccol_mem_free(mp, user_dec);
      _ccol_mem_free(mp, pass_dec);
      return drv;
    }

    /* In RFC 7617 SS2 the colon is the one delimiter of the encoded
     * "user-id:password" string, so a user-id with a colon in it can't be
     * sent as it is: the recipient splits at the first colon and reads some
     * other identity than the URL names. That's a problem with the URL, not
     * a resource that ran out, so we report it as an invalid URL rather than
     * mixing it into the allocation-failure path below. */
    if (strchr(user_dec, ':')) {
      _ccol_mem_free(mp, user_dec);
      _ccol_mem_free(mp, pass_dec);
      return ccol_http_invalid_url;
    }

    userinfo_authorization = chttp_basic_auth_mp(mp, user_dec, pass_dec);
    _ccol_mem_free(mp, user_dec);
    _ccol_mem_free(mp, pass_dec);
    if (!userinfo_authorization) return ccol_not_enough_memory;

    host_scan_start = last_at + 1;
  }

  bool is_ipv6 = false;
  const char *host_start;
  const char *q;
  if (*host_scan_start == '[') {
    /* The literal must end inside the authority, and a ']' later in the
     * path or query doesn't close it, so "http://[a/b]/c" has no valid
     * host. */
    const char *close = host_scan_start + 1;
    while (close < authority_end && *close != ']') close++;
    if (close >= authority_end) {
      _ccol_mem_free(mp, userinfo_authorization);
      return ccol_http_invalid_url;
    }
    char after = close[1];
    if (after != '\0' && after != ':' && after != '/' && after != '?' &&
        after != '#') {
      _ccol_mem_free(mp, userinfo_authorization);
      return ccol_http_invalid_url;
    }
    host_start = host_scan_start + 1;
    q = close; /* points at ']' */
    is_ipv6 = true;
  } else {
    host_start = host_scan_start;
    q = host_scan_start;
    while (*q && *q != ':' && *q != '/' && *q != '?' && *q != '#') q++;
  }
  size_t host_len = (size_t)(q - host_start);
  if (host_len == 0) {
    _ccol_mem_free(mp, userinfo_authorization);
    return ccol_http_invalid_url;
  }
  /* The host must be an RFC 3986 SS3.2.2 uri-host because it goes
   * unchanged into the "host: " line that _serialize_request writes, into
   * the idle pool's key, into SNI and into the resolver. A byte outside the
   * grammar would inject header lines (CR, LF), split one name into two
   * tokens (SP, HTAB), or name a host that another reader of the same URL
   * resolves to something else. */
  if (is_ipv6 ? !_uri_ip_literal_is_valid(host_start, host_len)
              : !_uri_reg_name_is_valid(host_start, host_len)) {
    _ccol_mem_free(mp, userinfo_authorization);
    return ccol_http_invalid_url;
  }
  if (is_ipv6) q++; /* skip past ']' */

  uint16_t port = https ? 443 : 80;
  if (*q == ':') {
    q++;
    unsigned long pv = 0;
    size_t pd = 0;
    while (*q && isdigit((unsigned char)*q)) {
      pv = pv * 10 + (unsigned long)(*q - '0');
      if (pv > 65535) {
        _ccol_mem_free(mp, userinfo_authorization);
        return ccol_http_invalid_url;
      }
      q++;
      pd++;
    }
    /* An empty port like "http://host:/" means the scheme's default port
     * (RFC 3986 section 3.2.3), and that's how curl and Go's net/url read
     * it too. Port 0 is nothing a connect could ever reach. */
    if (pd > 0 && pv == 0) {
      _ccol_mem_free(mp, userinfo_authorization);
      return ccol_http_invalid_url;
    }
    /* The digit loop stops at the first non-digit, whatever it is. Without
     * this check, junk right after a valid port ("http://host:80abc/get")
     * would fall into the path and query handling below and "abc/get" would
     * become the path, so we'd send the request somewhere other than what
     * the URL string says and report no error at all. */
    if (*q != '\0' && *q != '/' && *q != '?' && *q != '#') {
      _ccol_mem_free(mp, userinfo_authorization);
      return ccol_http_invalid_url;
    }
    if (pd > 0) port = (uint16_t)pv;
  }

  /* '#' always starts a fragment, no conditions. There's no escape rule for
   * the '#' itself, since a fragment's start is never quoted. */
  const char *pq = (*q && *q != '#') ? q : "/";
  size_t pq_raw_len = strlen(pq);
  const char *frag = memchr(pq, '#', pq_raw_len);
  size_t pq_len = frag ? (size_t)(frag - pq) : pq_raw_len;

  /* A raw CR or LF in the path or query makes the URL malformed, and we
   * refuse it instead of encoding it, for the same reason as in the host
   * check above: a URL string with a line break in it was made to inject
   * header lines or smuggle in a second request. The percent-encoded form
   * of those bytes is fine, it's ordinary path or query content and
   * _build_request_target leaves it as it is. Every other byte that can't
   * appear as is on the request line gets percent-encoded there. */
  if (memchr(pq, '\r', pq_len) || memchr(pq, '\n', pq_len)) {
    _ccol_mem_free(mp, userinfo_authorization);
    return ccol_http_invalid_url;
  }

  char *host = (char *)_ccol_mem_alloc(mp, host_len + 1);
  if (!host) {
    _ccol_mem_free(mp, userinfo_authorization);
    return ccol_not_enough_memory;
  }
  memcpy(host, host_start, host_len);
  host[host_len] = '\0';

  /* pq may be empty or start with '?' (a query and no path), in which case
   * _build_request_target puts the leading '/' in. */
  char *path_and_query = NULL;
  ccol_retval_t trv = _build_request_target(mp, pq, pq_len, &path_and_query);
  if (trv != ccol_success) {
    _ccol_mem_free(mp, host);
    _ccol_mem_free(mp, userinfo_authorization);
    return trv;
  }

  char *bracketed_host = _format_bracketed_host(mp, host, is_ipv6);
  if (!bracketed_host) {
    _ccol_mem_free(mp, host);
    _ccol_mem_free(mp, path_and_query);
    _ccol_mem_free(mp, userinfo_authorization);
    return ccol_not_enough_memory;
  }

  int needed = snprintf(NULL, 0, "%s://%s:%u", https ? "https" : "http",
                        bracketed_host, (unsigned)port);
  if (needed < 0) {
    _ccol_mem_free(mp, host);
    _ccol_mem_free(mp, path_and_query);
    _ccol_mem_free(mp, bracketed_host);
    _ccol_mem_free(mp, userinfo_authorization);
    return ccol_unexpected_failure;
  }
  char *origin_key = (char *)_ccol_mem_alloc(mp, (size_t)needed + 1);
  if (!origin_key) {
    _ccol_mem_free(mp, host);
    _ccol_mem_free(mp, path_and_query);
    _ccol_mem_free(mp, bracketed_host);
    _ccol_mem_free(mp, userinfo_authorization);
    return ccol_not_enough_memory;
  }
  snprintf(origin_key, (size_t)needed + 1, "%s://%s:%u",
           https ? "https" : "http", bracketed_host, (unsigned)port);
  _ccol_mem_free(mp, bracketed_host);
  /* Host names compare case-insensitively (RFC 4343), and so does the hex
   * of an IPv6 literal, so we lower-case the idle pool's key and
   * "Example.com" shares its connections with "example.com". The scheme is
   * lower case anyway and the port is digits; host itself keeps the URL's
   * spelling for the Host header. A trailing dot is kept in the key, because
   * "a." is an absolute name while "a" may go through a search domain to a
   * different host, so they aren't the same origin. */
  for (char *c = origin_key; *c; c++)
    *c = (char)ccol_ascii_tolower((unsigned char)*c);

  out->is_https = https;
  out->is_ipv6 = is_ipv6;
  out->host = host;
  out->port = port;
  out->path_and_query = path_and_query;
  out->origin_key = origin_key;
  out->userinfo_authorization = userinfo_authorization;
  return ccol_success;
}

/* RFC 3986 SS5.2.4 "Remove Dot Segments", for a path only. It must never
 * see a query string, since a literal ".." or "." in query bytes must not
 * turn into path navigation, so the caller cuts off any "?query" first and
 * puts it back afterwards. */
static char *_remove_dot_segments(ccol_memmgmt_procs_t *mp, const char *path) {
  size_t len = strlen(path);
  char *in = (char *)_ccol_mem_alloc(mp, len + 1);
  if (!in) return NULL;
  memcpy(in, path, len + 1);
  char *out = (char *)_ccol_mem_alloc(mp, len + 1);
  if (!out) {
    _ccol_mem_free(mp, in);
    return NULL;
  }
  size_t out_len = 0;
  char *p = in;
  while (*p) {
    if (strncmp(p, "../", 3) == 0) {
      p += 3;
    } else if (strncmp(p, "./", 2) == 0) {
      p += 2;
    } else if (strncmp(p, "/./", 3) == 0) {
      p += 2;
    } else if (strcmp(p, "/.") == 0) {
      p[1] = '\0';
    } else if (strncmp(p, "/../", 4) == 0) {
      p += 3;
      while (out_len > 0 && out[out_len - 1] != '/') out_len--;
      if (out_len > 0) out_len--;
    } else if (strcmp(p, "/..") == 0) {
      p[1] = '\0';
      while (out_len > 0 && out[out_len - 1] != '/') out_len--;
      if (out_len > 0) out_len--;
    } else if (strcmp(p, ".") == 0 || strcmp(p, "..") == 0) {
      p += strlen(p);
    } else {
      const char *seg_start = p;
      if (*p == '/') p++;
      while (*p && *p != '/') p++;
      size_t seg_len = (size_t)(p - seg_start);
      memcpy(out + out_len, seg_start, seg_len);
      out_len += seg_len;
    }
  }
  out[out_len] = '\0';
  _ccol_mem_free(mp, in);
  return out;
}

/* RFC 3986 SS5.3 "merge", on the path component only. The caller adds the
 * reference's query string back once the dot segments are removed (the
 * comment of _remove_dot_segments says why). */
static char *_merge_ref_path(ccol_memmgmt_procs_t *mp,
                             const char *base_path_and_query,
                             const char *ref_path, size_t ref_path_len) {
  const char *base_query = strchr(base_path_and_query, '?');
  size_t base_path_len = base_query ? (size_t)(base_query - base_path_and_query)
                                    : strlen(base_path_and_query);

  size_t dir_len;
  if (ref_path_len == 0) {
    /* A query-only reference like "?x" keeps the base path exactly as it
     * is. */
    dir_len = base_path_len;
  } else {
    const char *last_slash = NULL;
    for (size_t i = 0; i < base_path_len; i++)
      if (base_path_and_query[i] == '/') last_slash = &base_path_and_query[i];
    dir_len = last_slash ? (size_t)(last_slash - base_path_and_query) + 1 : 0;
  }

  /* Overflow guard for the size below, like the ones in _ob_append and
   * _sink_buffered. dir_len and ref_path_len are sized INDEPENDENTLY (one is
   * the redirect base path, the other the path of a Location reference).
   * One strlen() result alone gets close to SIZE_MAX only if it's huge, but
   * their sum can get there without either of them being that large. If it
   * wrapped, `out` would be too small and the memcpy calls below would
   * write past its end. */
  if (ref_path_len > SIZE_MAX - dir_len ||
      dir_len + ref_path_len > SIZE_MAX - 1)
    return NULL;
  char *out = (char *)_ccol_mem_alloc(mp, dir_len + ref_path_len + 1);
  if (!out) return NULL;
  memcpy(out, base_path_and_query, dir_len);
  memcpy(out + dir_len, ref_path, ref_path_len);
  out[dir_len + ref_path_len] = '\0';
  return out;
}

#ifdef RUNNING_UNIT_TESTS
/*
 * White-box test helper for the overflow guard of _merge_ref_path, the same
 * idea as _chttp_percent_encode_unix_path_overflow_guard_for_tests. Since
 * _merge_ref_path takes ref_path_len as a parameter anyway, we just pass
 * fake_ref_path_len through and nothing else changes.
 *
 * Only call it with a fake_ref_path_len above the guard's threshold, so the
 * guard refuses the call before either memcpy runs. The doc comment of that
 * other helper describes the out-of-bounds read a made-up length would
 * cause here if it got past the guard.
 *
 * Not public API: as with every other white-box helper in this file, a gate
 * keeps the symbol out of a production build of libccollections.so.
 */
bool _chttp_merge_ref_path_overflow_guard_for_tests(
    ccol_memmgmt_procs_t *mp, const char *base_path_and_query,
    const char *ref_path, size_t fake_ref_path_len) {
  char *r =
      _merge_ref_path(mp, base_path_and_query, ref_path, fake_ref_path_len);
  bool rejected = (r == NULL);
  _ccol_mem_free(mp, r);
  return rejected;
}
#endif /* RUNNING_UNIT_TESTS */

/* Joins a[0..a_len) and b[0..b_len) into one newly allocated, NUL-terminated
 * string. _resolve_redirect_url uses it to glue a reference's query string
 * back onto the resolved path. The lengths come in as parameters instead of
 * being computed with strlen so that a white-box test can drive the overflow
 * guard below with a made-up length and no string of many exabytes (see
 * _chttp_concat_len_overflow_guard_for_tests below). */
static char *_concat_len(ccol_memmgmt_procs_t *mp, const char *a, size_t a_len,
                         const char *b, size_t b_len) {
  /* Same overflow guard as in _merge_ref_path just above. a_len and b_len
   * are independent, so the sum can come close to SIZE_MAX with neither of
   * them that large alone, and then `out` would be too small for the
   * memcpy calls below. */
  if (b_len > SIZE_MAX - a_len || a_len + b_len > SIZE_MAX - 1) return NULL;
  char *out = (char *)_ccol_mem_alloc(mp, a_len + b_len + 1);
  if (!out) return NULL;
  memcpy(out, a, a_len);
  memcpy(out + a_len, b, b_len);
  out[a_len + b_len] = '\0';
  return out;
}

#ifdef RUNNING_UNIT_TESTS
/*
 * White-box test helper for _concat_len's overflow guard. Call it ONLY with
 * a fake_a_len and fake_b_len pair above the guard's threshold, so the call
 * is refused before either memcpy runs; the doc comment of
 * _chttp_merge_ref_path_overflow_guard_for_tests explains the out-of-bounds
 * read you'd get otherwise. It isn't public API, and a gate keeps the
 * symbol out of a production build of libccollections.so like every other
 * white-box helper here.
 */
bool _chttp_concat_len_overflow_guard_for_tests(ccol_memmgmt_procs_t *mp,
                                                const char *a,
                                                size_t fake_a_len,
                                                const char *b,
                                                size_t fake_b_len) {
  char *r = _concat_len(mp, a, fake_a_len, b, fake_b_len);
  bool rejected = (r == NULL);
  _ccol_mem_free(mp, r);
  return rejected;
}
#endif /* RUNNING_UNIT_TESTS */

/*
 * True if `s` starts with an RFC 3986 SS3.1 "scheme ':'", i.e.
 * ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":". We stop at the first '/',
 * '?' or '#', since a colon after one of those is plain path, query or
 * fragment content and never ends a scheme ("/a:b" and "?a:b" have no
 * scheme).
 *
 * _resolve_redirect_url needs it to spot a Location that is an absolute-URI
 * reference with a scheme we don't special-case (http, https and http+unix
 * are caught by a plain prefix check before this). Per RFC 3986 SS5.2.2 a
 * reference with ANY scheme is absolute, T = R, whether or not this client
 * knows how to fetch it.
 */
static bool _location_has_scheme(const char *s) {
  /* RFC 3986 SS3.1 is an ASCII grammar, so we write the classes out by hand
   * rather than ask isalpha() and isalnum(), which depend on the locale. */
  int c0 = ccol_ascii_tolower((unsigned char)s[0]);
  if (!(c0 >= 'a' && c0 <= 'z')) return false;
  for (const char *p = s + 1;; p++) {
    if (*p == ':') return true;
    int c = ccol_ascii_tolower((unsigned char)*p);
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' ||
          c == '-' || c == '.'))
      return false;
  }
}

/* Copies url, a reference with an authority ("scheme://authority..."), and
 * runs remove_dot_segments (RFC 3986 SS5.2.4) over its path, which is what
 * RFC 3986 SS5.2.2 does with every reference that has a scheme or an
 * authority. Everything up to the end of the authority, and the query and
 * fragment after the path, are copied untouched, so a ".." in a query can't
 * become path navigation. NULL comes back only for a failed allocation. */
static char *_normalize_authority_ref(ccol_memmgmt_procs_t *mp,
                                      const char *url) {
  const char *auth = strstr(url, "//");
  if (!auth) return ccol_strdup(mp, url);
  const char *path = auth + 2;
  while (*path && *path != '/' && *path != '?' && *path != '#') path++;
  size_t path_len = strcspn(path, "?#");
  if (path_len == 0) return ccol_strdup(mp, url);
  char *path_only = (char *)_ccol_mem_alloc(mp, path_len + 1);
  if (!path_only) return NULL;
  memcpy(path_only, path, path_len);
  path_only[path_len] = '\0';
  char *clean = _remove_dot_segments(mp, path_only);
  _ccol_mem_free(mp, path_only);
  if (!clean) return NULL;
  size_t prefix_len = (size_t)(path - url);
  size_t clean_len = strlen(clean);
  const char *rest = path + path_len;
  size_t rest_len = strlen(rest);
  char *out =
      (char *)_ccol_mem_alloc(mp, prefix_len + clean_len + rest_len + 1);
  if (out) {
    memcpy(out, url, prefix_len);
    memcpy(out + prefix_len, clean, clean_len);
    memcpy(out + prefix_len + clean_len, rest, rest_len + 1);
  }
  _ccol_mem_free(mp, clean);
  return out;
}

/*
 * Resolves a Location header against the current hop's URL, following RFC
 * 3986 SS5.2-5.3. Absolute URLs, protocol-relative references like
 * "//host/path", absolute-path references like "/foo" and the general
 * relative-path ones ("foo", "../foo", "./foo", "?query") all work, and any
 * Location that isn't NULL or empty resolves to SOME absolute URL, the way
 * a browser or curl does it. When we return NULL, *rv_out tells why:
 * ccol_http_transfer_aborted for a NULL or empty location, and
 * ccol_not_enough_memory for a failed allocation (a size computation that
 * would overflow counts as one, since no allocator could serve it). Every
 * tier passes that code on as it is, so a failed allocation never looks
 * like a broken connection.
 *
 * The next hop always runs the result through _parse_chttp_url again, so
 * we don't need to care here about carrying userinfo or credentials
 * forward; each tier's hop loop takes care of that. The same thing makes an
 * absolute-URI reference with an unknown scheme come out right (see
 * _location_has_scheme below). We don't need to know which schemes
 * _parse_chttp_url takes, we just hand the reference back as it is, and the
 * next hop's _parse_chttp_url refuses it with the same
 * ccol_http_invalid_url that an unsupported top-level request URL gets.
 * That way such a reference is never resolved wrongly without anybody
 * hearing about it.
 *
 * Dot segment removal (RFC 3986 SS5.2.4) touches the path component only.
 * Each branch below that can produce a query string splits it off first and
 * appends it back unchanged afterwards, so a query value containing "/",
 * ".." or "." bytes never turns into path navigation.
 */
static char *_resolve_redirect_url_impl(ccol_memmgmt_procs_t *mp,
                                        const chttp_url_t *base,
                                        const char *location,
                                        ccol_retval_t *rv_out) {
  if (!location || !*location) {
    *rv_out = ccol_http_transfer_aborted;
    return NULL;
  }
  /* From here on, a NULL return means an allocation failed or a size was too
   * big for any allocation. */
  *rv_out = ccol_not_enough_memory;
  if (ccol_ascii_ncasecmp(location, "http://", 7) == 0 ||
      ccol_ascii_ncasecmp(location, "https://", 8) == 0 ||
      ccol_ascii_ncasecmp(location, "http+unix://", 12) == 0) {
    return _normalize_authority_ref(mp, location);
  }
  /* An absolute-URI reference with a scheme other than the three above, like
   * "g:h", "mailto:x@y" or "ftp://host/path". RFC 3986 SS5.2.2 says T = R
   * the moment R has a scheme, unconditionally, so it must not fall into the
   * relative-reference code below. That code would merge the whole
   * "scheme:opaque" string onto the path of the CURRENT origin as if it were
   * a same-origin relative path: "g:h" against "http://a/b/c/d;p?q" would
   * quietly become "http://a/b/c/g:h" when the right answer is the absolute
   * reference "g:h". We return it untouched since the next hop refuses it
   * anyway (the doc comment of this function explains why it's the next
   * hop's _parse_chttp_url that does the refusing). */
  if (_location_has_scheme(location)) return ccol_strdup(mp, location);

  const char *scheme = base->is_https ? "https" : "http";

  if (location[0] == '/' && location[1] == '/') {
    /* Protocol-relative reference. It names a network host to redirect to
     * (maybe a different one) without any ambiguity, unix-socket base or
     * not. It inherits the base's scheme, which is always "http" for a unix
     * base because is_https is never true there. What follows is a
     * well-formed authority and path for the next _parse_chttp_url call once
     * its dot segments are removed, same as for the fully absolute URL
     * above. */
    int needed = snprintf(NULL, 0, "%s:%s", scheme, location);
    if (needed < 0) return NULL;
    char *out = (char *)_ccol_mem_alloc(mp, (size_t)needed + 1);
    if (!out) return NULL;
    snprintf(out, (size_t)needed + 1, "%s:%s", scheme, location);
    char *clean = _normalize_authority_ref(mp, out);
    _ccol_mem_free(mp, out);
    return clean;
  }

  /* A client never sends a fragment to a server (RFC 3986 SS3.5), and the
   * SS5.3 resolution algorithm keeps the fragment out of R.path and R.query
   * as well. The absolute-URL and protocol-relative branches above can leave
   * it to the next hop's _parse_chttp_url, but this branch can't, so we drop
   * the fragment here before any merge or dot segment removal. Otherwise
   * _remove_dot_segments below would walk a fragment holding "/../" (say
   * "g#/../h") as real path navigation and quietly land on the wrong target,
   * and a fragment-only reference like "#s" would reach the merge branch
   * with a non-empty ref_path when the right answer is to keep the base path
   * exactly as it is. '#' itself has no escape rules (_parse_chttp_url
   * treats the fragment delimiter the same way), so the first raw '#' always
   * starts the fragment. */
  const char *frag = strchr(location, '#');
  size_t loc_len_nf = frag ? (size_t)(frag - location) : strlen(location);
  char *location_nf = (char *)_ccol_mem_alloc(mp, loc_len_nf + 1);
  if (!location_nf) return NULL;
  memcpy(location_nf, location, loc_len_nf);
  location_nf[loc_len_nf] = '\0';
  location = location_nf;

  /* A unix target has no host or port to rebuild from, so with a unix-socket
   * base an absolute-path or relative-path reference becomes
   * "http+unix://<percent-encoded-path>" plus the resolved path, in place of
   * "scheme://host:port" plus the path. */
  char *authority = base->is_unix
                        ? _percent_encode_unix_path(mp, base->unix_socket_path)
                        : _format_bracketed_host(mp, base->host, base->is_ipv6);
  if (!authority) {
    _ccol_mem_free(mp, location_nf);
    return NULL;
  }

  bool default_port =
      !base->is_unix && ((base->is_https && base->port == 443) ||
                         (!base->is_https && base->port == 80));

  if (loc_len_nf == 0) {
    /* A fragment-only reference like "#s". RFC 3986 SS5.3 leaves both
     * R.path and R.query undefined then, giving T.path = Base.path and
     * T.query = Base.query, so we keep the whole base->path_and_query as it
     * is and not just its directory part. The general ref_path_len == 0
     * code further down would keep only the base's PATH and lose its query,
     * which is right for a real query-only reference like "?y" (there
     * R.query IS defined and replaces the base's query) but wrong here. The
     * base is resolved and normalised already, so no merge and no dot
     * segment removal are needed. */
    _ccol_mem_free(mp, location_nf);
    const char *out_scheme0 = base->is_unix ? "http+unix" : scheme;
    int needed0;
    if (base->is_unix || default_port) {
      needed0 = snprintf(NULL, 0, "%s://%s%s", out_scheme0, authority,
                         base->path_and_query);
    } else {
      needed0 = snprintf(NULL, 0, "%s://%s:%u%s", out_scheme0, authority,
                         (unsigned)base->port, base->path_and_query);
    }
    if (needed0 < 0) {
      _ccol_mem_free(mp, authority);
      return NULL;
    }
    char *out0 = (char *)_ccol_mem_alloc(mp, (size_t)needed0 + 1);
    if (!out0) {
      _ccol_mem_free(mp, authority);
      return NULL;
    }
    if (base->is_unix || default_port) {
      snprintf(out0, (size_t)needed0 + 1, "%s://%s%s", out_scheme0, authority,
               base->path_and_query);
    } else {
      snprintf(out0, (size_t)needed0 + 1, "%s://%s:%u%s", out_scheme0,
               authority, (unsigned)base->port, base->path_and_query);
    }
    _ccol_mem_free(mp, authority);
    return out0;
  }

  /* We split R.query off R.path ONCE, here at the start, and both branches
   * below share the result. remove_dot_segments (RFC 3986 SS5.2.4) must only
   * ever see the path component and never query bytes, since a literal ".."
   * or "." inside a query value must never turn into path navigation. A
   * query holding "/../", for example, would make the dot segment removal
   * walk backwards and delete path segments it has no business touching.
   * Once the removal has run, both branches append ref_query back unchanged
   * (and there may be no ref_query at all). */
  const char *ref_query = strchr(location, '?');
  size_t ref_path_len =
      ref_query ? (size_t)(ref_query - location) : strlen(location);

  char *new_path = NULL;
  if (location[0] == '/') {
    /* An absolute-path reference, so T.path is just
     * remove_dot_segments(R.path) and there is no merge against the base
     * path. */
    char *path_only = (char *)_ccol_mem_alloc(mp, ref_path_len + 1);
    if (path_only) {
      memcpy(path_only, location, ref_path_len);
      path_only[ref_path_len] = '\0';
      new_path = _remove_dot_segments(mp, path_only);
      _ccol_mem_free(mp, path_only);
    }
  } else if (ref_path_len == 0) {
    /* RFC 3986 SS5.3 says R.path == "" means T.path = Base.path EXACTLY AS
     * IT IS, with no merge and no dot segment removal, and a query-only
     * reference like "?y" has this shape. We don't assume the base is
     * already normalised, because this client doesn't remove the dot
     * segments of the caller's original request URL either (see
     * _parse_chttp_url). "Cleaning" that URL for this one reference shape
     * would silently redirect to a different path than the original request
     * used, although no server asked for it. It would also go against the
     * RFC and against reference implementations like Python's
     * urllib.parse.urljoin, which both leave a dotted base path such as
     * "/a/../b" alone for a query-only reference; the fragment-only branch
     * above behaves the same way for the same reason. The ref_path_len == 0
     * code of _merge_ref_path makes exactly this copy (dir_len covers the
     * whole base path and nothing gets appended), but the branch below
     * would then run _remove_dot_segments on it and normalise it again.
     * That's why this case gets a branch of its own rather than skipping
     * that one call inline. */
    new_path = _merge_ref_path(mp, base->path_and_query, location, 0);
  } else {
    char *merged =
        _merge_ref_path(mp, base->path_and_query, location, ref_path_len);
    if (merged) {
      new_path = _remove_dot_segments(mp, merged);
      _ccol_mem_free(mp, merged);
    }
  }
  if (new_path && ref_query) {
    /* _concat_len has an overflow guard, so a path_len + query_len that
     * wraps SIZE_MAX is safe here (see its doc comment). Its rejection
     * lands in the !with_query branch as an ordinary allocation failure,
     * the same way a real out-of-memory does. */
    char *with_query = _concat_len(mp, new_path, strlen(new_path), ref_query,
                                   strlen(ref_query));
    if (!with_query) {
      _ccol_mem_free(mp, new_path);
      new_path = NULL;
    } else {
      _ccol_mem_free(mp, new_path);
      new_path = with_query;
    }
  }
  if (!new_path) {
    _ccol_mem_free(mp, authority);
    _ccol_mem_free(mp, location_nf);
    return NULL;
  }

  const char *out_scheme = base->is_unix ? "http+unix" : scheme;
  int needed;
  if (base->is_unix) {
    needed = snprintf(NULL, 0, "%s://%s%s", out_scheme, authority, new_path);
  } else if (default_port) {
    needed = snprintf(NULL, 0, "%s://%s%s", out_scheme, authority, new_path);
  } else {
    needed = snprintf(NULL, 0, "%s://%s:%u%s", out_scheme, authority,
                      (unsigned)base->port, new_path);
  }
  if (needed < 0) {
    _ccol_mem_free(mp, authority);
    _ccol_mem_free(mp, new_path);
    _ccol_mem_free(mp, location_nf);
    return NULL;
  }
  char *out = (char *)_ccol_mem_alloc(mp, (size_t)needed + 1);
  if (!out) {
    _ccol_mem_free(mp, authority);
    _ccol_mem_free(mp, new_path);
    _ccol_mem_free(mp, location_nf);
    return NULL;
  }
  if (base->is_unix || default_port) {
    snprintf(out, (size_t)needed + 1, "%s://%s%s", out_scheme, authority,
             new_path);
  } else {
    snprintf(out, (size_t)needed + 1, "%s://%s:%u%s", out_scheme, authority,
             (unsigned)base->port, new_path);
  }
  _ccol_mem_free(mp, authority);
  _ccol_mem_free(mp, new_path);
  _ccol_mem_free(mp, location_nf);
  return out;
}

#ifdef RUNNING_UNIT_TESTS
/* Set on a thread for as long as it runs _resolve_redirect_url, which lets
 * a test allocator fail exactly the allocations of the redirect resolution
 * whatever the tier is and whichever thread does the resolving. */
static __thread bool g_chttp_in_redirect_resolve;
bool _chttp_in_redirect_resolve_for_tests(void) {
  return g_chttp_in_redirect_resolve;
}
#endif

/* See _resolve_redirect_url_impl. Only read *rv_out when the result is
 * NULL. */
static char *_resolve_redirect_url(ccol_memmgmt_procs_t *mp,
                                   const chttp_url_t *base,
                                   const char *location,
                                   ccol_retval_t *rv_out) {
#ifdef RUNNING_UNIT_TESTS
  g_chttp_in_redirect_resolve = true;
  char *r = _resolve_redirect_url_impl(mp, base, location, rv_out);
  g_chttp_in_redirect_resolve = false;
  return r;
#else
  return _resolve_redirect_url_impl(mp, base, location, rv_out);
#endif
}

/*
 * Says whether we may fetch a redirect target (already resolved against
 * `base`) over the transport that the target names. Two rules hold no
 * matter what: a redirect never adds the AF_UNIX transport, and it never
 * points that transport at another socket. A redirect that takes a chain
 * off TLS onto a plaintext connection is refused only when the caller
 * asked for it through the prevent_tls_downgrade_on_redirect field of
 * chttp_request_t. _parse_chttp_url turns a "http+unix://" URL into a
 * connect() against a filesystem path, so we honour such a target only if
 * the hop it came from was itself bound to that exact same socket path.
 * Every other target (http, https, and any scheme _parse_chttp_url doesn't
 * know) is handed to the parse of the next hop exactly as it arrives here.
 *
 * Without this check, any ordinary http or https server could answer with
 * something like
 * "Location: http+unix://%2Fvar%2Frun%2Fdocker.sock/containers/json" and
 * point the next hop at whatever local socket it likes. That server also
 * picks the request target, so it would get a server-side request forgery
 * primitive against every AF_UNIX service the calling process can reach,
 * and any application fetching a URL whose responses it doesn't fully
 * control would expose it. That risk is too large to put behind a switch,
 * so unlike the TLS rule this one can't be turned off.
 *
 * The screen is a prefix test since "http+unix://" is the only spelling
 * that _parse_chttp_url takes as a unix target, and it ignores case for
 * the same reason that function does. A target that passes the screen
 * gets parsed, so we compare the DECODED socket path and not one of its
 * many equivalent percent-encodings. A target that fails to parse is let
 * through as it is, and the next hop's _parse_chttp_url rejects it before
 * any connect runs.
 *
 * Running the test on every hop, against the hop the Location came from,
 * covers a chain of any length as well: once a chain leaves the unix
 * transport it can never get back on it, and a chain on that transport can
 * only stay on the same socket. With prevent_tls_downgrade_on_redirect
 * set, a chain that starts on https stays on https for every hop.
 */
static ccol_retval_t _redirect_transport_allowed(ccol_memmgmt_procs_t *mp,
                                                 const chttp_url_t *base,
                                                 const char *next_url,
                                                 bool prevent_downgrade) {
  if (ccol_ascii_ncasecmp(next_url, "http+unix://", 12) != 0) {
    /* Refusing an https to http downgrade is OPT-IN.
     *
     * By default we follow such a redirect, like curl (its
     * CURLOPT_REDIR_PROTOCOLS permits both schemes) and the Go net/http
     * client do. Someone porting from either keeps the behaviour they rely
     * on and doesn't hit a refusal that no other client would give.
     *
     * That default does cost something, and that's why the opt-in exists.
     * The peer that sent the Location picks the downgrade, and once a chain
     * leaves TLS, that hop and every hop after it go in clear. A caller who
     * sets the prevent_tls_downgrade_on_redirect field of chttp_request_t
     * gets a loud ccol_http_invalid_url instead, and no request goes out
     * over the plaintext connection. A caller whose requests must never
     * travel in clear should set it.
     *
     * An UPGRADE from http to https is never refused since it can only add
     * protection.
     *
     * A target that doesn't parse goes through unchanged, just like the unix
     * screen below lets one through, and the next hop's _parse_chttp_url
     * rejects it with ccol_http_invalid_url before any connect runs. A
     * target whose scheme this client doesn't know ends up in the same
     * place. A parse that fails for lack of memory decides nothing, so we
     * report ccol_not_enough_memory and don't send the hop. */
    if (!prevent_downgrade || !base->is_https) return ccol_success;
    chttp_url_t next;
    ccol_retval_t prv = _parse_chttp_url(mp, next_url, &next);
    if (prv == ccol_not_enough_memory) return prv;
    if (prv != ccol_success) return ccol_success;
    bool allowed = next.is_https;
    _url_free(mp, &next);
    return allowed ? ccol_success : ccol_http_invalid_url;
  }
  if (!base->is_unix || !base->unix_socket_path) return ccol_http_invalid_url;
  chttp_url_t next;
  ccol_retval_t prv = _parse_chttp_url(mp, next_url, &next);
  if (prv == ccol_not_enough_memory) return prv;
  if (prv != ccol_success) return ccol_success;
  bool allowed = next.is_unix && next.unix_socket_path &&
                 strcmp(next.unix_socket_path, base->unix_socket_path) == 0;
  _url_free(mp, &next);
  return allowed ? ccol_success : ccol_http_invalid_url;
}

#ifdef RUNNING_UNIT_TESTS
/* Runs _redirect_transport_allowed on an already resolved target with the
 * caller's allocator, so a test can fail each allocation of the check one
 * by one. The base URL itself is parsed with plain malloc. */
ccol_retval_t _chttp_redirect_transport_check_for_tests(
    ccol_memmgmt_procs_t *mp, const char *base_url, const char *next_url,
    bool prevent_downgrade) {
  chttp_url_t base;
  ccol_retval_t rv = _parse_chttp_url(NULL, base_url, &base);
  if (rv != ccol_success) return rv;
  rv = _redirect_transport_allowed(mp, &base, next_url, prevent_downgrade);
  _url_free(NULL, &base);
  return rv;
}
#endif

/* White-box test helpers around _parse_chttp_url and _resolve_redirect_url.
 * chttp_url_t is local to this file, so the helpers flatten the result into
 * out parameters or into a plain string. With an mp of NULL every result
 * comes from plain malloc (see _ccol_mem_alloc) and the test code frees it
 * with plain free(). They aren't public API, and like every other white-box
 * helper in this file a gate keeps them out of a production build of
 * libccollections.so. */
#ifdef RUNNING_UNIT_TESTS
ccol_retval_t _chttp_parse_url_for_tests(
    const char *url, bool *is_https_out, bool *is_ipv6_out, char **host_out,
    uint16_t *port_out, char **path_and_query_out, char **origin_key_out,
    char **userinfo_authorization_out, bool *is_unix_out,
    char **unix_socket_path_out) {
  ccol_memmgmt_procs_t *mp = NULL;
  chttp_url_t parsed;
  ccol_retval_t rv = _parse_chttp_url(mp, url, &parsed);
  if (rv != ccol_success) return rv;
  if (is_https_out) *is_https_out = parsed.is_https;
  if (is_ipv6_out) *is_ipv6_out = parsed.is_ipv6;
  if (port_out) *port_out = parsed.port;
  if (is_unix_out) *is_unix_out = parsed.is_unix;
  if (unix_socket_path_out)
    *unix_socket_path_out = parsed.unix_socket_path;
  else
    _ccol_mem_free(mp, parsed.unix_socket_path);
  if (host_out)
    *host_out = parsed.host;
  else
    _ccol_mem_free(mp, parsed.host);
  if (path_and_query_out)
    *path_and_query_out = parsed.path_and_query;
  else
    _ccol_mem_free(mp, parsed.path_and_query);
  if (origin_key_out)
    *origin_key_out = parsed.origin_key;
  else
    _ccol_mem_free(mp, parsed.origin_key);
  if (userinfo_authorization_out)
    *userinfo_authorization_out = parsed.userinfo_authorization;
  else
    _ccol_mem_free(mp, parsed.userinfo_authorization);
  return ccol_success;
}

char *_chttp_resolve_redirect_url_for_tests(const char *base_url,
                                            const char *location) {
  ccol_memmgmt_procs_t *mp = NULL;
  chttp_url_t base;
  ccol_retval_t rv = _parse_chttp_url(mp, base_url, &base);
  if (rv != ccol_success) return NULL;
  ccol_retval_t rrv;
  char *result = _resolve_redirect_url(mp, &base, location, &rrv);
  _url_free(mp, &base);
  return result;
}

/* Resolves `location` against `base_url` the same way a hop does and hands
 * the absolute URL back through *resolved_out (NULL when the reference
 * doesn't resolve at all); the caller frees it with plain free().
 * prevent_downgrade is the per-request opt-in that chttp_request_t calls
 * prevent_tls_downgrade_on_redirect. We return whether
 * _redirect_transport_allowed accepts that target's transport. */
bool _chttp_redirect_transport_allowed_for_tests(const char *base_url,
                                                 const char *location,
                                                 bool prevent_downgrade,
                                                 char **resolved_out) {
  ccol_memmgmt_procs_t *mp = NULL;
  chttp_url_t base;
  if (resolved_out) *resolved_out = NULL;
  ccol_retval_t rv = _parse_chttp_url(mp, base_url, &base);
  if (rv != ccol_success) return false;
  ccol_retval_t rrv;
  char *resolved = _resolve_redirect_url(mp, &base, location, &rrv);
  bool allowed =
      resolved && _redirect_transport_allowed(
                      mp, &base, resolved, prevent_downgrade) == ccol_success;
  _url_free(mp, &base);
  if (resolved_out) {
    *resolved_out = resolved;
  } else {
    _ccol_mem_free(mp, resolved);
  }
  return allowed;
}
#endif /* RUNNING_UNIT_TESTS */

/* ========================================================================== */
/*                         REQUEST LIFECYCLE                                  */
/* ========================================================================== */

chttp_request_t *chttp_request_create_mp(chttp_method_t method, const char *url,
                                         const chttp_request_body_t *body,
                                         ccol_memmgmt_procs_t *mprocs,
                                         char **err_str) {
  if (!url) {
    if (err_str) *err_str = _CCOL_ERR_STR("url must not be NULL");
    return NULL;
  }
  /* CHTTP_ANY is the server's route sentinel and a value outside the enum
   * isn't a method at all. Both would end up as a request line no server
   * understands ("ANY" or "UNKNOWN"). */
  if ((unsigned)method >= (unsigned)CHTTP_ANY) {
    if (err_str)
      *err_str = _CCOL_ERR_STR("method must name a real HTTP method");
    return NULL;
  }
  /* A NULL body->data with a non-zero body->len makes no sense as a body
   * descriptor, since there's nothing to copy body->len bytes from. The
   * body-copy block below only runs for a non-NULL body->data, so without
   * this check we would quietly treat that pair as "no body".
   * chttp_base64_encode_mp refuses the same NULL pointer plus non-zero
   * length pair. A caller with a real bug, say a wrong length next to a null
   * buffer, is better served by a ccol_invalid_args it can diagnose than by
   * a request that silently goes out without its body. */
  if (body && !body->data && body->len > 0) {
    if (err_str)
      *err_str =
          _CCOL_ERR_STR("body->data must not be NULL when body->len > 0");
    return NULL;
  }
  if (mprocs && !_ccol_verify_memmgmt_procs(mprocs, err_str)) return NULL;

  ccol_memmgmt_procs_t *mp = NULL;
  if (mprocs) {
    mp = (ccol_memmgmt_procs_t *)mprocs->malloc(sizeof(ccol_memmgmt_procs_t));
    if (!mp) {
      if (err_str) *err_str = _CCOL_ERR_STR("failed to allocate mprocs");
      return NULL;
    }
    memcpy(mp, mprocs, sizeof(ccol_memmgmt_procs_t));
  }

  chttp_request_t *req =
      (chttp_request_t *)_ccol_mem_alloc(mp, sizeof(chttp_request_t));
  if (!req) {
    if (err_str) *err_str = _CCOL_ERR_STR("failed to allocate request");
    if (mp) mp->free(mp);
    return NULL;
  }
  memset(req, 0, sizeof(*req));
  req->method = method;
  req->_m_procs = mp;

  req->url = ccol_strdup(mp, url);
  if (!req->url) {
    if (err_str) *err_str = _CCOL_ERR_STR("failed to copy url");
    _ccol_mem_free(mp, req);
    if (mp) mp->free(mp);
    return NULL;
  }

  if (body && body->data && body->len > 0) {
    void *body_copy = _ccol_mem_alloc(mp, body->len);
    if (!body_copy) {
      if (err_str) *err_str = _CCOL_ERR_STR("failed to copy body");
      _ccol_mem_free(mp, req->url);
      _ccol_mem_free(mp, req);
      if (mp) mp->free(mp);
      return NULL;
    }
    memcpy(body_copy, body->data, body->len);
    req->body.data = body_copy;
    req->body.len = body->len;
  }

  /* content_type is copied whenever the caller passes one, with or without
   * body->len > 0, because a zero-length body with an explicit content type
   * is a perfectly fine request (an empty JSON payload from
   * CHTTP_JSON_BODY("", 0), for example). _serialize_request adds the
   * Content-Type header on its own and its doc says it honours
   * req->body.content_type whatever the body length is. So this copy must
   * stay out of the body->len > 0 branch above, where it would silently
   * drop content_type for exactly that shape. */
  if (body && body->content_type) {
    req->body.content_type = ccol_strdup(mp, body->content_type);
    if (!req->body.content_type) {
      if (err_str) *err_str = _CCOL_ERR_STR("failed to copy content_type");
      _ccol_mem_free(mp, (void *)req->body.data);
      _ccol_mem_free(mp, req->url);
      _ccol_mem_free(mp, req);
      if (mp) mp->free(mp);
      return NULL;
    }
  }

  return req;
}

/* Writes the lower-case form of name, NUL included, into stack_buf if it
 * fits and into a buffer from mp if it doesn't. We return that buffer (NULL
 * when the allocation fails) and set *len_out to the name's length. When
 * the returned buffer isn't stack_buf, the caller frees it through mp.
 *
 * Real HTTP field names (the "token" grammar of RFC 9110 section 5.1, all
 * the names in the IANA registry, any custom name anybody would actually
 * use) are far shorter than the stack buffer. A header lookup or update
 * therefore allocates nothing for the name, and only a name of a
 * pathological length goes to the heap. */
enum { CHTTP_NAME_STACK_BUF = 128 };
static char *_lower_name_copy(ccol_memmgmt_procs_t *mp, const char *name,
                              char *stack_buf, size_t *len_out) {
  size_t nlen = strlen(name);
  char *lower = stack_buf;
  if (nlen >= CHTTP_NAME_STACK_BUF) {
    lower = (char *)_ccol_mem_alloc(mp, nlen + 1);
    if (!lower) return NULL;
  }
  for (size_t i = 0; i <= nlen; i++)
    lower[i] = (char)ccol_ascii_tolower((unsigned char)name[i]);
  *len_out = nlen;
  return lower;
}

/* True when every byte of a header field value is a field-vchar, a space or
 * a horizontal tab (RFC 9110 section 5.5). Any other control byte and DEL
 * get refused. CR and LF would split the request, and with a NUL or another
 * control byte in a field the recipient may reject the message or read it
 * differently from how this client meant it. */
static bool _header_value_is_valid(const char *v) {
  for (const unsigned char *p = (const unsigned char *)v; *p; p++)
    if ((*p < 0x20 && *p != '\t') || *p == 0x7F) return false;
  return true;
}

ccol_retval_t chttp_request_set_header(chttp_request_t *req, const char *name,
                                       const char *value) {
  if (!req || !name || !value) return ccol_invalid_args;
  /* A field-name must be a non-empty token made of tchar bytes only
   * (RFC 7230 SS3.2.6). chttpsvr_resp_set_header in chttpserver.c does the
   * same check on the server side, and chttp1_parser.c does it for the
   * method and the header names that come in off the wire. An empty name
   * gives a malformed "name: value\r\n" wire line, and so does a name with a
   * byte outside that set like a space or a literal ':', even if the name
   * carries no CR or LF of its own. */
  if (!*name) return ccol_invalid_args;
  for (const char *p = name; *p; p++) {
    if (!chttp1_is_tchar((unsigned char)*p)) return ccol_invalid_args;
  }
  /* _serialize_request puts name and value on the wire exactly as they are,
   * as "name: value\r\n", with no escaping. Some callers put data they don't
   * trust into a request header (a forwarded bearer token, a proxied
   * header), and a CR or LF inside it lets whoever controls that data
   * inject as many extra header lines as they like, or split the request in
   * two. That's classic HTTP request splitting (CRLF injection). The bytes
   * go out exactly as given, so a loud refusal is the only safe answer: we
   * refuse such a byte right here along with every other control byte a
   * field value must not hold (see _header_value_is_valid); the name check
   * above already refuses all of them in a name. Every documented way of
   * setting a header goes through this one function. _serialize_request
   * repeats the check as a backstop for the header map anyway, because a
   * caller holding the internal chmap handle can build and assign that map
   * directly, as chttp_run_query and the tests do. */
  if (!_header_value_is_valid(value)) return ccol_invalid_args;
  /* chttpclient.c has no transfer-coding for a request body, chunked or
   * otherwise. A request with a body is always framed with Content-Length
   * followed by the exact bytes of req->body.data, so we could never honour
   * a "Transfer-Encoding" header set by the caller. _serialize_request adds
   * its own Content-Length header whenever there's no explicit
   * content-length header, and it doesn't look at transfer-encoding at all.
   * If we accepted the header silently, a Content-Length would go out next
   * to the caller's header and the request would declare BOTH framings over
   * a body with no chunk encoding, which is the ambiguous framing
   * RFC 7230 SS3.3.3 talks about. Our own chttp1_parser.c refuses exactly
   * that shape on an incoming message (see its F_CONTENT_LENGTH and
   * F_CHUNKED conflict check). Rather than dropping the header without a
   * word or sending it as it is, we refuse it here, following the same rule
   * as the CRLF check above: fail loudly and never build a malformed wire
   * message. */
  if (ccol_ascii_casecmp(name, "transfer-encoding") == 0)
    return ccol_invalid_args;

  if (!req->headers) {
    char *err = NULL;
    chmap hm =
        chmap_create_full(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                          ccol_string, req->_m_procs, NULL, NULL, &err);
    if (!hm) return ccol_not_enough_memory;
    req->headers = hm;
  }

  char name_buf[CHTTP_NAME_STACK_BUF];
  size_t nlen = 0;
  char *lower = _lower_name_copy(req->_m_procs, name, name_buf, &nlen);
  if (!lower) return ccol_not_enough_memory;

  cmap_pair kp = {.ptr = lower, .size = nlen + 1};
  cmap_pair vp = {.ptr = (void *)value, .size = strlen(value) + 1};

  ccol_retval_t rv = chmap_insert_elem((chmap)req->headers, &kp, &vp);
  if (lower != name_buf) _ccol_mem_free(req->_m_procs, lower);
  if (rv == ccol_key_already_present) rv = ccol_success;
  return rv;
}

const char *chttp_request_get_header(const chttp_request_t *req,
                                     const char *name) {
  if (!req || !name || !req->headers) return NULL;

  char name_buf[CHTTP_NAME_STACK_BUF];
  size_t nlen = 0;
  char *lower = _lower_name_copy(req->_m_procs, name, name_buf, &nlen);
  if (!lower) return NULL;

  cmap_pair kp = {.ptr = lower, .size = nlen + 1};
  cmap_pair vp;
  ccol_retval_t rv = chmap_get_elem_ref((chmap)req->headers, &kp, &vp);
  if (lower != name_buf) _ccol_mem_free(req->_m_procs, lower);

  if (rv != ccol_success) return NULL;
  return (const char *)vp.ptr;
}

void chttp_request_free(chttp_request_t *req) {
  if (!req) return;
  ccol_memmgmt_procs_t *mp = req->_m_procs;
  _ccol_mem_free(mp, req->url);
  _ccol_mem_free(mp, (void *)req->body.data);
  _ccol_mem_free(mp, (void *)req->body.content_type);
  if (req->headers) __chmap_destroy((chmap)req->headers);
  _ccol_mem_free(mp, req);
  if (mp) mp->free(mp);
}

/* ========================================================================== */
/*                         DEADLINE HELPERS                                   */
/* ========================================================================== */

/* Makes a deadline timeout_us microseconds from now on CLOCK_MONOTONIC. 0
 * gives an inactive deadline (no limit), and a value too large for the
 * clock saturates at the latest time we can represent instead of wrapping
 * into the past. */
static chttp_deadline_t _deadline_make(uint64_t timeout_us) {
  chttp_deadline_t d = {0};
  if (timeout_us == 0) return d; /* inactive, which means no limit */
  d.active = true;
  ccol_deadline_after_us(timeout_us, &d.deadline);
  return d;
}

/* Milliseconds from `from` to `to`, rounded down, computed from the full
 * nanosecond difference. Doing it per component (seconds times 1000 plus
 * the nanosecond difference over 1000000) truncates a negative nanosecond
 * part toward zero, so a short span across a second boundary gets rounded
 * up and 0.5 ms comes out as 1 ms. */
static inline int64_t _ms_elapsed_floor(const struct timespec *from,
                                        const struct timespec *to) {
  int64_t ns = ((int64_t)to->tv_sec - (int64_t)from->tv_sec) * 1000000000 +
               ((int64_t)to->tv_nsec - (int64_t)from->tv_nsec);
  int64_t ms = ns / 1000000;
  if (ns < 0 && ms * 1000000 != ns) ms--;
  return ms;
}

/*
 * Returns false when the deadline has passed at `now`, which the caller
 * turns into an immediate ccol_timed_out. Otherwise *out_ms gets the
 * milliseconds left, rounded UP. A deadline a fraction of a millisecond
 * away gives 1 and not 0, and a wait of *out_ms milliseconds can't end
 * before the deadline itself, so a poll(2) that times out after that wait
 * always finds the deadline passed. With no active deadline *out_ms is -1
 * and the caller blocks forever.
 */
static bool _deadline_remaining_ms_at(const chttp_deadline_t *d,
                                      const struct timespec *now, int *out_ms) {
  if (!d->active) {
    *out_ms = -1;
    return true;
  }
  /* We do the math in int64_t whatever the widths of long and time_t, and
   * saturate before multiplying. Otherwise a timeout of years, or a
   * deadline saturated at the largest time_t, overflows the product into a
   * negative count that looks like a deadline already gone. The nanosecond
   * difference is strictly between -1 and 1 second, so a negative second
   * count always means the deadline is in the past and a count above
   * INT_MAX / 1000 always means more than INT_MAX milliseconds. */
  int64_t secs = (int64_t)d->deadline.tv_sec - (int64_t)now->tv_sec;
  if (secs < 0) return false;
  if (secs > INT_MAX / 1000) {
    *out_ms = INT_MAX;
    return true;
  }
  int64_t ns = secs * 1000000000 +
               ((int64_t)d->deadline.tv_nsec - (int64_t)now->tv_nsec);
  if (ns <= 0) return false;
  int64_t ms = (ns + 999999) / 1000000;
  *out_ms = (ms > INT_MAX) ? INT_MAX : (int)ms;
  return true;
}

static bool _deadline_remaining_ms(const chttp_deadline_t *d, int *out_ms) {
  if (!d->active) {
    *out_ms = -1;
    return true;
  }
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return _deadline_remaining_ms_at(d, &now, out_ms);
}

#ifdef RUNNING_UNIT_TESTS
/* White-box accessor that runs _deadline_remaining_ms on a deadline add_sec
 * seconds past the current monotonic time. A test can put the deadline at
 * any distance with it, even one whose millisecond product doesn't fit a
 * long. */
bool _chttp_deadline_remaining_ms_for_tests(long long add_sec, int *out_ms) {
  chttp_deadline_t d = {0};
  d.active = true;
  clock_gettime(CLOCK_MONOTONIC, &d.deadline);
  d.deadline.tv_sec = (time_t)((long long)d.deadline.tv_sec + add_sec);
  return _deadline_remaining_ms(&d, out_ms);
}

/* White-box accessor for _ms_elapsed_floor, for tests that feed it two
 * instants on both sides of a second boundary. */
long long _chttp_ms_elapsed_floor_for_tests(long long from_sec, long from_nsec,
                                            long long to_sec, long to_nsec) {
  struct timespec a = {.tv_sec = (time_t)from_sec, .tv_nsec = from_nsec};
  struct timespec b = {.tv_sec = (time_t)to_sec, .tv_nsec = to_nsec};
  return (long long)_ms_elapsed_floor(&a, &b);
}

/* White-box accessor that evaluates _deadline_remaining_ms_at at a fixed
 * instant, for a deadline ahead_ns nanoseconds after it. That way a test
 * checks the rounding without racing the clock. */
bool _chttp_deadline_remaining_ms_at_for_tests(long long ahead_ns,
                                               int *out_ms) {
  struct timespec now = {.tv_sec = 1000, .tv_nsec = 0};
  chttp_deadline_t d = {0};
  d.active = true;
  d.deadline.tv_sec = (time_t)(1000 + ahead_ns / 1000000000LL);
  d.deadline.tv_nsec = (long)(ahead_ns % 1000000000LL);
  if (d.deadline.tv_nsec < 0) {
    d.deadline.tv_nsec += 1000000000L;
    d.deadline.tv_sec -= 1;
  }
  return _deadline_remaining_ms_at(&d, &now, out_ms);
}

/* While this is set, every read point of every tier waits for its fd to be
 * readable and then sees its overall deadline as passed. Tests use it to
 * hit "the deadline passed while the whole response was already buffered"
 * at the same point on each tier, without racing a clock or the deadline
 * sweep. */
static _Atomic bool g_expire_at_read_for_tests = false;

void _chttp_set_expire_at_read_for_tests(bool on) {
  atomic_store(&g_expire_at_read_for_tests, on);
}
#endif

/*
 * All tiers treat a response whose bytes are being read the same way. Before
 * each read of response bytes we check the request's overall deadline, and
 * if it has passed the request ends with ccol_timed_out, even when the rest
 * of the response is already there and the next read would finish it. A
 * response whose last read finished before the deadline passed counts as a
 * success. Tier 1 does this at the top of the read loop of
 * _chttp_read_message, Tier 2 at the start of each read dispatch and before
 * each further read of its TLS drain.
 */
static bool _read_deadline_passed(const chttp_deadline_t *overall, int fd) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&g_expire_at_read_for_tests)) {
    struct pollfd p = {.fd = fd, .events = POLLIN, .revents = 0};
    (void)poll(&p, 1, 5000);
    return true;
  }
#else
  (void)fd;
#endif
  int ms;
  return !_deadline_remaining_ms(overall, &ms);
}

static int _combine_ms(int a, int b) {
  if (a < 0) return b;
  if (b < 0) return a;
  return (a < b) ? a : b;
}

/* Returns whichever of a and b passes first. An "inactive" deadline means
 * no limit, so it never beats an active one. We use this to bound the
 * interim wait for "Expect: 100-continue" by both its own
 * CHTTP_100_CONTINUE_WAIT_US budget and whatever is left of the request's
 * overall deadline, without computing the remaining milliseconds twice in
 * parallel. _tcp_connect takes a connect and overall pair instead, but that
 * shape doesn't fit here because this wait happens inside one
 * _chttp_read_message call, and that call takes a single chttp_deadline_t.
 */
static chttp_deadline_t _deadline_earlier(chttp_deadline_t a,
                                          chttp_deadline_t b) {
  if (!a.active) return b;
  if (!b.active) return a;
  if (a.deadline.tv_sec != b.deadline.tv_sec)
    return (a.deadline.tv_sec < b.deadline.tv_sec) ? a : b;
  return (a.deadline.tv_nsec <= b.deadline.tv_nsec) ? a : b;
}

/* Initializes *cv on CLOCK_MONOTONIC, which is the clock every deadline in
 * this file comes from (through _deadline_make and
 * clock_gettime(CLOCK_MONOTONIC, ...)). Without condattr support on the
 * platform we fall back to the platform's default clock. If we didn't do
 * this, _ccol_cond_var_timedwait would compare a monotonic timespec from
 * _deadline_make with a condition variable that runs on the wall clock
 * (CLOCK_REALTIME by default), and the wait would either return ETIMEDOUT
 * immediately or never honour the deadline at all.
 * _client_deadline_init_globals of client_deadline_bundle sets up its
 * condition variable the same way for the same reason. */
static int _ccol_cond_var_init_monotonic(_ccol_cond_var_t *cv) {
  _ccol_cond_var_attr_t cv_attr;
  if (_ccol_cond_var_attr_init(cv_attr) == 0) {
    _ccol_cond_var_attr_setclock(cv_attr, CLOCK_MONOTONIC);
    int rv = _ccol_cond_var_init_ca(*cv, cv_attr);
    _ccol_cond_var_attr_destroy(cv_attr);
    return rv;
  }
  return _ccol_cond_var_init(*cv);
}

/* ========================================================================== */
/*                         LOW-LEVEL SOCKET / TLS I/O                         */
/* ========================================================================== */

#ifdef RUNNING_UNIT_TESTS
/*
 * Reproduces on demand the one TLS condition a test can't provoke against a
 * cooperating peer: an SSL_write that needs to READ before it can make
 * progress, or an SSL_read that has to WRITE first. A TLS 1.2
 * renegotiation does it, and so does a TLS 1.3 KeyUpdate arriving at the
 * wrong moment. ctls.c reports both as -1 with EWOULDBLOCK and gives the
 * real direction separately through ctls_conn_wants_write(), so an I/O
 * loop that always waits in its own home direction either spins on a
 * level-triggered reactor or waits for a readiness event that never comes.
 *
 * With a mode armed, the first TLS I/O attempt in the named home direction
 * is reported as EWOULDBLOCK without running, and _conn_tls_wants_write
 * then answers with the OPPOSITE direction. The block is released only by
 * a real wait or registration of this module in that opposite direction,
 * so the release is driven by state and not by a clock. Code that keeps
 * waiting in its home direction never releases it and runs out of
 * deadline.
 */
#define CHTTP_TLS_DIR_INJECT_OFF 0
#define CHTTP_TLS_DIR_INJECT_WRITE_WANTS_READ 1
#define CHTTP_TLS_DIR_INJECT_READ_WANTS_WRITE 2

static _Atomic int g_tls_dir_inject_mode = CHTTP_TLS_DIR_INJECT_OFF;
static _Atomic bool g_tls_dir_blocked = false;
static _Atomic bool g_tls_dir_released = false;
/* Only a safety net against a hang, never what makes the test correct. Code
 * that keeps waiting in its home direction is caught by its
 * deadline, and Tier 1's deadline does that. A Tier 2 hop whose
 * registration is stuck in the wrong direction has no deadline of its own
 * once the sweep shuts its fd down though, so it would keep reading
 * forever. After this many refused attempts the injection stops refusing,
 * but it does NOT mark itself released, so a test checking the released
 * state or the interest state fails instead of hanging the whole binary. */
#define CHTTP_TLS_DIR_INJECT_MAX_BLOCKS 256
static _Atomic unsigned g_tls_dir_block_attempts = 0;
static _Atomic unsigned g_tls_dir_read_interest_while_writing = 0;
static _Atomic unsigned g_tls_dir_write_interest_while_reading = 0;

/* True when a TLS I/O attempt with home direction `home_is_write` must not
 * run, in which case we report it as blocked and ask for the other
 * direction. */
static bool _tls_dir_inject_blocks(bool home_is_write) {
  int mode = atomic_load(&g_tls_dir_inject_mode);
  if (mode == CHTTP_TLS_DIR_INJECT_OFF || atomic_load(&g_tls_dir_released))
    return false;
  bool match = home_is_write ? (mode == CHTTP_TLS_DIR_INJECT_WRITE_WANTS_READ)
                             : (mode == CHTTP_TLS_DIR_INJECT_READ_WANTS_WRITE);
  if (!match) return false;
  if (atomic_fetch_add(&g_tls_dir_block_attempts, 1u) >=
      CHTTP_TLS_DIR_INJECT_MAX_BLOCKS)
    return false;
  atomic_store(&g_tls_dir_blocked, true);
  return true;
}

/* Records a wait or a registration in the `waiting_for_write` direction
 * while an injected block is pending. It returns true, and so releases the
 * block, when that's the direction the injection asked for. For Tier 1's
 * poll-based waits it also plays the part of the peer's bytes arriving, so
 * the test never needs a real renegotiation record. A caller waiting in its
 * home direction doesn't match here and waits for real, exactly as it
 * would against a live peer. */
static bool _tls_dir_note_wait(bool waiting_for_write) {
  int mode = atomic_load(&g_tls_dir_inject_mode);
  if (mode == CHTTP_TLS_DIR_INJECT_OFF || !atomic_load(&g_tls_dir_blocked) ||
      atomic_load(&g_tls_dir_released))
    return false;
  bool opposite = (mode == CHTTP_TLS_DIR_INJECT_WRITE_WANTS_READ)
                      ? !waiting_for_write
                      : waiting_for_write;
  if (!opposite) return false;
  atomic_store(&g_tls_dir_released, true);
  return true;
}
#endif /* RUNNING_UNIT_TESTS */

#ifdef RUNNING_UNIT_TESTS
/* When this is non-zero, the first ctls_conn_read of each Tier 2 read
 * dispatch asks for at most this many bytes. A test uses it to leave part
 * of a record inside the TLS layer on purpose; a real peer only does that
 * through record sizes the test has no control over. */
static _Atomic size_t g_async_tls_first_read_cap_for_tests = 0;

void _chttp_set_async_tls_first_read_cap_for_tests(size_t cap) {
  atomic_store(&g_async_tls_first_read_cap_for_tests, cap);
}
#endif

/* The direction a blocked TLS call needs. It's a thin layer over
 * ctls_conn_wants_write so a test build can inject the condition described
 * above, and in an ordinary build it compiles down to the bare call. */
static bool _conn_tls_wants_write(ctls_conn_t *tls) {
#ifdef RUNNING_UNIT_TESTS
  int mode = atomic_load(&g_tls_dir_inject_mode);
  if (mode != CHTTP_TLS_DIR_INJECT_OFF && atomic_load(&g_tls_dir_blocked) &&
      !atomic_load(&g_tls_dir_released))
    return (mode == CHTTP_TLS_DIR_INJECT_READ_WANTS_WRITE);
#endif
  return ctls_conn_wants_write(tls);
}

/*
 * poll() can legitimately fail with EINTR when a signal arrives before any
 * fd is ready. It happens more under tools like valgrind, which use signals
 * internally, but any process can see it. So we retry here and keep track
 * of the elapsed time against the original budget, and a string of
 * interruptions can't stretch the wait beyond what the caller asked for.
 *
 * A POLLERR or POLLNVAL on the fd returns error_rv. Waits of the connect
 * phase (the connect itself and the TLS handshake) pass
 * ccol_http_connection_failed. Waits on an established connection, while a
 * request is being sent or a response read, pass
 * ccol_http_transfer_aborted, which matches what Tier 2 and Tier 3 report
 * for the same event.
 *
 * ccol_timed_out only says that timeout_ms went by without an event. The
 * callers compute timeout_ms from a deadline and cap it at INT_MAX ms, so
 * for them ccol_timed_out is a reason to look at the deadline again and
 * never the end of the request. */
#ifdef RUNNING_UNIT_TESTS
/* As long as this is above 0, a wait capped at INT_MAX ms returns at once
 * with no event, pretending to be a capped wait that ran out. */
static atomic_int g_chttp_short_capped_waits;
void _chttp_short_next_capped_waits_for_tests(int n) {
  atomic_store(&g_chttp_short_capped_waits, n);
}
#endif
static ccol_retval_t _conn_wait(int fd, short events, int timeout_ms,
                                ccol_retval_t error_rv) {
#ifdef RUNNING_UNIT_TESTS
  if (_tls_dir_note_wait((events & POLLOUT) != 0)) return ccol_success;
#endif
  bool has_timeout = (timeout_ms >= 0);
  struct timespec start;
  if (has_timeout) clock_gettime(CLOCK_MONOTONIC, &start);
  int remaining = timeout_ms;

  for (;;) {
    struct pollfd pfd = {.fd = fd, .events = events, .revents = 0};
    int wait = remaining;
#ifdef RUNNING_UNIT_TESTS
    if (wait == INT_MAX && atomic_load(&g_chttp_short_capped_waits) > 0 &&
        atomic_fetch_sub(&g_chttp_short_capped_waits, 1) > 0)
      wait = 0;
#endif
    int rc = poll(&pfd, 1, wait);
    if (rc < 0) {
      if (errno == EINTR) {
        if (has_timeout) {
          struct timespec now;
          clock_gettime(CLOCK_MONOTONIC, &now);
          /* Full nanosecond difference rounded down, so the next wait can't
           * end before timeout_ms has gone by. */
          int64_t elapsed_ms = _ms_elapsed_floor(&start, &now);
          remaining = (int)(timeout_ms - elapsed_ms);
          if (remaining <= 0) return ccol_timed_out;
        }
        continue;
      }
      return ccol_http_transfer_aborted;
    }
    if (rc == 0) return ccol_timed_out;
    if (pfd.revents & (POLLERR | POLLNVAL)) return error_rv;
    return ccol_success;
  }
}

static ssize_t _conn_read(chttp_conn_t *c, void *buf, size_t len) {
  if (c->tls) {
#ifdef RUNNING_UNIT_TESTS
    if (_tls_dir_inject_blocks(false)) {
      errno = EWOULDBLOCK;
      return -1;
    }
#endif
    ssize_t n = ctls_conn_read(c->tls, buf, len);
    if (n < 0 && (errno == EAGAIN)) errno = EWOULDBLOCK;
    return n;
  }
  ssize_t n;
  do {
    n = recv(c->fd, buf, len, 0);
  } while (n < 0 && errno == EINTR);
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) errno = EWOULDBLOCK;
  return n;
}

static ssize_t _conn_write(chttp_conn_t *c, const void *buf, size_t len) {
  if (c->tls) {
#ifdef RUNNING_UNIT_TESTS
    if (_tls_dir_inject_blocks(true)) {
      errno = EWOULDBLOCK;
      return -1;
    }
#endif
    ssize_t n = ctls_conn_write(c->tls, buf, len);
    if (n < 0 && (errno == EAGAIN)) errno = EWOULDBLOCK;
    return n;
  }
  ssize_t n;
  do {
    n = send(c->fd, buf, len, CCOL_MSG_NOSIGNAL);
  } while (n < 0 && errno == EINTR);
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) errno = EWOULDBLOCK;
  return n;
}

static ccol_retval_t _chttp_send_all(chttp_conn_t *conn, const char *data,
                                     size_t len, chttp_deadline_t *overall) {
  size_t sent = 0;
  /* The direction we wait on before the NEXT attempt. It starts as POLLOUT
   * like for a plain send(2), but on a TLS connection we work it out again
   * after every EWOULDBLOCK, since an EWOULDBLOCK from ctls_conn_write()
   * doesn't always mean "wait until writable". After a TLS 1.2
   * renegotiation or a TLS 1.3 KeyUpdate, OpenSSL may have to READ before
   * this very call can move on (see the doc comment of
   * ctls_conn_wants_write). Polling on POLLOUT blindly in that case is a
   * busy loop: the socket is writable, poll(2) comes back at once, and the
   * next write blocks on the same missing read. The thread burns 100% CPU,
   * and with the default request_timeout_us of 0 there isn't even a
   * deadline to stop it. */
  short want_events = POLLOUT;
  while (sent < len) {
    int wait_ms;
    if (!_deadline_remaining_ms(overall, &wait_ms)) return ccol_timed_out;
    ccol_retval_t prv =
        _conn_wait(conn->fd, want_events, wait_ms, ccol_http_transfer_aborted);
    if (prv == ccol_timed_out) continue; /* The loop checks the deadline. */
    if (prv != ccol_success) return prv;
    ssize_t n = _conn_write(conn, data + sent, len - sent);
    if (n < 0) {
      if (errno == EWOULDBLOCK) {
        if (conn->tls)
          want_events = _conn_tls_wants_write(conn->tls) ? POLLOUT : POLLIN;
        continue;
      }
      return ccol_http_transfer_aborted;
    }
    /* The peer closed during the write. */
    if (n == 0) return ccol_http_transfer_aborted;
    sent += (size_t)n;
    /* Some bytes went out, so whatever blocked the previous attempt has been
     * dealt with. The next attempt starts from this call's home direction
     * and only works the direction out again if it blocks too. */
    want_events = POLLOUT;
  }
  return ccol_success;
}

/* Sets TCP_NODELAY on fd, best effort. If it fails the connection is fine
 * and we only lose a latency optimisation. The Tier 1 and Tier 2 connect
 * paths both go through here, so they apply the option the same way and
 * neither sets socket options of its own. A unix domain socket has no TCP
 * layer and the option means nothing there, so callers only use this for
 * an AF_INET or AF_INET6 connection. */
static void _apply_tcp_nodelay(int fd) {
  int one = 1;
  (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

/* One resolved address of a TCP target, copied out of the getaddrinfo()
 * result so it can live longer than that result. */
typedef struct {
  struct sockaddr_storage addr;
  socklen_t addr_len;
  int protocol;
} chttp_resolved_addr_t;

/* The candidate addresses of one TCP target. _addr_list_resolve keeps the
 * order getaddrinfo() returns, which is the RFC 6724 destination address
 * selection that the C library's resolver applies, and
 * _addr_list_interleave then alternates the address families starting with
 * the family of the first address (RFC 8305 section 4). So IPv6 comes
 * first whenever the resolver prefers it, which is its default on a host
 * with IPv6 connectivity.
 *
 * All tiers connect with one policy, Happy Eyeballs (RFC 8305). The first
 * candidate starts at once, and the next one starts either when the
 * attempts in flight have had no result for CHTTP_CONNECT_ATTEMPT_DELAY_MS
 * or immediately when an attempt fails. The first attempt to connect wins
 * and all the others are closed. The connect deadline is ONE budget for the
 * whole race and never gets renewed per candidate; once it expires the race
 * ends with ccol_timed_out. The race only fails with
 * ccol_http_connection_failed after every candidate has failed.
 *
 * An address that never answers (say an IPv6 route that drops its packets)
 * costs a quarter of a second this way instead of the whole connect
 * timeout. A server listening on 127.0.0.1 only can also be reached as
 * "localhost", which a standard /etc/hosts lists as ::1 before 127.0.0.1:
 * ::1 refuses at once and 127.0.0.1 starts right away. For a client that
 * talks to hosts it doesn't control, that's a much better trade than trying
 * the addresses strictly one after another. */
typedef struct {
  size_t count;
  chttp_resolved_addr_t addrs[];
} chttp_addr_list_t;

/* True when host is an IPv4 or IPv6 address literal (IPv6 without the
 * brackets), so no name lookup is needed. */
static bool _host_is_ip_literal(const char *host) {
  unsigned char buf[sizeof(struct in6_addr)];
  return inet_pton(AF_INET, host, buf) == 1 ||
         inet_pton(AF_INET6, host, buf) == 1;
}

/* Resolves host:port into a new list allocated from mp, or returns
 * ccol_http_host_resolution_failed when the name resolves to nothing. An
 * address literal gets converted without any lookup. */
static ccol_retval_t _addr_list_resolve(ccol_memmgmt_procs_t *mp,
                                        const char *host, uint16_t port,
                                        chttp_addr_list_t **out) {
  *out = NULL;
  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (_host_is_ip_literal(host)) hints.ai_flags = AI_NUMERICHOST;

  struct addrinfo *res = NULL;
  if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
    return ccol_http_host_resolution_failed;

  size_t count = 0;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next)
    if (ai->ai_addrlen <= sizeof(struct sockaddr_storage)) count++;
  if (count == 0) {
    freeaddrinfo(res);
    return ccol_http_host_resolution_failed;
  }

  chttp_addr_list_t *l = (chttp_addr_list_t *)_ccol_mem_alloc(
      mp, sizeof(*l) + count * sizeof(l->addrs[0]));
  if (!l) {
    freeaddrinfo(res);
    return ccol_not_enough_memory;
  }
  size_t i = 0;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    if (ai->ai_addrlen > sizeof(struct sockaddr_storage)) continue;
    memset(&l->addrs[i].addr, 0, sizeof(l->addrs[i].addr));
    memcpy(&l->addrs[i].addr, ai->ai_addr, ai->ai_addrlen);
    l->addrs[i].addr_len = (socklen_t)ai->ai_addrlen;
    l->addrs[i].protocol = ai->ai_protocol;
    i++;
  }
  l->count = count;
  freeaddrinfo(res);
  *out = l;
  return ccol_success;
}

/* Reorders l so the address families alternate, starting with the family
 * of the first address, while each family keeps the resolver's order
 * (RFC 8305 section 4 with a First Address Family Count of 1). A list has
 * a handful of addresses, so the quadratic rotation costs next to nothing
 * and it doesn't allocate. */
static void _addr_list_interleave(chttp_addr_list_t *l) {
  if (l->count < 3)
    return; /* With two addresses they alternate or are
             * the same family anyway. */
  sa_family_t first = l->addrs[0].addr.ss_family;
  for (size_t i = 1; i < l->count; i++) {
    bool want_first = (i % 2) == 0;
    size_t j = i;
    while (j < l->count &&
           ((l->addrs[j].addr.ss_family == first) != want_first))
      j++;
    if (j == l->count)
      break; /* One family ran out, the rest stays in its
              * order. */
    if (j != i) {
      chttp_resolved_addr_t moved = l->addrs[j];
      memmove(&l->addrs[i + 1], &l->addrs[i], (j - i) * sizeof(l->addrs[0]));
      l->addrs[i] = moved;
    }
  }
}

/* Starts a non-blocking connect to a and returns the fd, with *in_progress
 * set to true while the connect is pending (EINPROGRESS) and false when it
 * already completed. Returns -1 if socket() or connect() fails at once.
 *
 * All sockets of this module are close-on-exec. A child the process spawns
 * (system(), posix_spawn(), fork() and exec()) must not inherit a
 * connection, otherwise the peer sees no FIN when we close it for as long
 * as the child lives, and the child could read and write the stream. */
#ifdef RUNNING_UNIT_TESTS
static bool _addr_is_silent_for_tests(const chttp_resolved_addr_t *a);
static int _silent_attempt_fd_for_tests(void);
#endif

static int _addr_connect_start(const chttp_resolved_addr_t *a,
                               bool *in_progress) {
  int fd = ccol_socket_nb(a->addr.ss_family, SOCK_STREAM, a->protocol);
  if (fd < 0) return -1;
#ifdef RUNNING_UNIT_TESTS
  if (_addr_is_silent_for_tests(a)) {
    close(fd);
    *in_progress = true;
    return _silent_attempt_fd_for_tests();
  }
#endif
  if (connect(fd, (const struct sockaddr *)&a->addr, a->addr_len) == 0) {
    *in_progress = false;
#ifdef RUNNING_UNIT_TESTS
    if (atomic_load(&g_connect_async_failures_for_tests) > 0)
      *in_progress = true;
#endif
    return fd;
  }
  if (errno == EINPROGRESS) {
    *in_progress = true;
    return fd;
  }
  close(fd);
  return -1;
}

#ifdef RUNNING_UNIT_TESTS
/* White-box hook. An attempt on an address in this set doesn't get a
 * connecting TCP socket but one end of a Unix socket pair whose send buffer
 * is full and whose peer stays open and quiet. It never becomes writable,
 * never reports an error, and after a shutdown(2) it reports EOF both ways,
 * which is exactly what a TCP connect with a dropped SYN looks like (on
 * Linux the tests get that shape from a listener with a full accept queue,
 * but the BSDs and macOS answer such a SYN). Every peer end stays open
 * until the set is emptied, either by a NULL address or by clearing the
 * address override. */
enum { SILENT_ADDRS_FOR_TESTS = 4, SILENT_PEERS_FOR_TESTS = 32 };
static struct sockaddr_storage g_silent_addrs_for_tests[SILENT_ADDRS_FOR_TESTS];
static socklen_t g_silent_lens_for_tests[SILENT_ADDRS_FOR_TESTS];
static atomic_int g_silent_count_for_tests;
/* The peer ends, stored as fd + 1 so a 0 marks an empty slot. */
static atomic_int g_silent_peers_for_tests[SILENT_PEERS_FOR_TESTS];

static int _silent_attempt_fd_for_tests(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
  for (int i = 0; i < 2; i++) {
    int fl = fcntl(sv[i], F_GETFL, 0);
    (void)fcntl(sv[i], F_SETFL, fl | O_NONBLOCK);
    (void)fcntl(sv[i], F_SETFD, FD_CLOEXEC);
  }
  char block[4096];
  memset(block, 0, sizeof(block));
  while (send(sv[0], block, sizeof(block), CCOL_MSG_NOSIGNAL) > 0) {
  }
  for (int i = 0; i < SILENT_PEERS_FOR_TESTS; i++) {
    int empty = 0;
    if (atomic_compare_exchange_strong(&g_silent_peers_for_tests[i], &empty,
                                       sv[1] + 1))
      return sv[0];
  }
  close(sv[0]);
  close(sv[1]);
  return -1;
}

void _chttp_set_silent_addr_for_tests(const struct sockaddr *addr,
                                      socklen_t len) {
  if (!addr) {
    atomic_store(&g_silent_count_for_tests, 0);
    for (int i = 0; i < SILENT_PEERS_FOR_TESTS; i++) {
      int stored = atomic_exchange(&g_silent_peers_for_tests[i], 0);
      if (stored > 0) close(stored - 1);
    }
    return;
  }
  int n = atomic_load(&g_silent_count_for_tests);
  if (n >= SILENT_ADDRS_FOR_TESTS || len > sizeof(g_silent_addrs_for_tests[0]))
    return;
  memcpy(&g_silent_addrs_for_tests[n], addr, len);
  g_silent_lens_for_tests[n] = len;
  atomic_store(&g_silent_count_for_tests, n + 1);
}

static bool _addr_is_silent_for_tests(const chttp_resolved_addr_t *a) {
  int n = atomic_load(&g_silent_count_for_tests);
  for (int i = 0; i < n; i++) {
    if (g_silent_lens_for_tests[i] == a->addr_len &&
        memcmp(&g_silent_addrs_for_tests[i], &a->addr, a->addr_len) == 0)
      return true;
  }
  return false;
}

/* White-box hook. While it's non-NULL, _addr_list_resolve_for_target skips
 * getaddrinfo() and answers every TCP resolution with this list in this
 * order, so a test can hand over a candidate list whose first address
 * refuses no matter what the machine's /etc/hosts says. */
static _Atomic(const chttp_addr_list_t *) g_addr_list_override_for_tests = NULL;

void _chttp_set_addr_override_for_tests(const struct sockaddr *const *addrs,
                                        const socklen_t *lens, size_t count) {
  enum { MAX_OVERRIDE_ADDRS = 8 };
  static _Alignas(max_align_t) unsigned char
      storage[sizeof(chttp_addr_list_t) +
              MAX_OVERRIDE_ADDRS * sizeof(chttp_resolved_addr_t)];
  chttp_addr_list_t *l = (chttp_addr_list_t *)(void *)storage;
  if (!addrs || count == 0) {
    atomic_store(&g_addr_list_override_for_tests, NULL);
    _chttp_set_silent_addr_for_tests(NULL, 0);
    return;
  }
  if (count > MAX_OVERRIDE_ADDRS) count = MAX_OVERRIDE_ADDRS;
  memset(storage, 0, sizeof(storage));
  for (size_t i = 0; i < count; i++) {
    memcpy(&l->addrs[i].addr, addrs[i], lens[i]);
    l->addrs[i].addr_len = lens[i];
    l->addrs[i].protocol = 0;
  }
  l->count = count;
  atomic_store(&g_addr_list_override_for_tests, l);
}
#endif

#ifdef RUNNING_UNIT_TESTS
/* White-box hook: builds a list where address i has family families[i] and
 * port i, runs _addr_list_interleave on it and writes the resulting order
 * of the original indexes to order_out. */
void _chttp_addr_list_interleave_for_tests(const int *families, size_t n,
                                           size_t *order_out) {
  enum { MAX_ADDRS = 16 };
  static _Alignas(max_align_t) unsigned char
      storage[sizeof(chttp_addr_list_t) +
              MAX_ADDRS * sizeof(chttp_resolved_addr_t)];
  chttp_addr_list_t *l = (chttp_addr_list_t *)(void *)storage;
  if (n > MAX_ADDRS) n = MAX_ADDRS;
  memset(storage, 0, sizeof(storage));
  for (size_t i = 0; i < n; i++) {
    l->addrs[i].addr.ss_family = (sa_family_t)families[i];
    ((struct sockaddr_in *)&l->addrs[i].addr)->sin_port = htons((uint16_t)i);
  }
  l->count = n;
  _addr_list_interleave(l);
  for (size_t i = 0; i < n; i++)
    order_out[i] = ntohs(((struct sockaddr_in *)&l->addrs[i].addr)->sin_port);
}
#endif

#ifdef RUNNING_UNIT_TESTS
/* White-box hook. When non-zero, every name lookup (address literals are
 * never affected) sleeps this many milliseconds first, playing a resolver
 * that doesn't answer. */
static _Atomic unsigned g_name_lookup_delay_ms_for_tests = 0;

void _chttp_set_name_lookup_delay_ms_for_tests(unsigned ms) {
  atomic_store(&g_name_lookup_delay_ms_for_tests, ms);
}

/* True from _chttp_sweep_fd_wait_arm_for_tests() until a sweep tick catches
 * the armed hop before its connect (see _sweep_fd_wait_for_tests). While it's
 * set, a name lookup waits for that tick after its delay, with a bound, so the
 * test never depends on a 100 ms tick landing inside the delay; on a slow
 * runner a sleep can end too late for that. */
static _Atomic bool g_sweep_fd_wait_pending_for_tests = false;

/* Defined with the deadline sweep further down. The wait above uses them to
 * ask for a walk at once instead of waiting for the sweep's next planned
 * instant, which a slow runner (ThreadSanitizer on macOS arm64 among them)
 * can push past any fixed bound. */
static uint64_t _monotonic_now_ns(void);
static void _client_deadline_note_ns(uint64_t at_ns);

/* White-box count of the name lookups started by all tiers (address
 * literals never count). */
static _Atomic unsigned long g_name_lookups_for_tests = 0;

unsigned long _chttp_name_lookups_for_tests(void) {
  return atomic_load(&g_name_lookups_for_tests);
}
#endif

/* The resolution step all tiers call. In a test build its answer can be
 * replaced, see _chttp_set_addr_override_for_tests. */
static ccol_retval_t _addr_list_resolve_for_target(ccol_memmgmt_procs_t *mp,
                                                   const char *host,
                                                   uint16_t port,
                                                   chttp_addr_list_t **out) {
#ifdef RUNNING_UNIT_TESTS
  if (!_host_is_ip_literal(host))
    atomic_fetch_add_explicit(&g_name_lookups_for_tests, 1,
                              memory_order_relaxed);
  unsigned delay_ms = atomic_load(&g_name_lookup_delay_ms_for_tests);
  if (delay_ms && !_host_is_ip_literal(host)) {
    struct timespec ts = {(time_t)(delay_ms / 1000),
                          (long)(delay_ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
    struct timespec tick = {0, 1000000};
    for (int i = 0; i < 5000 && atomic_load(&g_sweep_fd_wait_pending_for_tests);
         i++) {
      _client_deadline_note_ns(_monotonic_now_ns());
      nanosleep(&tick, NULL);
    }
  }
  const chttp_addr_list_t *ov = atomic_load(&g_addr_list_override_for_tests);
  if (ov) {
    size_t bytes = sizeof(*ov) + ov->count * sizeof(ov->addrs[0]);
    chttp_addr_list_t *l = (chttp_addr_list_t *)_ccol_mem_alloc(mp, bytes);
    if (!l) return ccol_not_enough_memory;
    memcpy(l, ov, bytes);
    *out = l;
    return ccol_success;
  }
#endif
  return _addr_list_resolve(mp, host, port, out);
}

/* Milliseconds from now until `at` rounded up, or 0 once `at` is past. */
static int _ms_until(const struct timespec *at, const struct timespec *now) {
  chttp_deadline_t d = {.active = true, .deadline = *at};
  int ms;
  return _deadline_remaining_ms_at(&d, now, &ms) ? ms : 0;
}

static struct timespec _timespec_add_ms(struct timespec t, long ms) {
  t.tv_sec += ms / 1000;
  t.tv_nsec += (ms % 1000) * 1000000L;
  if (t.tv_nsec >= 1000000000L) {
    t.tv_nsec -= 1000000000L;
    t.tv_sec += 1;
  }
  return t;
}

/* Resolves host:port and connects with the Happy Eyeballs race described at
 * chttp_addr_list_t. Both the connect deadline and the request's overall
 * deadline apply, and whichever is tighter wins. */
static ccol_retval_t _tcp_connect(ccol_memmgmt_procs_t *mp, const char *host,
                                  uint16_t port, chttp_deadline_t *connect_dl,
                                  chttp_deadline_t *overall, int *fd_out) {
  chttp_addr_list_t *list = NULL;
  ccol_retval_t rrv = _addr_list_resolve_for_target(mp, host, port, &list);
  if (rrv != ccol_success) return rrv;
  _addr_list_interleave(list);

  /* A slot per attempt that may be in flight at the same time. A resolver
   * rarely returns more than a few addresses and those fit on the stack. */
  struct pollfd stack_pfds[8];
  struct pollfd *pfds = stack_pfds;
  if (list->count > sizeof(stack_pfds) / sizeof(stack_pfds[0])) {
    pfds = (struct pollfd *)_ccol_mem_alloc(mp, list->count * sizeof(*pfds));
    if (!pfds) {
      _ccol_mem_free(mp, list);
      return ccol_not_enough_memory;
    }
  }
  size_t npend = 0;
  size_t next = 0;
  int winner = -1;
  ccol_retval_t result = ccol_http_connection_failed;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  struct timespec next_start = now;

  for (;;) {
    /* The next candidate starts when nothing is in flight or once the
     * attempts in flight have had their delay. If a candidate fails
     * immediately, the one after it starts without waiting. */
    bool due = npend == 0 || _ms_until(&next_start, &now) == 0;
    while (due && next < list->count && winner < 0) {
      bool in_progress = false;
      int fd = _addr_connect_start(&list->addrs[next++], &in_progress);
      if (fd < 0) continue;
      if (!in_progress) {
        winner = fd;
        break;
      }
      pfds[npend].fd = fd;
      pfds[npend].events = POLLOUT;
      pfds[npend].revents = 0;
      npend++;
      next_start = _timespec_add_ms(now, _connect_attempt_delay_ms());
      break;
    }
    if (winner >= 0) {
      result = ccol_success;
      break;
    }
    if (npend == 0) {
      result = ccol_http_connection_failed;
      break;
    }

    int wait_ms, overall_ms;
    if (!_deadline_remaining_ms_at(connect_dl, &now, &wait_ms) ||
        !_deadline_remaining_ms_at(overall, &now, &overall_ms)) {
      result = ccol_timed_out;
      break;
    }
    wait_ms = _combine_ms(wait_ms, overall_ms);
    if (next < list->count)
      wait_ms = _combine_ms(wait_ms, _ms_until(&next_start, &now));

    int rc = poll(pfds, (nfds_t)npend, wait_ms);
    if (rc < 0 && errno != EINTR) {
      result = ccol_http_connection_failed;
      break;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (rc <= 0) continue;

    size_t kept = 0;
    for (size_t i = 0; i < npend; i++) {
      if (pfds[i].revents == 0 || winner >= 0) {
        pfds[kept++] = pfds[i];
        continue;
      }
      int soerr = 0;
      socklen_t slen = sizeof(soerr);
      int gso = getsockopt(pfds[i].fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
#ifdef RUNNING_UNIT_TESTS
      if (gso == 0 && soerr == 0 &&
          _take_one_for_tests(&g_connect_async_failures_for_tests))
        soerr = ECONNREFUSED;
#endif
      if (gso == 0 && soerr == 0) {
        winner = pfds[i].fd;
        continue;
      }
      close(pfds[i].fd);
      /* A failed attempt lets the next candidate start immediately. */
      next_start = now;
    }
    npend = kept;
    if (winner >= 0) {
      result = ccol_success;
      break;
    }
  }

  /* Close whatever attempts didn't win. */
  for (size_t i = 0; i < npend; i++) close(pfds[i].fd);
  if (pfds != stack_pfds) _ccol_mem_free(mp, pfds);
  _ccol_mem_free(mp, list);
  if (result == ccol_success) {
    _apply_tcp_nodelay(winner);
    *fd_out = winner;
  }
  return result;
}

/* Connects to the unix domain stream socket at path with the same connect
 * and overall deadlines as _tcp_connect, but with no TCP_NODELAY and no
 * DNS. On Linux a unix domain connect() almost never gives EINPROGRESS
 * since the kernel services the accept queue synchronously. We keep the
 * non-blocking socket and the poll() step anyway, so the code stays
 * portable and the deadline holds even in that rare case. */
static ccol_retval_t _unix_connect(const char *path,
                                   chttp_deadline_t *connect_dl,
                                   chttp_deadline_t *overall, int *fd_out) {
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  size_t path_len = strlen(path);
  if (path_len >= sizeof(addr.sun_path)) return ccol_http_invalid_url;
  memcpy(addr.sun_path, path, path_len + 1);

  int fd = ccol_socket_nb(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return ccol_http_connection_failed;

  int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  if (rc == 0) {
    *fd_out = fd;
    return ccol_success;
  }
  if (errno != EINPROGRESS) {
    close(fd);
    return ccol_http_connection_failed;
  }

  ccol_retval_t prv;
  do {
    int wait_ms, overall_ms;
    if (!_deadline_remaining_ms(connect_dl, &wait_ms) ||
        !_deadline_remaining_ms(overall, &overall_ms)) {
      close(fd);
      return ccol_timed_out;
    }
    prv = _conn_wait(fd, POLLOUT, _combine_ms(wait_ms, overall_ms),
                     ccol_http_connection_failed);
  } while (prv == ccol_timed_out); /* The loop checks the deadlines. */
  if (prv != ccol_success) {
    close(fd);
    return prv;
  }

  int soerr = 0;
  socklen_t slen = sizeof(soerr);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0 || soerr != 0) {
    close(fd);
    return ccol_http_connection_failed;
  }

  *fd_out = fd;
  return ccol_success;
}

/* Runs the client-mode TLS handshake to completion within both the connect
 * deadline and the overall deadline. Like in curl, the handshake time is
 * charged to the connect timeout. */
static ccol_retval_t _tls_handshake(chttp_conn_t *conn,
                                    chttp_deadline_t *connect_dl,
                                    chttp_deadline_t *overall) {
  for (;;) {
    ctls_handshake_result_t r = ctls_conn_handshake_step(conn->tls);
    if (r == CTLS_HANDSHAKE_DONE) return ccol_success;
    if (r == CTLS_HANDSHAKE_ERROR) {
      /* In OpenSSL 0 is X509_V_OK. ctls.h keeps the OpenSSL headers away
       * from its callers on purpose, so we compare the raw value here and
       * not the X509_V_OK symbol. */
      long vr = ctls_conn_verify_result(conn->tls);
      return (vr != 0) ? ccol_http_tls_cert_verification_failed
                       : ccol_http_tls_handshake_failed;
    }

    short ev = (r == CTLS_HANDSHAKE_WANT_WRITE) ? POLLOUT : POLLIN;
    int wait_ms, overall_ms;
    if (!_deadline_remaining_ms(connect_dl, &wait_ms)) return ccol_timed_out;
    if (!_deadline_remaining_ms(overall, &overall_ms)) return ccol_timed_out;
    /* TCP is already connected, so a socket error at this point (say the
     * peer resets during the handshake) counts as a failed handshake, the
     * way Tiers 2 and 3 report it, and not as a failed connect. */
    ccol_retval_t prv =
        _conn_wait(conn->fd, ev, _combine_ms(wait_ms, overall_ms),
                   ccol_http_tls_handshake_failed);
    if (prv == ccol_timed_out) continue; /* The loop checks the deadlines. */
    if (prv != ccol_success) return prv;
  }
}

/* Maps a failure to create the client side of a TLS connection over fd to
 * the code the request reports. ctls_conn_create_client refuses to verify
 * a name when it isn't given one. That has nothing to do with memory: the
 * URL's host can't be checked against a certificate, which makes the URL
 * useless for https in the first place, so we report
 * ccol_http_invalid_url. ctls only describes a failure through its static
 * err_str, so the text of that refusal is what we look at. Any other ctls
 * failure here is either an allocation inside OpenSSL or ctls
 * (ccol_not_enough_memory) or arguments that ctls refuses, which would be
 * an internal fault of this module. */
static ccol_retval_t _tls_client_create_failure_code(const char *err) {
  if (!err) return ccol_not_enough_memory;
  if (strstr(err, "verification requested with no hostname"))
    return ccol_http_invalid_url;
  if (strstr(err, "invalid arguments")) return ccol_unexpected_failure;
  return ccol_not_enough_memory;
}

/* A fully qualified name may end in one dot, like "www.example.com.". The
 * dot anchors the name at the DNS root, which is why the connect keeps it,
 * but no certificate names a host with that dot and the SNI extension
 * forbids it (RFC 6066 section 3). So, as curl and Go's crypto/tls do, we
 * strip exactly one trailing dot from the name TLS sends and verifies,
 * while the Host header keeps the name as the URL wrote it. A DNS name is
 * at most 253 characters (254 with the dot), which fits the buffer. A
 * longer host isn't a DNS name at all and goes to TLS unchanged, where no
 * certificate can match it. */
static ctls_conn_t *_tls_client_create(ctls_ctx_t *tls_ctx, int fd,
                                       const char *host, bool verify_host,
                                       ccol_retval_t *rv_out) {
  char undotted[256];
  const char *tls_name = host;
  size_t hlen = host ? strlen(host) : 0;
  if (hlen > 1 && host[hlen - 1] == '.' && hlen - 1 < sizeof(undotted)) {
    memcpy(undotted, host, hlen - 1);
    undotted[hlen - 1] = '\0';
    tls_name = undotted;
  }
  char *err = NULL;
  ctls_conn_t *c =
      ctls_conn_create_client(tls_ctx, fd, tls_name, verify_host, &err);
  *rv_out = c ? ccol_success : _tls_client_create_failure_code(err);
  return c;
}

#ifdef RUNNING_UNIT_TESTS
/* White-box helper that runs _tls_client_create on one end of a fresh
 * socket pair with its own TLS context and returns the code a request
 * would report. The parser accepts no URL with an empty host, so this is
 * the only way to get to the ctls refusal. */
ccol_retval_t _chttp_tls_client_create_code_for_tests(const char *host,
                                                      bool verify_host) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
    return ccol_unexpected_failure;
  ctls_ctx_t *tctx = ctls_ctx_new_mp(NULL, NULL);
  ccol_retval_t rv = ccol_unexpected_failure;
  if (tctx) {
    ctls_conn_t *c = _tls_client_create(tctx, sv[0], host, verify_host, &rv);
    if (c) ctls_conn_destroy(c);
    ctls_ctx_release(tctx);
  }
  close(sv[0]);
  close(sv[1]);
  return rv;
}

/* White-box helper running a full handshake over a fresh socket pair. On
 * the client side it's _tls_client_create for host with name verification,
 * trusting only ca_path. The server side offers default_cert as the default
 * certificate and named_cert under the SNI name named_as. Returns true when
 * both sides get through the handshake. */
bool _chttp_tls_client_handshake_for_tests(
    const char *host, const char *ca_path, const char *default_cert,
    const char *default_key, const char *named_as, const char *named_cert,
    const char *named_key) {
  int sv[2];
  if (ccol_socketpair_nb(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return false;
  bool ok = false;
  ctls_ctx_t *cctx = ctls_ctx_new_mp(NULL, NULL);
  ctls_ctx_t *sctx = ctls_ctx_new_mp(NULL, NULL);
  ctls_conn_t *cc = NULL, *sc = NULL;
  ccol_retval_t rv = ccol_unexpected_failure;
  if (cctx && sctx && ctls_ctx_trust(cctx, ca_path, NULL) == ccol_success &&
      ctls_ctx_cert_add(sctx, NULL, default_cert, default_key, NULL, NULL) ==
          ccol_success &&
      ctls_ctx_cert_add(sctx, named_as, named_cert, named_key, NULL, NULL) ==
          ccol_success) {
    cc = _tls_client_create(cctx, sv[0], host, true, &rv);
    sc = ctls_conn_create_server(sctx, sv[1], NULL, NULL);
  }
  if (cc && sc) {
    bool cdone = false, sdone = false, failed = false;
    for (int i = 0; i < 2000 && !failed && !(cdone && sdone); i++) {
      if (!cdone) {
        ctls_handshake_result_t r = ctls_conn_handshake_step(cc);
        cdone = (r == CTLS_HANDSHAKE_DONE);
        failed = (r == CTLS_HANDSHAKE_ERROR);
      }
      if (!sdone && !failed) {
        ctls_handshake_result_t r = ctls_conn_handshake_step(sc);
        sdone = (r == CTLS_HANDSHAKE_DONE);
        failed = (r == CTLS_HANDSHAKE_ERROR);
      }
      if (!failed && !(cdone && sdone)) {
        struct pollfd p[2] = {{.fd = sv[0], .events = POLLIN},
                              {.fd = sv[1], .events = POLLIN}};
        (void)poll(p, 2, 5);
      }
    }
    ok = cdone && sdone && !failed;
  }
  if (cc) ctls_conn_destroy(cc);
  if (sc) ctls_conn_destroy(sc);
  if (cctx) ctls_ctx_release(cctx);
  if (sctx) ctls_ctx_release(sctx);
  close(sv[0]);
  close(sv[1]);
  return ok;
}
#endif

/* tls_generation is the copy of chttpclient.tls_generation that the caller
 * took under cli->lock along with tls_ctx itself. We stamp it on the new
 * connection so the idle pool can later tell whether that connection's
 * handshake ran under the TLS configuration that is current (see the
 * field comment of chttpclient.tls_generation). The caller passes it in
 * rather than us reading it here, since this function is meant to know
 * nothing about the client that owns the connection it opens. */
static ccol_retval_t _conn_open(ccol_memmgmt_procs_t *mp,
                                const chttp_url_t *url, bool want_tls,
                                ctls_ctx_t *tls_ctx, bool verify_host,
                                uint64_t tls_generation,
                                chttp_deadline_t *connect_dl,
                                chttp_deadline_t *overall, chttp_conn_t *out) {
  memset(out, 0, sizeof(*out));
  out->fd = -1;
  out->tls_generation = tls_generation;

  int fd = -1;
  ccol_retval_t rv =
      url->is_unix
          ? _unix_connect(url->unix_socket_path, connect_dl, overall, &fd)
          : _tcp_connect(mp, url->host, url->port, connect_dl, overall, &fd);
  if (rv != ccol_success) return rv;
  out->fd = fd;

  if (want_tls) {
    ccol_retval_t crv;
    out->tls = _tls_client_create(tls_ctx, fd, url->host, verify_host, &crv);
    if (!out->tls) {
      close(fd);
      out->fd = -1;
      return crv;
    }
    rv = _tls_handshake(out, connect_dl, overall);
    if (rv != ccol_success) {
      ctls_conn_destroy(out->tls);
      close(fd);
      out->fd = -1;
      out->tls = NULL;
      return rv;
    }
  }

  out->origin_key = ccol_strdup(mp, url->origin_key);
  if (!out->origin_key) {
    if (out->tls) ctls_conn_destroy(out->tls);
    close(fd);
    out->fd = -1;
    out->tls = NULL;
    return ccol_not_enough_memory;
  }

  clock_gettime(CLOCK_MONOTONIC, &out->last_used);
  return ccol_success;
}

static void _conn_teardown(ccol_memmgmt_procs_t *mp, chttp_conn_t *c) {
  if (!c) return;
  if (c->tls) ctls_conn_destroy(c->tls);
  if (c->fd >= 0) close(c->fd);
  _ccol_mem_free(mp, c->origin_key);
  memset(c, 0, sizeof(*c));
  c->fd = -1;
}

/* ========================================================================== */
/*                         CONCURRENCY LIMITER                                */
/* ========================================================================== */

static size_t _resolve_pool_cap(size_t configured) {
  if (configured != 0) return configured;
  long np = sysconf(_SC_NPROCESSORS_ONLN);
  return (np > 0) ? (size_t)np : 1;
}

/*
 * Takes a slot of the concurrency limiter, blocking while the pool is at
 * capacity. `deadline` bounds how long we block for a slot (a NULL
 * deadline and an inactive chttp_deadline_t both mean no limit), and if it
 * passes first we return ccol_timed_out instead of waiting forever.
 *
 * That's what keeps the documented contract of
 * chttpclient_set_request_timeout ("the maximum time from when
 * chttpclient_do is called...") true while the pool sized by
 * chttpclient_set_pool_size is full. Our caller, chttp_do_internal,
 * computes the overall deadline BEFORE calling us, so the time spent
 * waiting here is charged to that deadline, whereas an unconditional
 * _ccol_cond_var_wait would hide it from the deadline.
 *
 * cli->available is set up on CLOCK_MONOTONIC by
 * _ccol_cond_var_init_monotonic and deadline->deadline is a
 * CLOCK_MONOTONIC timespec from _deadline_make, so we can pass it straight
 * to _ccol_cond_var_timedwait.
 */
static ccol_retval_t _slot_acquire(struct chttpclient *cli,
                                   const chttp_deadline_t *deadline) {
  _ccol_mutex_lock(cli->lock);
  if (cli->destroying) {
    _ccol_mutex_unlock(cli->lock);
    return ccol_not_permitted;
  }
  if (!cli->pool_initialized) {
    cli->pool_cap = _resolve_pool_cap(cli->configured_pool_size);
    cli->pool_initialized = true;
  }
  while (cli->in_flight_count >= cli->pool_cap && !cli->destroying) {
    if (deadline && deadline->active) {
      int wrc = _ccol_cond_var_timedwait(cli->available, cli->lock,
                                         deadline->deadline);
      if (wrc == ETIMEDOUT) {
        _ccol_mutex_unlock(cli->lock);
        return ccol_timed_out;
      }
      /* Anything else, a real wake or a spurious one, falls through to the
       * loop condition above, the same way a spurious wakeup of
       * _ccol_cond_var_wait is handled. */
    } else {
      _ccol_cond_var_wait(cli->available, cli->lock);
    }
  }
  if (cli->destroying) {
    _ccol_mutex_unlock(cli->lock);
    return ccol_not_permitted;
  }
  cli->in_flight_count++;
  _ccol_mutex_unlock(cli->lock);
  return ccol_success;
}

static void _slot_release(struct chttpclient *cli) {
  _ccol_mutex_lock(cli->lock);
  cli->in_flight_count--;
  _ccol_cond_var_broadcast(cli->available);
  _ccol_mutex_unlock(cli->lock);
}

/* ========================================================================== */
/*                         KEEP-ALIVE IDLE POOL                               */
/* ========================================================================== */

/* The SSO storage of chmap_entry is naturally aligned, so nothing really
 * needs this memcpy for alignment; it's a second line of defence. We keep
 * it so the pointer reads look the same as in cjson, cyaml, clrucache and
 * cthreadcomm, which store their pointers in a chmap the same way. */
static inline cvec _read_cvec(const void *src) {
  cvec v;
  memcpy(&v, src, sizeof(v));
  return v;
}

/*
 * Pops a usable idle connection for `origin_key` into *out, or returns
 * false when there is none. It may pop and throw away several stale or dead
 * candidates before it finds a live one or the list runs empty. The age
 * check and the liveness probe both run OUTSIDE the client's lock, since we
 * never do I/O while holding that lock.
 */
static bool _idle_pool_take(struct chttpclient *cli, const char *origin_key,
                            chttp_conn_t *out) {
  for (;;) {
    bool got = false;
    uint64_t cur_tls_generation;
    _ccol_mutex_lock(cli->lock);
    cur_tls_generation = cli->tls_generation;
    if (cli->idle_pools) {
      cmap_pair kp = {.ptr = (void *)origin_key,
                      .size = strlen(origin_key) + 1};
      cmap_pair vp;
      if (chmap_get_elem_ref(cli->idle_pools, &kp, &vp) == ccol_success) {
        cvec list = _read_cvec(vp.ptr);
        if (list && cvector_elem_count(list) > 0 &&
            cvector_pop_back(list, out) == ccol_success) {
          got = true;
          if (cli->idle_total_count > 0) cli->idle_total_count--;
          /* Once the list of this origin is empty we drop it together with
           * its map entry instead of leaving both behind forever. A client
           * that lives long, a crawler or a service with a lot of outbound
           * traffic, can talk to many distinct origins, and without this
           * prune it keeps one permanent chmap entry plus an empty cvec for
           * every origin it EVER pooled a connection for.
           * CHTTP_MAX_IDLE_TOTAL and CHTTP_MAX_IDLE_PER_ORIGIN don't help
           * there, they count live connections and never distinct origin
           * keys. Removing an empty entry is always safe because
           * _idle_pool_offer makes a new entry for an origin that has none
           * (see its code a few lines below), so the next offer for this
           * origin builds it again just like for a brand new origin. */
          if (cvector_elem_count(list) == 0) {
            __cvector_destroy(list);
            chmap_delete_elem(cli->idle_pools, &kp);
          }
        }
      }
    }
    _ccol_mutex_unlock(cli->lock);
    if (!got) return false;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t age_ms = _ms_elapsed_floor(&out->last_used, &now);

    /* A later chttpclient_set_tls call can replace the configuration that a
     * TLS connection did its handshake under. We refuse such a connection
     * here and the code below tears it down, the same as a connection that
     * is too old or that the peer closed. Both idle pools are keyed on the
     * origin alone, so this check is the only thing that keeps us from
     * serving a request over a connection made under the old policy, which
     * otherwise goes on for up to CHTTP_IDLE_MAX_AGE_MS after the change.
     * The change may have pinned a private CA in place of the system
     * bundle, turned verification back on or rotated the client
     * certificate. */
    bool tls_config_current =
        (!out->tls || out->tls_generation == cur_tls_generation);

    bool alive = false;
    if (age_ms <= CHTTP_IDLE_MAX_AGE_MS && tls_config_current) {
      char probe;
      ssize_t pn;
      if (out->tls) {
        /* A raw MSG_PEEK on out->fd would look at the encrypted wire bytes
         * and go around OpenSSL completely. The peer can send something at
         * the TLS record layer after we read the last response, the usual
         * case being a TLS 1.3 NewSessionTicket that OpenSSL servers send
         * right after the handshake or the response. The connection is
         * healthy, but a raw peek says "data available", the probe would
         * call it dead and throw it away, and HTTPS keep-alive reuse would
         * quietly stop working. Going through ctls_conn_read lets OpenSSL
         * swallow and process such a protocol-only record, the same way the
         * async engine's liveness check (the CHTTP_ASYNC_DISPATCH_IDLE
         * branch of _async_on_readable_impl) does it. It's a real read and
         * not a peek, but that does no harm: if it returns more than 0,
         * real application data was waiting, the connection goes away and
         * is never reused, so nobody sees the consumed byte. On EWOULDBLOCK
         * it consumes nothing and the connection is intact for reuse. */
        pn = ctls_conn_read(out->tls, &probe, 1);
      } else {
        /* Retry on EINTR like the other signal-safe syscall wrappers of
         * this file (_conn_read, _conn_write and _conn_wait). A signal that
         * interrupts this MSG_DONTWAIT peek gives errno == EINTR, which is
         * neither EAGAIN nor EWOULDBLOCK below, and without the retry we
         * would call a healthy connection dead and drop it instead of
         * reusing it. */
        do {
          pn = recv(out->fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
        } while (pn < 0 && errno == EINTR);
      }
      alive = (pn < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    }
    if (alive) return true;

    _conn_teardown(cli->m_procs, out);
    /* Go around and try the next candidate of this origin, if any. */
  }
}

/*
 * Gives a good connection back to the idle pool, which has a cap per origin
 * and a total cap. When it doesn't fit, because a cap is full or the client
 * is being destroyed, we close it instead; that only loses an optimisation
 * and is never a correctness problem.
 */
static void _idle_pool_offer(struct chttpclient *cli, chttp_conn_t *c) {
  bool pooled = false;
  _ccol_mutex_lock(cli->lock);
  if (!cli->destroying && cli->idle_pools &&
      cli->idle_total_count < CHTTP_MAX_IDLE_TOTAL) {
    cmap_pair kp = {.ptr = c->origin_key, .size = strlen(c->origin_key) + 1};
    cmap_pair vp;
    cvec list = NULL;
    if (chmap_get_elem_ref(cli->idle_pools, &kp, &vp) == ccol_success) {
      list = _read_cvec(vp.ptr);
    }
    /* A new origin gets its own entry only while the cap on distinct
     * origins has room (see the comment of CHTTP_MAX_IDLE_ORIGINS). At the
     * cap list stays NULL and we fall through to the same "it doesn't fit"
     * path that every other pooling failure below takes. */
    if (!list &&
        chmap_elem_count(cli->idle_pools) < _effective_max_idle_origins()) {
      char *cverr = NULL;
      list = cvector_create_full(sizeof(chttp_conn_t), cli->m_procs, &cverr);
      if (list) {
        cmap_pair vp2 = {.ptr = &list, .size = sizeof(list)};
        if (chmap_insert_elem(cli->idle_pools, &kp, &vp2) != ccol_success) {
          __cvector_destroy(list);
          list = NULL;
        }
      }
    }
    if (list && cvector_elem_count(list) < CHTTP_MAX_IDLE_PER_ORIGIN) {
      clock_gettime(CLOCK_MONOTONIC, &c->last_used);
      if (cvector_push_back(list, c) == ccol_success) {
        pooled = true;
        cli->idle_total_count++;
      }
    }
  }
  _ccol_mutex_unlock(cli->lock);
  if (!pooled) _conn_teardown(cli->m_procs, c);
}

/* ========================================================================== */
/*                         REQUEST SERIALIZATION                              */
/* ========================================================================== */

/*
 * Records which of the headers that _serialize_request has a default for
 * are present. _scan_header_presence below fills it from req->headers in a
 * single pass that ignores case.
 *
 * A point lookup with chmap_get_elem_ref uses a lower-cased query string,
 * the same as chttp_request_get_header, so it misses a borrowed map whose
 * keys keep their natural case ("Content-Type", "Authorization"). A caller
 * can build such a map by hand and pass it to chttp_run_query, and that map
 * never goes through the lower-casing of chttp_request_set_header. With a
 * miss chttp_run_query would put on the wire both the caller's header and a
 * duplicate that we added. Host goes wrong the other way: the
 * header-emission loop below skips every "host" match regardless of case,
 * so the caller's value would vanish without a word. Every field below
 * comes from this one case-insensitive scan, so no field can get out of
 * step with the emission loop like that.
 */
typedef struct {
  bool has_host;
  bool has_accept;
  bool has_user_agent;
  bool has_content_length;
  bool has_content_type;
  bool has_authorization;
  bool has_expect;
  bool has_transfer_encoding;
  /* With an Idempotency-Key or X-Idempotency-Key header a request of any
   * method is safe to resend on a fresh connection after a reused one
   * died; see
   * _request_is_replayable. */
  bool has_idempotency_key;
  /* The caller's Connection header names the "close" option, so the
   * connection must not carry another request after this one (RFC 9112
   * section 9.6); see _connection_value_has_close. */
  bool request_close;
} chttp_header_presence_t;

/* True when a Connection field value lists the "close" option. The value is
 * a comma-separated list of tokens, each element with optional whitespace
 * around it (RFC 9110 section 7.6.1), and tokens compare ignoring ASCII
 * case. */
static bool _connection_value_has_close(const char *v) {
  const char *p = v;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == ',') p++;
    const char *start = p;
    while (*p && *p != ',') p++;
    const char *end = p;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
    if (end - start == 5 && ccol_ascii_ncasecmp(start, "close", 5) == 0)
      return true;
  }
  return false;
}

/* We set *failed when the iterator over the header map can't be allocated.
 * The flags are unknown in that case and the caller must not build a
 * request from them. */
static chttp_header_presence_t _scan_header_presence(chmap headers,
                                                     bool *failed) {
  chttp_header_presence_t p = {0};
  *failed = false;
  if (!headers) return p;
  char *ierr = NULL;
  cmap_iterator *it = chashmap_begin_iter(headers, &ierr);
  if (!it && ierr) {
    *failed = true;
    return p;
  }
  for (; it; it = it->_next_fn(it)) {
    const char *name = (const char *)it->key_pair->ptr;
    if (ccol_ascii_casecmp(name, "host") == 0)
      p.has_host = true;
    else if (ccol_ascii_casecmp(name, "accept") == 0)
      p.has_accept = true;
    else if (ccol_ascii_casecmp(name, "user-agent") == 0)
      p.has_user_agent = true;
    else if (ccol_ascii_casecmp(name, "content-length") == 0)
      p.has_content_length = true;
    else if (ccol_ascii_casecmp(name, "content-type") == 0)
      p.has_content_type = true;
    else if (ccol_ascii_casecmp(name, "authorization") == 0)
      p.has_authorization = true;
    else if (ccol_ascii_casecmp(name, "expect") == 0)
      p.has_expect = true;
    else if (ccol_ascii_casecmp(name, "transfer-encoding") == 0)
      p.has_transfer_encoding = true;
    else if (ccol_ascii_casecmp(name, "idempotency-key") == 0 ||
             ccol_ascii_casecmp(name, "x-idempotency-key") == 0)
      p.has_idempotency_key = true;
    else if (ccol_ascii_casecmp(name, "connection") == 0 &&
             _connection_value_has_close((const char *)it->val_pair->ptr))
      p.request_close = true;
  }
  return p;
}

/* True when we may send a request again, unchanged, on a fresh connection
 * after a reused connection took every byte of it and died without an
 * answer. The server may have acted on it already, so this holds only for
 * an RFC 9110 idempotent method (section 9.2.2; of this client's methods
 * that is GET, HEAD, OPTIONS, PUT and DELETE) or for a request carrying an
 * Idempotency-Key or X-Idempotency-Key header, which lets the server spot a
 * second delivery. A request that didn't go out in full is always safe to
 * resend, and the callers check that separately. For anything else we'd
 * rather fail the call than guess, because a second delivery of a POST can
 * repeat an action the server already took. */
static bool _request_is_replayable(chttp_method_t method,
                                   const chttp_header_presence_t *hp) {
  switch (method) {
    case CHTTP_GET:
    case CHTTP_HEAD:
    case CHTTP_OPTIONS:
    case CHTTP_PUT:
    case CHTTP_DELETE:
      return true;
    default:
      return hp->has_idempotency_key;
  }
}

/*
 * Tracks which header NAMES the header-emission loop of _serialize_request
 * below has sent, ignoring case. req->headers is keyed on the exact bytes,
 * so a map that a caller builds and lends to us can hold two keys that are
 * different bytes but the SAME header name under HTTP's case-insensitive
 * rules, "Host" and "host" for example, and the map keeps them as two
 * distinct entries. The lower-casing of chttp_request_set_header prevents
 * that for the documented setter, but a map built by hand gives no such
 * guarantee: the headers parameter of chttp_run_query is one, and a caller
 * that fills chttp_request_t.headers itself makes another.
 *
 * Without this tracker both entries go out as two separate header lines.
 * For an ordinary custom header that's just unusual, and RFC 7230 SS3.2.2
 * lets a sender join repeated fields of the same name. For "Host" though
 * it makes a request that RFC 7230 SS5.4 tells a server to reject, since
 * that section lists "more than one Host header field" as a 400 Bad Request
 * case. A remote peer sees a real protocol violation, and the same
 * duplicate-Host shape is what request-smuggling and cache-poisoning
 * techniques are built on.
 *
 * `names` holds borrowed pointers into the key storage of req->headers.
 * They are valid for the whole emission loop because chmap iteration never
 * changes or moves an existing entry. We fill `names` in iteration order,
 * and our separate-chaining chmap gives the newest insertion first (see the
 * "Reverse-insertion-order iteration" documentation of chashmap.c). So the
 * loop looks for an earlier sighting before adding a name and skips the
 * name on a hit, which keeps exactly the newest occurrence of each
 * case-insensitive name and drops every older duplicate. That is also how
 * a plain update through chmap_insert_elem with an exact key behaves: the
 * last set wins.
 */
typedef struct {
  const char **names;
  size_t count;
  size_t cap;
} chttp_seen_names_t;

static bool _seen_names_contains(const chttp_seen_names_t *seen,
                                 const char *name) {
  for (size_t i = 0; i < seen->count; i++)
    if (ccol_ascii_casecmp(seen->names[i], name) == 0) return true;
  return false;
}

/* Records that the loop sent name. The only false return is a failed
 * allocation of the growable backing array; we never copy the bytes of
 * name, which is borrowed and outlives this whole call. */
static bool _seen_names_add(ccol_memmgmt_procs_t *mp, chttp_seen_names_t *seen,
                            const char *name) {
  if (seen->count == seen->cap) {
    /* The doubling below gets the same guard as every other
     * "curr_size *= 2" in the library (cvector, csort, cmempool, and
     * _ob_append and _sink_buffered in this file). Without it an
     * astronomical seen->cap can double past SIZE_MAX, and the
     * multiplication by sizeof(*nn) after it can overflow on its own too.
     * Either way nc can end up small or zero, the header-emission loop
     * above takes the too-small array as properly grown, and
     * seen->names[seen->count++] writes past its real end. Reaching this
     * takes about 2^61 distinct header names on one request, so no real
     * input gets here, but we count an unguarded overflow in a size
     * computation as a real bug however big the input that triggers it. */
    if (seen->cap > SIZE_MAX / 2) return false;
    size_t nc = seen->cap ? seen->cap * 2 : 8;
    if (nc > SIZE_MAX / sizeof(*seen->names)) return false;
    const char **nn = (const char **)_ccol_mem_realloc(mp, (void *)seen->names,
                                                       nc * sizeof(*nn));
    if (!nn) return false;
    seen->names = nn;
    seen->cap = nc;
  }
  seen->names[seen->count++] = name;
  return true;
}

#ifdef RUNNING_UNIT_TESTS
/*
 * White-box test helper that reaches the overflow guard of _seen_names_add
 * directly. It builds a chttp_seen_names_t with count and cap set to
 * fake_cap and names left NULL, so no backing array of that size is ever
 * allocated or touched, and calls _seen_names_add once. The guard must
 * reject the call before the realloc of the growth branch and before the
 * seen->names[seen->count++] write below that branch.
 *
 * It's the same kind of test we write for every overflow guard of this
 * sort (the guard must refuse before any real work happens), like
 * _chttp_ob_append_overflow_guard_for_tests and
 * _chttp_percent_encode_unix_path_overflow_guard_for_tests. The helper
 * isn't public API, and a gate keeps the symbol out of a production build
 * of libccollections.so like every other white-box helper here.
 */
bool _chttp_seen_names_add_overflow_guard_for_tests(ccol_memmgmt_procs_t *mp,
                                                    size_t fake_cap) {
  chttp_seen_names_t seen = {.names = NULL, .count = fake_cap, .cap = fake_cap};
  bool added = _seen_names_add(mp, &seen, "x");
  /* When the guard lets the call through, it's an ordinary call that grows
   * seen.names with a real realloc, and since our struct is a local that
   * goes away we free that array here instead of leaking it. Real callers
   * of _seen_names_add never need to care, the header-emission loop of
   * _serialize_request frees seen.names itself. */
  _ccol_mem_free(mp, (void *)seen.names);
  return !added;
}
#endif /* RUNNING_UNIT_TESTS */

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  ccol_memmgmt_procs_t *mp;
  bool oom;
} chttp_outbuf_t;

/* Next capacity for a growable buffer that holds `cap` bytes and needs to
 * hold `need` (need > cap). An empty buffer gets `initial`, otherwise the
 * capacity doubles until it reaches `need`. If a doubling would go past
 * SIZE_MAX we return `need` itself so the result never wraps; on ILP32 a
 * 2 GiB capacity doubles to 0, and a loop doubling 0 never ends. The caller
 * has checked beforehand that `need` didn't wrap. */
static size_t _chttp_grow_capacity(size_t cap, size_t need, size_t initial) {
  if (cap == 0) {
    cap = initial;
  } else {
    if (cap > SIZE_MAX / 2) return need;
    cap *= 2;
  }
  while (cap < need) {
    if (cap > SIZE_MAX / 2) return need;
    cap *= 2;
  }
  return cap;
}

static void _ob_append(chttp_outbuf_t *b, const char *data, size_t n) {
  if (b->oom) return;
  /* The needed size below is guarded against overflow relative to
   * SIZE_MAX, the same way the other growable buffers of the library
   * (cvector, csort and cmempool) do it. Without the guard a caller's body
   * could be big enough to wrap b->len + n + 1 past SIZE_MAX, and the
   * doubling loop below would pick an `nc` far too small, or `nc` would
   * wrap to 0 inside the loop and spin forever since 0 is always below a
   * non-zero target. The memcpy after it would then overflow the heap
   * buffer instead of us reporting a clean allocation failure. It takes a
   * request body of many exabytes in memory before this function is even
   * called, so no real input gets here, but we count an unguarded overflow
   * in a size computation as a real bug however big the input that
   * triggers it. */
  if (n > SIZE_MAX - b->len || b->len + n > SIZE_MAX - 1) {
    b->oom = true;
    return;
  }
  size_t need = b->len + n + 1;
  if (need > b->cap) {
    size_t nc = _chttp_grow_capacity(b->cap, need, 1024);
    char *nb = (char *)_ccol_mem_realloc(b->mp, b->buf, nc);
    if (!nb) {
      b->oom = true;
      return;
    }
    b->buf = nb;
    b->cap = nc;
  }
  memcpy(b->buf + b->len, data, n);
  b->len += n;
  b->buf[b->len] = '\0';
}

static void _ob_append_cstr(chttp_outbuf_t *b, const char *s) {
  _ob_append(b, s, strlen(s));
}

#ifdef RUNNING_UNIT_TESTS
/*
 * White-box test helper that reaches the overflow guard of _ob_append
 * directly. It sets up a chttp_outbuf_t whose `len` is fake_len, a length
 * nobody ever allocates. The buffer starts with buf == NULL and cap == 0,
 * so the guard must reject the append before mp's malloc or realloc is
 * touched. It appends up to 64 bytes, and with n capped at compile time a
 * small real backing buffer is enough. It returns whether _ob_append set
 * b->oom.
 *
 * The caller passes a counting allocator and checks its call count on its
 * own side, which is how we test this kind of overflow guard everywhere
 * (the allocator must never have been called), as in the default
 * comparators of csort and cmempool. Not public API; a gate keeps the
 * symbol out of a production build of libccollections.so, like every
 * other white-box helper in this file.
 */
bool _chttp_ob_append_overflow_guard_for_tests(ccol_memmgmt_procs_t *mp,
                                               size_t fake_len, size_t n) {
  chttp_outbuf_t ob = {.mp = mp, .len = fake_len};
  char probe[64] = {0};
  if (n > sizeof(probe)) n = sizeof(probe);
  _ob_append(&ob, probe, n);
  bool oom = ob.oom;
  _ccol_mem_free(mp, ob.buf);
  return oom;
}

/*
 * White-box test helper for _ob_append growing from a capacity whose
 * doubling would pass SIZE_MAX. The chttp_outbuf_t claims fake_cap bytes of
 * capacity and fake_cap - 1 bytes of content with buf left NULL, and we
 * append 8 bytes so it has to grow. The caller gives an allocator whose
 * realloc fails, and we return whether _ob_append set b->oom. If the growth
 * wrapped the capacity, this would never return.
 */
bool _chttp_ob_append_cap_overflow_for_tests(ccol_memmgmt_procs_t *mp,
                                             size_t fake_cap) {
  chttp_outbuf_t ob = {.mp = mp, .len = fake_cap - 1, .cap = fake_cap};
  char probe[8] = {0};
  _ob_append(&ob, probe, sizeof(probe));
  return ob.oom;
}
#endif /* RUNNING_UNIT_TESTS */

/*
 * Headers set with chttp_request_set_header have no origin of their own,
 * but a redirect can send the request to a server the caller never named.
 * These two flags say which of those headers we leave off a hop because of
 * that. Every tier recomputes them per hop against the origin of the FIRST
 * request, never against the previous hop (curl's rule), so a hop that
 * comes back to the first origin sends the headers again and a hop that
 * stays on a foreign origin keeps leaving them off.
 *
 * CHTTP_REDIRECT_DROP_CREDENTIALS: the hop's scheme, host or port differs
 * from the first request, and the caller's Authorization, Cookie, Cookie2
 * and WWW-Authenticate headers are not sent. curl does the same in this
 * case (CVE-2018-1000007 and CVE-2022-27776).
 *
 * CHTTP_REDIRECT_DROP_HOST: the hop's host differs from the first request,
 * so the caller's Host header is not sent and we write our own Host line
 * for the new target, since a Host naming the first server is wrong for
 * another one. curl has the same rule.
 */
#define CHTTP_REDIRECT_DROP_CREDENTIALS 0x1u
#define CHTTP_REDIRECT_DROP_HOST 0x2u

/* Host part of an origin_key: for a TCP key the text between "://" and the
 * port, for a "unix://" key the whole socket path. */
static void _origin_key_host_span(const char *key, const char **start,
                                  size_t *len) {
  const char *sep = strstr(key, "://");
  const char *h = sep ? sep + 3 : key;
  if (strncmp(key, "unix://", 7) == 0) {
    *start = h;
    *len = strlen(h);
    return;
  }
  const char *colon = strrchr(h, ':');
  *start = h;
  *len = colon ? (size_t)(colon - h) : strlen(h);
}

/* True when two origin_keys are the same origin as far as carrying
 * credentials across a redirect goes. A TCP origin's host compares
 * ignoring case, like DNS names and curl. The scheme is always lower-case
 * "http" or "https" and the port is decimal digits, so a single
 * case-insensitive comparison of the whole key compares those two
 * exactly. A "unix://" key compares byte for byte, a socket path is a file
 * name and its case matters. */
static bool _origin_keys_same(const char *a, const char *b) {
  bool a_unix = strncmp(a, "unix://", 7) == 0;
  bool b_unix = strncmp(b, "unix://", 7) == 0;
  if (a_unix || b_unix) return a_unix == b_unix && strcmp(a, b) == 0;
  return ccol_ascii_casecmp(a, b) == 0;
}

/* The CHTTP_REDIRECT_DROP_* flags for a hop to `origin_key`, judged
 * against `initial_origin_key`, which is the origin of the first
 * request. */
static unsigned _redirect_header_drops(const char *initial_origin_key,
                                       const char *origin_key) {
  if (!initial_origin_key || _origin_keys_same(initial_origin_key, origin_key))
    return 0;
  unsigned drops = CHTTP_REDIRECT_DROP_CREDENTIALS;
  const char *a, *b;
  size_t alen, blen;
  _origin_key_host_span(initial_origin_key, &a, &alen);
  _origin_key_host_span(origin_key, &b, &blen);
  bool a_unix = strncmp(initial_origin_key, "unix://", 7) == 0;
  bool b_unix = strncmp(origin_key, "unix://", 7) == 0;
  bool same_host;
  if (a_unix != b_unix || alen != blen) {
    same_host = false;
  } else if (a_unix) {
    same_host = memcmp(a, b, alen) == 0;
  } else {
    same_host = ccol_ascii_ncasecmp(a, b, alen) == 0;
  }
  if (!same_host) drops |= CHTTP_REDIRECT_DROP_HOST;
  return drops;
}

/* True for the header names that CHTTP_REDIRECT_DROP_CREDENTIALS
 * takes off. */
static bool _is_redirect_credential_header(const char *name) {
  return ccol_ascii_casecmp(name, "authorization") == 0 ||
         ccol_ascii_casecmp(name, "cookie") == 0 ||
         ccol_ascii_casecmp(name, "cookie2") == 0 ||
         ccol_ascii_casecmp(name, "www-authenticate") == 0;
}

/*
 * Serializes the method line, the headers and the body into one wire
 * buffer. Changing the method and the body on a redirect hop is up to the
 * caller, which does it through the method and body fields of `req`, since
 * `req` is a shallow per-hop view and not the caller's original request
 * object.
 *
 * out_presence may be NULL. Otherwise it gets the same
 * chttp_header_presence_t that we compute from req->headers anyway to
 * decide which headers to add and which duplicates to drop, so a caller
 * that needs one of those flags too doesn't pay for a second O(header
 * count) pass over the same map through its own _scan_header_presence
 * call. The Tier 1 hop loop wants has_expect from it, to know whether this
 * hop goes through the "Expect: 100-continue" wait.
 */
static ccol_retval_t _serialize_request(ccol_memmgmt_procs_t *mp,
                                        const chttp_request_t *req,
                                        const chttp_url_t *url,
                                        const char *auto_authorization,
                                        unsigned redirect_drops, char **out_buf,
                                        size_t *out_len,
                                        chttp_header_presence_t *out_presence) {
  /* Backstop for a method the caller wrote into the request after
   * chttp_request_create (which checks it as well). */
  if ((unsigned)req->method >= (unsigned)CHTTP_ANY) return ccol_invalid_args;
  chttp_outbuf_t ob = {.mp = mp};

  _ob_append_cstr(&ob, chttp_method_str(req->method));
  _ob_append(&ob, " ", 1);
  _ob_append_cstr(&ob, url->path_and_query);
  _ob_append_cstr(&ob, " HTTP/1.1\r\n");

  bool scan_failed;
  chttp_header_presence_t hp =
      _scan_header_presence((chmap)req->headers, &scan_failed);
  if (scan_failed) {
    _ccol_mem_free(mp, ob.buf);
    return ccol_not_enough_memory;
  }
  if (hp.has_transfer_encoding) {
    /* chttp_request_set_header refuses this too (see its doc comment), but
     * the real backstop is here. req->headers is an internal chmap handle
     * that a caller can build and assign by hand without ever going through
     * chttp_request_set_header, as with the borrowed map of chttp_run_query
     * or a caller that fills chttp_request_t.headers itself. If we let it
     * pass, the Content-Length code further down would pair it with a
     * conflicting Content-Length over a body that has no chunk encoding. */
    _ccol_mem_free(mp, ob.buf);
    return ccol_invalid_args;
  }
  /* When CHTTP_REDIRECT_DROP_HOST takes off the caller's Host header we
   * count it as absent and write our own Host line for this hop. */
  bool has_host = hp.has_host && !(redirect_drops & CHTTP_REDIRECT_DROP_HOST);
  bool has_accept = hp.has_accept;
  bool has_ua = hp.has_user_agent;
  bool has_cl = hp.has_content_length;
  bool has_ct = hp.has_content_type;
  /* Once a redirect crosses to an origin other than the one the caller set
   * "authorization" for, CHTTP_REDIRECT_DROP_CREDENTIALS takes that header
   * off. For the auto_authorization decision below we then treat it as if
   * it was never there, and the header loop further down skips writing it
   * from req->headers on its own.
   *
   * Without this, a caller with an explicit Authorization for the original
   * origin and a redirect target URL that has its own "user:pass@" userinfo
   * would get a hop with NO Authorization header at all, while the right
   * one is built from the new origin's userinfo. */
  bool has_auth = hp.has_authorization &&
                  !(redirect_drops & CHTTP_REDIRECT_DROP_CREDENTIALS);

  if (!has_host) {
    if (url->is_unix) {
      /* A unix-domain-socket target has no real hostname or port to send.
       * "localhost" is what curl's unix-socket option sends by default and
       * what a server on the other end can expect. RFC 7230 SS5.4 wants a
       * Host header on every HTTP/1.1 request even though the value means
       * nothing here. */
      _ob_append_cstr(&ob, "host: localhost\r\n");
    } else {
      bool default_port = (url->is_https && url->port == 443) ||
                          (!url->is_https && url->port == 80);
      _ob_append_cstr(&ob, "host: ");
      if (url->is_ipv6) _ob_append(&ob, "[", 1);
      _ob_append_cstr(&ob, url->host);
      if (url->is_ipv6) _ob_append(&ob, "]", 1);
      if (!default_port) {
        char portbuf[16];
        int pn = snprintf(portbuf, sizeof(portbuf), ":%u", (unsigned)url->port);
        if (pn > 0) _ob_append(&ob, portbuf, (size_t)pn);
      }
      _ob_append(&ob, "\r\n", 2);
    }
  }
  if (!has_accept) _ob_append_cstr(&ob, "accept: */*\r\n");
  if (!has_ua)
    _ob_append_cstr(&ob, "user-agent: c_collections-chttpclient/1.0\r\n");
  if (!has_auth && auto_authorization) {
    _ob_append_cstr(&ob, "authorization: ");
    _ob_append_cstr(&ob, auto_authorization);
    _ob_append(&ob, "\r\n", 2);
  }

  bool body_carrying_method =
      (req->method == CHTTP_POST || req->method == CHTTP_PUT ||
       req->method == CHTTP_PATCH);

  if (req->headers) {
    /* We skip a "host" entry in this loop only under
     * CHTTP_REDIRECT_DROP_HOST, which also clears has_host so the block
     * above writes our own Host line. That block runs only when !has_host,
     * so outside of a drop a "host" entry gets here only when the caller
     * gave one, has_host is true and the block above didn't run. Skipping
     * unconditionally would drop the caller's Host (from
     * chttp_request_set_header or a borrowed map) and the wire would carry
     * no built line, which is right, but no caller line either, which is
     * wrong. That breaks RFC 7230 SS5.4 (cited by the unix-socket comment in
     * this file), which says every HTTP/1.1 request MUST carry a Host
     * header. Letting it fall through like any other header is what the
     * has_host gate is there for. */
    chttp_seen_names_t seen = {0};
    char *ierr = NULL;
    cmap_iterator *it = chashmap_begin_iter((chmap)req->headers, &ierr);
    if (!it && ierr) {
      /* We can't write the headers that the presence flags promise, and a
       * request without them (a body with no Content-Length, say) would be
       * malformed on the wire. */
      _ccol_mem_free(mp, ob.buf);
      return ccol_not_enough_memory;
    }
    for (; it; it = it->_next_fn(it)) {
      const char *name = (const char *)it->key_pair->ptr;
      const char *val = (const char *)it->val_pair->ptr;
      /* chttp_request_set_header does the same checks (see its doc
       * comment), but this is the real backstop: any caller can build
       * req->headers, an internal chmap handle, and assign it without going
       * through chttp_request_set_header at all, as with the borrowed map
       * of chttp_run_query or a caller filling chttp_request_t.headers
       * itself. Whichever path a header took, this loop is the one place
       * that writes it to the wire. */
      if (!*name || !_header_value_is_valid(val)) {
        ccol_iter_destroy(it);
        _ccol_mem_free(mp, (void *)seen.names);
        _ccol_mem_free(mp, ob.buf);
        return ccol_invalid_args;
      }
      for (const char *p = name; *p; p++) {
        if (!chttp1_is_tchar((unsigned char)*p)) {
          ccol_iter_destroy(it);
          _ccol_mem_free(mp, (void *)seen.names);
          _ccol_mem_free(mp, ob.buf);
          return ccol_invalid_args;
        }
      }
      /* When the method of THIS hop carries no body, req->body never goes
       * on the wire (see the body-append check further down), whatever
       * req->headers holds. One way to get here is a caller who set one of
       * these three headers on a request that never meant to have a body.
       * The more important one is a 301, 302 or 303 redirect that turns a
       * POST, PUT or PATCH into a GET: chttp_do_internal and
       * _async_submit_hop rewrite cur_method and cur_body for the new hop
       * but send req->headers and chain->req_headers again exactly as they
       * were. A Content-Length, Content-Type or Expect describing the
       * ORIGINAL body would then end up on a hop that sends no body. A
       * stale Content-Length does real harm, a server that trusts the
       * declared length up front blocks reading a body that never comes
       * until its own read timeout fires, and our chttpserver.c is such a
       * server, through chttp1_declared_content_length. */
      if (!body_carrying_method &&
          (ccol_ascii_casecmp(name, "content-length") == 0 ||
           ccol_ascii_casecmp(name, "content-type") == 0 ||
           ccol_ascii_casecmp(name, "expect") == 0))
        continue;
      /* See CHTTP_REDIRECT_DROP_CREDENTIALS and CHTTP_REDIRECT_DROP_HOST.
       * Once a redirect chain reaches a server the caller never named, the
       * credentials and Host header the caller meant for the first server
       * stay off the wire. */
      if ((redirect_drops & CHTTP_REDIRECT_DROP_CREDENTIALS) &&
          _is_redirect_credential_header(name))
        continue;
      if ((redirect_drops & CHTTP_REDIRECT_DROP_HOST) &&
          ccol_ascii_casecmp(name, "host") == 0)
        continue;
      /* See the comment of chttp_seen_names_t. A borrowed map may hold two
       * keys for one header name that differ only in case, and only the
       * first one we meet goes out, which by chmap's iteration order is the
       * newest insertion. */
      if (_seen_names_contains(&seen, name)) continue;
      /* A caller's Content-Length must match the body bytes we append
       * below (see the body-append check further down). Otherwise the
       * framing this hop declares to the server disagrees with what goes on
       * the wire, and on a connection this client also pools for reuse.
       * The Transfer-Encoding rejection above is there for the same hazard
       * (see the doc comment of chttp_request_set_header), only with a
       * wrong transfer-coding instead of a wrong length. We check it for a
       * body_carrying_method only, because for any other method the code a
       * few lines above drops this very header, so a wrong value has no
       * framing to break. */
      if (body_carrying_method &&
          ccol_ascii_casecmp(name, "content-length") == 0) {
        size_t expected = req->body.data ? req->body.len : (size_t)0;
        /* An accepted val goes on the wire byte for byte, so its grammar
         * must be checked as strictly as our own request-side parser checks
         * an incoming Content-Length (parse_uint64_decimal in
         * chttp1_parser.c, used by chttpserver.c): a bare "1*DIGIT" with no
         * sign and no whitespace. strtoull alone isn't enough, since
         * unlike parse_uint64_decimal it takes a leading '+' or '-' and
         * leading whitespace before the digits. "+42" or " 42" can equal the
         * real body length as a number and would pass, because
         * strtoull("+42", &endp, 10) returns 42 with *endp == '\0', and we
         * would write it on the wire as is. Our own chttpserver then
         * rejects that request as "Invalid Content-Length" although
         * chttp_request_set_header and _serialize_request took it as
         * valid. */
        size_t val_len = strlen(val);
        bool all_digits = val_len > 0;
        for (size_t vi = 0; vi < val_len; vi++) {
          if (!isdigit((unsigned char)val[vi])) {
            all_digits = false;
            break;
          }
        }
        char *endp = NULL;
        errno = 0;
        unsigned long long declared = all_digits ? strtoull(val, &endp, 10) : 0;
        if (!all_digits || *endp != '\0' || errno == ERANGE ||
            (unsigned long long)expected != declared) {
          ccol_iter_destroy(it);
          _ccol_mem_free(mp, (void *)seen.names);
          _ccol_mem_free(mp, ob.buf);
          return ccol_invalid_args;
        }
      }
      if (!_seen_names_add(mp, &seen, name)) {
        ccol_iter_destroy(it);
        _ccol_mem_free(mp, (void *)seen.names);
        _ccol_mem_free(mp, ob.buf);
        return ccol_not_enough_memory;
      }
      _ob_append_cstr(&ob, name);
      _ob_append(&ob, ": ", 2);
      _ob_append_cstr(&ob, val);
      _ob_append(&ob, "\r\n", 2);
    }
    _ccol_mem_free(mp, (void *)seen.names);
  }

  if (!has_ct && body_carrying_method && req->body.content_type) {
    /* This gate looks at body_carrying_method alone, without testing
     * req->body.data or req->body.len, the same as the Content-Length code a
     * few lines below, which runs for any body_carrying_method with !has_cl
     * whether body.data is set or not. A method without a body (GET,
     * DELETE, HEAD and OPTIONS) never puts req->body on the wire (see the
     * body-append check further down), so a content-type header there
     * makes no sense. A caller can attach a body to a DELETE, like the
     * delete_body_not_transmitted test in this file does, and would get a
     * "content-type: ..." header with no body bytes and no Content-Length
     * behind it; checking body_carrying_method alone rules that out.
     *
     * An empty body on a method that has one is a different, legitimate
     * case: body.data is NULL because body.len is 0, which is what
     * CHTTP_JSON_BODY("", 0) gives. The code below writes
     * "content-length: 0" anyway, so a "content-type: ..." describing that
     * empty representation is correct HTTP, and that case must not get the
     * extra body.data or body.len gate.
     *
     * We refuse the same control bytes as the header loop above, CR and LF
     * among them. content_type has no setter that could check it, it comes
     * in a chttp_request_body_t that chttp_request_create_mp copies as it
     * is, so this is the only place it gets checked before the wire. */
    if (!_header_value_is_valid(req->body.content_type)) {
      _ccol_mem_free(mp, ob.buf);
      return ccol_invalid_args;
    }
    _ob_append_cstr(&ob, "content-type: ");
    _ob_append_cstr(&ob, req->body.content_type);
    _ob_append(&ob, "\r\n", 2);
  }

  if (body_carrying_method && !has_cl) {
    char clbuf[48];
    int cln = snprintf(clbuf, sizeof(clbuf), "content-length: %zu\r\n",
                       req->body.data ? req->body.len : (size_t)0);
    if (cln > 0) _ob_append(&ob, clbuf, (size_t)cln);
  }

  bool has_expect = hp.has_expect;
  if (!has_expect && req->expect_continue && body_carrying_method &&
      req->body.data && req->body.len > 0) {
    _ob_append_cstr(&ob, "expect: 100-continue\r\n");
  }

  _ob_append(&ob, "\r\n", 2);

  if (body_carrying_method && req->body.data && req->body.len > 0)
    _ob_append(&ob, (const char *)req->body.data, req->body.len);

  if (ob.oom) {
    _ccol_mem_free(mp, ob.buf);
    return ccol_not_enough_memory;
  }
  *out_buf = ob.buf;
  *out_len = ob.len;
  if (out_presence) *out_presence = hp;
  return ccol_success;
}

/* ========================================================================== */
/*                         CHTTP1_PARSER INTEGRATION                          */
/* ========================================================================== */

static size_t _sink_discard(const void *data, size_t len, void *ctx) {
  (void)data;
  (void)ctx;
  return len;
}

static size_t _sink_buffered(const void *data, size_t len, void *ctx) {
  chttp_bodybuf_t *bb = (chttp_bodybuf_t *)ctx;
  if (len == 0) return 0;
  /* Overflow guard for the size computations below, like the one _ob_append
   * has for the request serialization buffer. bb->len is a running total
   * over many _on_body callbacks, each one bounded to about 8KB by a socket
   * read, so it's the CUMULATIVE total that would get bb->len + len near
   * SIZE_MAX and not one huge value from a caller, unlike the single
   * strlen()-sized allocations elsewhere in this file. No real input gets
   * here either: the allocator behind _ccol_mem_realloc below fails long
   * before bb->buf grows that big, and the `if (!nb)` check further down
   * handles that. Even so, we count an unguarded overflow in a size
   * computation as a real bug, however big the input that triggers it.
   * With a wrapped bb->len + len, the max_size comparison below would take
   * an oversized response as under the cap, and the growth loop would pick
   * an nc far too small, or nc would wrap to 0 and the loop would spin
   * forever. The memcpy below would then overflow the heap buffer instead
   * of us reporting a clean allocation failure. */
  if (len > SIZE_MAX - bb->len || bb->len + len > SIZE_MAX - 1) {
    bb->oom = true;
    return 0;
  }
  /* The cap is applied here, as the body arrives, whatever the framing is
   * (Content-Length, chunked or EOF-delimited). The check in
   * _on_headers_complete runs up front against the declared Content-Length
   * and only catches a Content-Length that honestly admits the body is too
   * large, before we read one body byte. A short return here makes _on_body
   * stop the parse with CHTTP1_USER, like the out-of-memory case just
   * below. Without it bb->buf would grow without any bound and nothing
   * would report it. */
  if (bb->max_size > 0 && bb->len + len > bb->max_size) {
    bb->too_large = true;
    return 0;
  }
  if (bb->len + len + 1 > bb->cap) {
    size_t nc = _chttp_grow_capacity(bb->cap, bb->len + len + 1, 4096);
    char *nb = (char *)_ccol_mem_realloc(bb->mp, bb->buf, nc);
    if (!nb) {
      bb->oom = true;
      return 0;
    }
    bb->buf = nb;
    bb->cap = nc;
  }
  memcpy(bb->buf + bb->len, data, len);
  bb->len += len;
  bb->buf[bb->len] = '\0';
  return len;
}

#ifdef RUNNING_UNIT_TESTS
/*
 * White-box test helper that reaches the overflow guard of _sink_buffered
 * directly. The chttp_bodybuf_t has its accumulated `len` set to fake_len
 * and bb.buf left NULL, so no buffer of that size is ever allocated, and we
 * feed it a small real probe chunk. The guard must refuse before touching
 * bb->mp's malloc or realloc and before touching bb->buf. Same idea as
 * _chttp_ob_append_overflow_guard_for_tests, which tests this guard on the
 * other growable buffer. It isn't public API; a gate keeps the symbol out
 * of a production build of libccollections.so, like every other white-box
 * helper in this file.
 */
bool _chttp_sink_buffered_overflow_guard_for_tests(ccol_memmgmt_procs_t *mp,
                                                   size_t fake_len) {
  chttp_bodybuf_t bb = {0};
  bb.mp = mp;
  bb.len = fake_len;
  char probe[8] = {0};
  size_t n = _sink_buffered(probe, sizeof(probe), &bb);
  bool rejected = (n != sizeof(probe)) && bb.oom && !bb.too_large;
  _ccol_mem_free(mp, bb.buf);
  return rejected;
}

/*
 * Same as _chttp_ob_append_cap_overflow_for_tests but for _sink_buffered.
 * A body buffer claiming fake_cap bytes of capacity and fake_cap - 1 bytes
 * of body, with buf left NULL, gets 8 more bytes. The caller's allocator
 * fails every realloc, and we return whether the sink refused the bytes as
 * an allocation failure.
 */
bool _chttp_sink_buffered_cap_overflow_for_tests(ccol_memmgmt_procs_t *mp,
                                                 size_t fake_cap) {
  chttp_bodybuf_t bb = {0};
  bb.mp = mp;
  bb.len = fake_cap - 1;
  bb.cap = fake_cap;
  char probe[8] = {0};
  size_t n = _sink_buffered(probe, sizeof(probe), &bb);
  return (n == 0) && bb.oom && !bb.too_large;
}
#endif /* RUNNING_UNIT_TESTS */

#ifdef RUNNING_UNIT_TESTS
/* How many record headers the code below has read. A test keeps it at or
 * under the number of records, so no operation can rescan the block
 * without the test noticing. */
static _Atomic unsigned long g_field_rec_reads_for_tests = 0;
unsigned long _chttpclient_field_rec_reads_for_tests(void) {
  return atomic_load_explicit(&g_field_rec_reads_for_tests,
                              memory_order_relaxed);
}
#endif

static inline chttp_field_rec_t _field_rec_at(const chttp_field_lines_t *lines,
                                              uint32_t off) {
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&g_field_rec_reads_for_tests, 1,
                            memory_order_relaxed);
#endif
  chttp_field_rec_t r;
  memcpy(&r, lines->data + off, sizeof(r));
  return r;
}

static inline const char *_field_rec_name(const chttp_field_lines_t *lines,
                                          uint32_t off) {
  return lines->data + off + sizeof(chttp_field_rec_t);
}

static inline const char *_field_rec_value(const chttp_field_lines_t *lines,
                                           uint32_t off,
                                           const chttp_field_rec_t *r) {
  return _field_rec_name(lines, off) + r->name_len + 1;
}

static inline size_t _field_rec_size(size_t name_len, size_t value_len) {
  size_t n = sizeof(chttp_field_rec_t) + name_len + 1 + value_len + 1;
  return (n + 3) & ~(size_t)3;
}

static void _field_lines_free(ccol_memmgmt_procs_t *mp,
                              chttp_field_lines_t *lines) {
  if (!lines) return;
  _ccol_mem_free(mp, lines->index);
  _ccol_mem_free(mp, lines);
}

/* FNV-1a over the lower-case name, then a multiply whose high bits choose
 * the slot, so every byte of the name has a say in the bits the table
 * reads. */
static inline size_t _field_name_slot(const char *name, size_t name_len,
                                      size_t index_cap) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < name_len; i++) {
    h ^= (unsigned char)name[i];
    h *= 1099511628211ULL;
  }
  h *= 0x9E3779B97F4A7C15ULL;
  return (size_t)(h >> 32) & (index_cap - 1);
}

/* Slot of the lower-case name, or NULL if the name has no records. */
static chttp_field_slot_t *_field_lines_find(const chttp_field_lines_t *lines,
                                             const char *name,
                                             size_t name_len) {
  if (!lines || !lines->index) return NULL;
  size_t mask = lines->index_cap - 1;
  for (size_t i = _field_name_slot(name, name_len, lines->index_cap);;
       i = (i + 1) & mask) {
    chttp_field_slot_t *slot = &lines->index[i];
    if (!slot->head) return NULL;
    uint32_t off = slot->head - 1;
    chttp_field_rec_t r = _field_rec_at(lines, off);
    if (r.name_len == name_len &&
        memcmp(_field_rec_name(lines, off), name, name_len) == 0)
      return slot;
  }
}

/* Makes room for `need` more bytes of records and one more name in the
 * index, allocating or growing through mp. On a failed allocation it
 * returns false with every record and every slot left as they were. */
static bool _field_lines_reserve(ccol_memmgmt_procs_t *mp,
                                 chttp_field_lines_t **linesp, size_t need) {
  chttp_field_lines_t *lines = *linesp;
  size_t len = lines ? lines->len : 0;
  size_t cap = lines ? lines->cap : 0;
  if (cap - len < need) {
    /* The parser's header caps keep every length here far away from an
     * overflow, since the whole block is at most two copies of the header
     * section plus a small header per record. */
    size_t new_cap = cap ? cap * 2 : 256;
    while (new_cap - len < need) new_cap *= 2;
    chttp_field_lines_t *grown = (chttp_field_lines_t *)_ccol_mem_realloc(
        mp, lines, sizeof(chttp_field_lines_t) + new_cap);
    if (!grown) return false;
    if (!lines) {
      grown->len = 0;
      grown->index = NULL;
      grown->index_cap = 0;
      grown->names = 0;
    }
    grown->cap = new_cap;
    lines = grown;
    *linesp = lines;
  }
  /* Keep the index at most half full so a probe ends quickly. */
  if ((lines->names + 1) * 2 > lines->index_cap) {
    size_t new_icap = lines->index_cap ? lines->index_cap * 2 : 16;
    chttp_field_slot_t *idx = (chttp_field_slot_t *)_ccol_mem_calloc(
        mp, new_icap, sizeof(chttp_field_slot_t));
    if (!idx) return false;
    for (size_t i = 0; i < lines->index_cap; i++) {
      chttp_field_slot_t old = lines->index[i];
      if (!old.head) continue;
      chttp_field_rec_t r = _field_rec_at(lines, old.head - 1);
      size_t j = _field_name_slot(_field_rec_name(lines, old.head - 1),
                                  r.name_len, new_icap);
      while (idx[j].head) j = (j + 1) & (new_icap - 1);
      idx[j] = old;
    }
    _ccol_mem_free(mp, lines->index);
    lines->index = idx;
    lines->index_cap = new_icap;
  }
  return true;
}

/* Appends one record at the end of the block and returns its offset. The
 * room was reserved by the caller. */
static uint32_t _field_lines_put(chttp_field_lines_t *lines, const char *name,
                                 size_t name_len, const char *value,
                                 size_t value_len) {
  uint32_t off = (uint32_t)lines->len;
  chttp_field_rec_t r = {.name_len = (uint32_t)name_len,
                         .value_len = (uint32_t)value_len,
                         .next = 0};
  char *dst = lines->data + off;
  memcpy(dst, &r, sizeof(r));
  dst += sizeof(r);
  memcpy(dst, name, name_len);
  dst[name_len] = '\0';
  memcpy(dst + name_len + 1, value, value_len);
  dst[name_len + 1 + value_len] = '\0';
  lines->len += _field_rec_size(name_len, value_len);
  return off;
}

/* Records one more occurrence, `value`, of the lower-case name. If the name
 * has no records yet, first_value is its first occurrence (the one the
 * header map holds) and it gets recorded first. A failed allocation returns
 * false and leaves *linesp untouched. */
/* Repeated names are rare, so this is kept out of line and _on_header keeps
 * its short path. */
__attribute__((noinline, cold)) static bool _field_lines_add(
    ccol_memmgmt_procs_t *mp, chttp_field_lines_t **linesp, const char *name,
    size_t name_len, const char *first_value, size_t first_len,
    const char *value, size_t value_len) {
  chttp_field_slot_t *slot = _field_lines_find(*linesp, name, name_len);
  size_t need = _field_rec_size(name_len, value_len);
  if (!slot) need += _field_rec_size(name_len, first_len);
  if (!_field_lines_reserve(mp, linesp, need)) return false;
  chttp_field_lines_t *lines = *linesp;
  if (!slot) {
    uint32_t head =
        _field_lines_put(lines, name, name_len, first_value, first_len);
    size_t j = _field_name_slot(name, name_len, lines->index_cap);
    while (lines->index[j].head) j = (j + 1) & (lines->index_cap - 1);
    slot = &lines->index[j];
    slot->head = head + 1;
    slot->tail = head + 1;
    lines->names++;
  } else {
    /* The reserve above may have moved the block and rebuilt the index, so
     * look the slot up once more. */
    slot = _field_lines_find(lines, name, name_len);
  }
  uint32_t off = _field_lines_put(lines, name, name_len, value, value_len);
  uint32_t tail = slot->tail - 1;
  chttp_field_rec_t tr = _field_rec_at(lines, tail);
  tr.next = off + 1;
  memcpy(lines->data + tail, &tr, sizeof(tr));
  slot->tail = off + 1;
  return true;
}

/* For every repeated name, replaces the header map value with the combined
 * value of RFC 9110 section 5.3, all occurrences in wire order joined with
 * ", ". Set-Cookie keeps its first occurrence because RFC 9110 section 5.3
 * and RFC 6265 section 3 forbid combining it. We do this once after the
 * whole header block, so each value is built a single time no matter how
 * many repeats there are. Returns false when an allocation fails. */
static bool _field_lines_combine(chttp_parse_ctx_t *ctx) {
  const chttp_field_lines_t *lines = ctx->field_lines;
  for (size_t i = 0; i < lines->index_cap; i++) {
    const chttp_field_slot_t *slot = &lines->index[i];
    if (!slot->head) continue;
    uint32_t off = slot->head - 1;
    chttp_field_rec_t r = _field_rec_at(lines, off);
    const char *name = _field_rec_name(lines, off);
    if (r.name_len == sizeof("set-cookie") - 1 &&
        memcmp(name, "set-cookie", r.name_len) == 0)
      continue;
    size_t total = 0;
    for (uint32_t o = slot->head; o;) {
      chttp_field_rec_t cr = _field_rec_at(lines, o - 1);
      total += cr.value_len + 2;
      o = cr.next;
    }
    char *combined = (char *)_ccol_mem_alloc(ctx->mp, total - 1);
    if (!combined) return false;
    size_t pos = 0;
    for (uint32_t o = slot->head; o;) {
      chttp_field_rec_t cr = _field_rec_at(lines, o - 1);
      if (o != slot->head) {
        combined[pos++] = ',';
        combined[pos++] = ' ';
      }
      memcpy(combined + pos, _field_rec_value(lines, o - 1, &cr), cr.value_len);
      pos += cr.value_len;
      o = cr.next;
    }
    combined[pos] = '\0';
    cmap_pair kp = {.ptr = (void *)name, .size = r.name_len + 1};
    cmap_pair vp = {.ptr = combined, .size = pos + 1};
    ccol_retval_t rv = chmap_insert_elem(ctx->headers, &kp, &vp);
    _ccol_mem_free(ctx->mp, combined);
    if (rv != ccol_key_already_present && rv != ccol_success) return false;
  }
  return true;
}

/* chttp1_parser hands this callback a whole header line, name already split
 * from value and the optional whitespace trimmed, so we keep no accumulator
 * state here. It fires for the response's header block only. The trailer
 * fields of a chunked body go to settings->on_trailer, which
 * _init_chttp1_settings leaves NULL on purpose, so nothing we put into
 * ctx->headers or ctx->field_lines can come from after the body and a
 * trailer can never change what a response header reads as.
 *
 * The first occurrence of a name goes into the header map with one probe.
 * Later occurrences leave the map alone and go into ctx->field_lines (with
 * the first occurrence too, the first time the name repeats), and once the
 * header block is complete _field_lines_combine builds the combined map
 * value. Until that point the map holds the first occurrence of each name,
 * and that's what the redirect logic reads for Location.
 *
 * name and value point into the parser's internal line buffer and are only
 * valid during this call, so BOTH get a local NUL-terminated copy before we
 * use them. name because we have to lower-case it, and value because our
 * cmap_pair convention for a string value is "size = strlen + 1" and
 * chmap_insert_elem copies exactly that many bytes. Reading value_len + 1
 * raw bytes from the parser's line buffer would read one byte past the
 * value, and nothing says that byte is '\0'. Both copies share one buffer.
 * A line that fits the parser's fixed buffer of CHTTP1_MAX_LINE_LEN bytes
 * always fits the stack buffer below, since name, colon and value are all
 * part of that line; a longer line, which the parser keeps on the heap,
 * gets a heap copy here as well. */
static int _on_header(chttp1_parser_t *p, const char *name, size_t name_len,
                      const char *value, size_t value_len) {
  chttp_parse_ctx_t *ctx = (chttp_parse_ctx_t *)p->data;

  char stack_buf[CHTTP1_MAX_LINE_LEN + 1];
  size_t need = name_len + 1 + value_len + 1;
  char *buf = stack_buf;
  if (need > sizeof(stack_buf)) {
    buf = (char *)_ccol_mem_alloc(ctx->mp, need);
    if (!buf) {
      ctx->error = true;
      return 1;
    }
  }
  char *lower_name = buf;
  for (size_t i = 0; i < name_len; i++)
    lower_name[i] = (char)ccol_ascii_tolower((unsigned char)name[i]);
  lower_name[name_len] = '\0';

  char *value_copy = buf + name_len + 1;
  memcpy(value_copy, value, value_len);
  value_copy[value_len] = '\0';

  int ret = 0;
  cmap_pair kp = {.ptr = lower_name, .size = name_len + 1};
  cmap_pair vp = {.ptr = value_copy, .size = value_len + 1};
  cmap_pair slot;
  ccol_retval_t rv =
      ccol_chmap_insert_or_get_elem(ctx->headers, &kp, &vp, &slot);
  if (rv != ccol_success &&
      (rv != ccol_key_already_present ||
       !_field_lines_add(ctx->mp, &ctx->field_lines, lower_name, name_len,
                         (const char *)slot.ptr, slot.size - 1, value_copy,
                         value_len))) {
    ctx->error = true;
    ret = 1;
  }
  if (buf != stack_buf) _ccol_mem_free(ctx->mp, buf);
  return ret;
}

static int _on_headers_complete(chttp1_parser_t *p) {
  chttp_parse_ctx_t *ctx = (chttp_parse_ctx_t *)p->data;
  ctx->status_code = p->status_code;

  /* "101 Switching Protocols" is the answer to an Upgrade request (RFC 9110
   * SS15.2.2) and this client never sends Upgrade. The bytes after a 101
   * belong to some other protocol, so any HTTP reading of them is wrong,
   * be it the discard every other 1xx gets or a final response. We fail
   * the request right here like curl does, before the interim-response
   * handling of any tier sees the message. ctx->error and ctx->too_large
   * stay false, and every caller of the parser maps this stop to
   * ccol_http_transfer_aborted. */
  if (p->status_code == 101) return -1;

  /* The only coding the parser decodes is chunked. A final response whose
   * Transfer-Encoding names some other coding (gzip, deflate, identity,
   * ...), which this client never asks for, would reach the caller as a
   * success with an encoded body. So it fails here, the way Go's net/http
   * client fails it ("unsupported transfer encoding"), and maps to
   * ccol_http_transfer_aborted like the 101 above. The connection isn't
   * pooled after such a stop. */
  if (p->status_code >= 200 && chttp1_has_other_transfer_coding(p)) return -1;

  /* We always find out whether the response IS a redirect, however many
   * hops the chain has used. Following it or reporting
   * ccol_http_too_many_redirects is up to the caller, who knows the hop
   * count: the hop loop of chttp_do_internal for Tier 1, and
   * _async_handle_redirect for Tier 2 and Tier 3. This callback has no hop
   * count of its own to check.
   *
   * A redirect status is followed only with a present, non-empty Location.
   * A 3xx without Location or with an empty one is the final response of
   * the request, same as in Go's net/http client, since an empty field
   * value names no target and there is nothing to follow.
   *
   * At this point the map holds the FIRST occurrence of each name, as
   * _field_lines_combine runs below, so a response with several Location
   * fields redirects to the first one like curl and Go's net/http, and
   * never to a combined "a, b" that names no resource at all. */
  bool is_redirect_status = ctx->status_code == 301 ||
                            ctx->status_code == 302 ||
                            ctx->status_code == 303 ||
                            ctx->status_code == 307 || ctx->status_code == 308;
  if (is_redirect_status) {
    cmap_pair kp = {.ptr = (void *)"location", .size = sizeof("location")};
    cmap_pair vp;
    if (chmap_get_elem_ref(ctx->headers, &kp, &vp) == ccol_success &&
        ((const char *)vp.ptr)[0] != '\0') {
      ctx->location = ccol_strdup(ctx->mp, (const char *)vp.ptr);
      if (!ctx->location) {
        ctx->error = true;
        /* Anything other than 0 or 1 stops the parse with
         * CHTTP1_USER. */
        return -1;
      }
      ctx->will_redirect = true;
    }
  }

  if (ctx->field_lines && !_field_lines_combine(ctx)) {
    ctx->error = true;
    /* Returning anything but 0 or 1 stops the parse with CHTTP1_USER. */
    return -1;
  }

  /* Refuses an oversized Content-Length that the server honestly declares,
   * before we read one body byte off the wire. It only makes sense for a
   * buffered request that is NOT a redirect itself: _sink_discard throws a
   * redirect's body away whatever length it declares, and a caller that
   * streams the body manages its own memory through the return value of
   * chttpcli_write_fn and never has a chttp_bodybuf_t. It also only makes
   * sense for a message that really puts a body on the wire.
   *
   * Some messages can legitimately declare a Content-Length that has
   * nothing to do with what we read. A 1xx informational response is one:
   * RFC 7230 SS3.3.2 says a server MUST NOT send Content-Length on it, but
   * a broken or malicious server can do it anyway, and the caller's
   * interim-response code always discards or skips such a response and
   * never delivers it as a final result (the is_skippable_1xx check of
   * _chttp_read_message_loop, or the no_body rule for 1xx that the parser
   * applies a few lines below this callback). A 204 or 304 is another,
   * RFC 7230 SS3.3 treats them like 1xx for body framing. A 304 often
   * carries the original resource's Content-Length, which RFC 7232 SS4.1
   * allows, for example on a conditional GET against a CDN, and
   * chttp1_parser forces no_body for both whatever they declare (see the
   * same status codes in chttp1_parser.c). The last one is a HEAD response,
   * whose Content-Length describes what a GET would have returned (RFC 7231
   * SS4.3.2) and no body bytes follow (see the is_head_request check at the
   * very end of this function).
   *
   * Without these exclusions the cap would fail the WHOLE request with
   * ccol_msg_too_large while the response we deliver is well inside the
   * configured limit (it has zero bytes for 1xx, 204 and 304). Ordinary
   * things would break: a 103 Early Hints, or any other 1xx, declaring an
   * oversized length ahead of a small final response inside the cap; a
   * plain HEAD request on a large resource; and a normal conditional GET
   * getting 304 Not Modified with the original resource's oversized
   * Content-Length.
   *
   * A chunked or EOF-delimited body declares no Content-Length, so there is
   * nothing to check up front, but the per-append check in _sink_buffered
   * below keeps it bounded. */
  bool is_informational_status =
      ctx->status_code >= 100 && ctx->status_code < 200;
  bool is_always_bodyless_status = is_informational_status ||
                                   ctx->status_code == 204 ||
                                   ctx->status_code == 304;
  if (!ctx->will_redirect && !ctx->is_head_request &&
      !is_always_bodyless_status && ctx->requested_sink_fn == _sink_buffered) {
    chttp_bodybuf_t *bb = (chttp_bodybuf_t *)ctx->requested_sink_ctx;
    if (bb->max_size > 0 && chttp1_has_content_length(p) &&
        chttp1_declared_content_length(p) > (uint64_t)bb->max_size) {
      ctx->too_large = true;
      /* A value other than 0 and 1 stops the parse with
       * CHTTP1_USER. */
      return -1;
    }
  }

  ctx->sink_fn = ctx->will_redirect ? _sink_discard : ctx->requested_sink_fn;
  ctx->sink_ctx = ctx->will_redirect ? NULL : ctx->requested_sink_ctx;

  /* A HEAD response's Content-Length describes a body the server never
   * sent, and the header may be missing altogether. It's the one case the
   * parser can't work out from the wire alone, so we tell it. */
  return ctx->is_head_request ? 1 : 0;
}

static int _on_body(chttp1_parser_t *p, const char *at, size_t len) {
  chttp_parse_ctx_t *ctx = (chttp_parse_ctx_t *)p->data;
  size_t n = ctx->sink_fn ? ctx->sink_fn(at, len, ctx->sink_ctx) : len;
  if (n != len) {
    /* _sink_buffered returns short for one of two reasons. Its realloc
     * failed, which we report in ctx->error and the caller sees as
     * ccol_not_enough_memory, or the body went over the configured response
     * size cap, reported in ctx->too_large and seen as ccol_msg_too_large.
     * The third case is a streaming caller stopping the body, reported in
     * ctx->aborted and seen as ccol_http_transfer_aborted. All three come
     * back from chttp1_parser_execute as the same CHTTP1_USER, and this is
     * the last place where we can tell them apart. */
    if (ctx->sink_fn == _sink_buffered) {
      chttp_bodybuf_t *bb = (chttp_bodybuf_t *)ctx->sink_ctx;
      if (bb->too_large)
        ctx->too_large = true;
      else
        ctx->error = true;
    } else {
      ctx->aborted = true;
    }
    return 1;
  }
  return 0;
}

static int _on_message_complete(chttp1_parser_t *p) {
  chttp_parse_ctx_t *ctx = (chttp_parse_ctx_t *)p->data;
  ctx->message_complete = true;
  return 0;
}

static struct {
  chttp1_settings_t settings;
  _ccol_once_flag_t once;
} client_http1_settings_bundler = {0};

static void _init_chttp1_settings(void) {
  chttp1_settings_init(&client_http1_settings_bundler.settings);
  client_http1_settings_bundler.settings.on_header = _on_header;
  /* settings.on_trailer is left NULL as chttp1_settings_init zeroed it, and
   * this client has no trailer API. With no callback set, a trailer field
   * has no way to reach ctx->headers at all (see the comment of
   * _on_header). */
  client_http1_settings_bundler.settings.on_headers_complete =
      _on_headers_complete;
  client_http1_settings_bundler.settings.on_body = _on_body;
  client_http1_settings_bundler.settings.on_message_complete =
      _on_message_complete;
}

static void _parse_ctx_free_fields(chttp_parse_ctx_t *ctx) {
  _ccol_mem_free(ctx->mp, ctx->location);
  if (ctx->headers) __chmap_destroy(ctx->headers);
  _field_lines_free(ctx->mp, ctx->field_lines);
  ctx->location = NULL;
  ctx->headers = NULL;
  ctx->field_lines = NULL;
}

/* Resets ctx for a SECOND, logically separate message on the same
 * connection, the real final response that comes after the "100 Continue"
 * interim response this ctx just parsed. chttp_do_internal gives every hop
 * fresh state with a new chttp_parse_ctx_t, and this is the same idea
 * inside a single hop, which the two-message exchange on one connection
 * needs. Headers on the interim response are rare but legal, and they must
 * never leak into the final response's header map.
 *
 * is_head_request is left alone since it belongs to the request and not to
 * any one parsed message, and so are requested_sink_fn and
 * requested_sink_ctx, which hold the caller's real sink configuration.
 * sink_fn and sink_ctx are cleared because _on_headers_complete resolves
 * them again for the message it parses. */
static ccol_retval_t _parse_ctx_reset_for_continue(chttp_parse_ctx_t *ctx) {
  _ccol_mem_free(ctx->mp, ctx->location);
  ctx->location = NULL;
  if (ctx->headers) __chmap_destroy(ctx->headers);
  _field_lines_free(ctx->mp, ctx->field_lines);
  ctx->field_lines = NULL;
  char *herr = NULL;
  ctx->headers =
      chmap_create_full(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                        ccol_string, ctx->mp, NULL, NULL, &herr);
  ctx->will_redirect = false;
  ctx->message_complete = false;
  ctx->trailing_garbage = false;
  ctx->error = false;
  ctx->aborted = false;
  ctx->too_large = false; /* No path gets here with the flag set, since
                           * every path that sets it fails the read with
                           * CHTTP1_USER before this reset can run. We clear
                           * it anyway so a stale "too large" verdict from a
                           * discarded interim message can never carry over
                           * into the next message this function prepares
                           * ctx for. */
  ctx->status_code = 0;
  ctx->sink_fn = NULL;
  ctx->sink_ctx = NULL;
  return ctx->headers ? ccol_success : ccol_not_enough_memory;
}

/*
 * Reads and parses exactly one HTTP/1.1 message from `conn`. Whatever
 * `carry_in` holds goes to the parser first, before we touch the socket,
 * and once carry_in is used up we fall back to normal socket reads bounded
 * by the deadline. On ccol_success *keep_alive_out has chttp1_parser's own
 * keep-alive answer, which doesn't account for trailing garbage yet (see
 * _chttp_early_resp_finish, the only caller that should take leftover
 * bytes as garbage).
 *
 * Bytes past the message boundary aren't treated as trailing garbage here.
 * We hand them back through leftover_out and leftover_len_out in a buffer
 * allocated with pctx->mp (NULL and 0 when there are none). The "Expect:
 * 100-continue" code of chttp_do_internal needs that to carry the final
 * response of a fast server into a second parse, since those bytes can
 * come in the same read as the "100 Continue" interim status line. No other
 * caller has a second message to carry them into, so for those callers a
 * non-empty leftover is exactly the trailing garbage it looks like (see
 * _chttp_early_resp_finish).
 *
 * *any_bytes_read_out becomes true as soon as the first byte of THIS
 * message reaches the parser, no matter where it came from: a live read in
 * this call, or carry_in, bytes the caller read in an earlier call, which
 * is what the "Expect: 100-continue" code of chttp_do_internal does when it
 * passes a fast server's leftover bytes into the read of the real final
 * response.
 *
 * Both sources are handled the same way on purpose. A non-empty carry_in
 * goes straight into chttp1_parser_execute() below, which can run
 * on_header, on_body and on_headers_complete against pctx and the caller's
 * sink just like a live read. So a failure halfway through can leave real
 * state that the caller sees, even if we never read a byte from the fd in
 * this call.
 *
 * The caller classifies a failure with *any_bytes_read_out. When it's
 * false nothing was parsed or delivered, and a quiet retry on a fresh
 * connection is safe. When it's true part of a response is in flight and a
 * user callback may hold some of it, so the failure must be reported.
 * carry_in is NOT exempt from this on purpose. With an exemption, the
 * retry-once safety net of chttp_do_internal for a reused connection could
 * resend on a fresh connection while reusing a chttp_parse_ctx_t and a body
 * buffer that a dead carry_in parse partly filled, and the headers and body
 * prefix of a discarded response would silently mix into the response we
 * deliver.
 *
 * first_byte is NULL or a deadline for the wait for this message's first
 * byte only; the "Expect: 100-continue" wait of _chttp_send_and_read passes
 * its window here. Until a byte of the message reaches the parser, that
 * deadline bounds the read and its expiry returns ccol_timed_out with the
 * parser untouched. That being said, from the first byte on only `overall`
 * bounds the read, so a message the server started inside the window is
 * always read to the end by the parser that holds its start. A TLS record
 * with no plaintext, a session ticket for example, starts no message.
 */
#ifdef RUNNING_UNIT_TESTS
/* Counts of the reads and of the readability waits in the Tier 1 read
 * loop, so a test can check how the two are ordered. */
static unsigned long _chttp_tier1_reads;
static unsigned long _chttp_tier1_read_waits;
unsigned long _chttp_tier1_reads_for_tests(void) {
  return __atomic_load_n(&_chttp_tier1_reads, __ATOMIC_RELAXED);
}
unsigned long _chttp_tier1_read_waits_for_tests(void) {
  return __atomic_load_n(&_chttp_tier1_read_waits, __ATOMIC_RELAXED);
}
#endif

static ccol_retval_t _chttp_read_message_with(
    chttp1_parser_t *parser, chttp_conn_t *conn, chttp_parse_ctx_t *pctx,
    chttp_deadline_t *overall, chttp_deadline_t *first_byte,
    const char *carry_in, size_t carry_in_len, bool *keep_alive_out,
    bool *any_bytes_read_out, char **leftover_out, size_t *leftover_len_out) {
  *leftover_out = NULL;
  *leftover_len_out = 0;
  /* Set once a byte of this message has reached the parser; after that only
   * `overall` bounds the read (see the doc comment above about
   * first_byte). */
  bool started = carry_in_len > 0;

  if (carry_in_len > 0) {
    *any_bytes_read_out = true;
    chttp1_errno_t err = chttp1_parser_execute(parser, carry_in, carry_in_len);
    if (err == CHTTP1_PAUSED) {
      size_t consumed = chttp1_parser_consumed(parser);
      *keep_alive_out = chttp1_should_keep_alive(parser);
      if (consumed < carry_in_len) {
        size_t rem = carry_in_len - consumed;
        char *lb = (char *)_ccol_mem_alloc(pctx->mp, rem);
        if (!lb) return ccol_not_enough_memory;
        memcpy(lb, carry_in + consumed, rem);
        *leftover_out = lb;
        *leftover_len_out = rem;
      }
      return ccol_success;
    }
    if (err == CHTTP1_USER) {
      if (pctx->too_large) return ccol_msg_too_large;
      return pctx->error ? ccol_not_enough_memory : ccol_http_transfer_aborted;
    }
    if (err != CHTTP1_OK) return ccol_http_transfer_aborted;
    /* Message incomplete, fall through and read more. */
  }

  char buf[8192];
  /* Mirrors want_events of _chttp_send_all with the opposite home
   * direction. An EWOULDBLOCK from ctls_conn_read() can just as well mean
   * "wait for WRITABLE", when OpenSSL needs to flush a renegotiation or a
   * KeyUpdate response before it can decrypt another record. Polling for
   * POLLIN unconditionally there waits for readability that never comes,
   * the request stalls for the rest of its deadline, and with the default
   * request_timeout_us of 0 that's forever. */
  short want_events = POLLIN;
  /* True after a read that got EWOULDBLOCK, false after one that made
   * progress; the wait below uses it. */
  bool read_blocked = false;
  /* We wait before the first read and after a read that blocked, but after
   * a read that made progress we read again right away. A body that is
   * arriving usually has more bytes sitting in the socket, and a poll(2)
   * before every read would double the system calls of a large body. */
  bool wait_first = true;
  for (;;) {
    int wait_ms;
    chttp_deadline_t *dl = (started || !first_byte) ? overall : first_byte;
    /* _read_deadline_passed explains the rule behind this check. */
    if (_read_deadline_passed(dl, conn->fd) ||
        !_deadline_remaining_ms(dl, &wait_ms))
      return ccol_timed_out;
    /* When a TLS read uses a buffer smaller than the record, the rest of
     * the record stays inside the TLS layer where poll(2) can't see it, and
     * a readability wait would wait for bytes that have arrived, until the
     * deadline or forever with the default request_timeout_us of 0. So
     * while ctls_conn_has_pending_input reports input we read first. Once
     * a read blocks the wait is right again, because what's pending is only
     * the start of an incomplete record. */
    bool buffered = conn->tls && want_events == POLLIN && !read_blocked &&
                    ctls_conn_has_pending_input(conn->tls);
    if (wait_first && !buffered) {
#ifdef RUNNING_UNIT_TESTS
      __atomic_fetch_add(&_chttp_tier1_read_waits, 1, __ATOMIC_RELAXED);
#endif
      ccol_retval_t prv = _conn_wait(conn->fd, want_events, wait_ms,
                                     ccol_http_transfer_aborted);
      if (prv == ccol_timed_out) continue; /* Deadline checked at the top. */
      if (prv != ccol_success) return prv;
    }

#ifdef RUNNING_UNIT_TESTS
    __atomic_fetch_add(&_chttp_tier1_reads, 1, __ATOMIC_RELAXED);
#endif
    ssize_t n = _conn_read(conn, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EWOULDBLOCK) {
        read_blocked = true;
        wait_first = true;
        if (conn->tls)
          want_events = _conn_tls_wants_write(conn->tls) ? POLLOUT : POLLIN;
        continue;
      }
      return ccol_http_transfer_aborted;
    }
    read_blocked = false;
    wait_first = false;
    /* Progress or EOF, so the next attempt goes back to this call's home
     * direction, the same way _chttp_send_all resets its own. */
    want_events = POLLIN;
    if (n > 0) {
      *any_bytes_read_out = true;
      started = true;
    }
    if (n == 0) {
      /* A valid EOF-delimited body can end here with CHTTP1_PAUSED as well
       * as with CHTTP1_OK, and we expect both. That kind of body comes from
       * an HTTP/1.0-style response, or from an explicit "Connection: close"
       * without Content-Length or chunked framing (see the doc comment of
       * chttp1_parser_finish). Only CHTTP1_ERROR, a truncated message, goes
       * to the aborted case. */
      chttp1_errno_t fe = chttp1_parser_finish(parser);
      if ((fe != CHTTP1_OK && fe != CHTTP1_PAUSED) || !pctx->message_complete)
        return ccol_http_transfer_aborted;
      /* The peer closed, nothing left here to reuse. */
      *keep_alive_out = false;
      return ccol_success;
    }

    chttp1_errno_t err = chttp1_parser_execute(parser, buf, (size_t)n);
    if (err == CHTTP1_PAUSED) {
      size_t consumed = chttp1_parser_consumed(parser);
      *keep_alive_out = chttp1_should_keep_alive(parser);
      if (consumed < (size_t)n) {
        size_t rem = (size_t)n - consumed;
        char *lb = (char *)_ccol_mem_alloc(pctx->mp, rem);
        if (!lb) return ccol_not_enough_memory;
        memcpy(lb, buf + consumed, rem);
        *leftover_out = lb;
        *leftover_len_out = rem;
      }
      return ccol_success;
    }
    if (err == CHTTP1_USER) {
      if (pctx->too_large) return ccol_msg_too_large;
      return pctx->error ? ccol_not_enough_memory : ccol_http_transfer_aborted;
    }
    if (err != CHTTP1_OK) return ccol_http_transfer_aborted;
    /* Incomplete message, go around for more data. */
  }
}

static ccol_retval_t _chttp_read_message(
    chttp_conn_t *conn, chttp_parse_ctx_t *pctx, chttp_deadline_t *overall,
    chttp_deadline_t *first_byte, const char *carry_in, size_t carry_in_len,
    bool *keep_alive_out, bool *any_bytes_read_out, char **leftover_out,
    size_t *leftover_len_out) {
  _ccol_call_once(client_http1_settings_bundler.once, _init_chttp1_settings);

  /* Response header lines may be up to CHTTP1_MAX_SPILL_LINE_LEN bytes. The
   * parser allocates only when a line outgrows its fixed buffer, and the
   * release below frees that allocation on every return. */
  chttp1_parser_t parser;
  chttp1_parser_init(&parser, &client_http1_settings_bundler.settings);
  parser.data = pctx;
  (void)chttp1_parser_enable_line_spill(&parser, pctx->mp);
  ccol_retval_t rv = _chttp_read_message_with(
      &parser, conn, pctx, overall, first_byte, carry_in, carry_in_len,
      keep_alive_out, any_bytes_read_out, leftover_out, leftover_len_out);
  chttp1_parser_release(&parser);
  return rv;
}

/*
 * Same job as _chttp_read_message, but we read past and drop every interim
 * 1xx response whose status isn't stop_at_status (pass 0 when no status
 * should stop us, so every 1xx is skipped). We keep going until a message
 * arrives that the caller actually wants: stop_at_status itself, any final
 * response with a status of 200 or more, or a hard failure or a timeout.
 *
 * Skipping interim responses here is what makes a 1xx that the server sends
 * on its own harmless. If each caller got exactly one message and took it as
 * final, things break, because a server may send an interim status other
 * than the one the caller waits for. "103 Early Hints" (RFC 8297) is the
 * main example, and it can come ahead of an ORDINARY response too, not only
 * ahead of "100 Continue". Taking it as the final response would hand the
 * caller an empty result with the wrong status while the server's real
 * response sits unread on the wire, and that corrupts whatever unrelated
 * later request picks this connection out of the keep-alive pool.
 *
 * Before the next read, _parse_ctx_reset_for_continue clears the pctx state
 * of every dropped interim message (the headers, the Location and the other
 * fields), the same way it does for "100 Continue", so nothing from a
 * dropped message can leak into the one the caller receives.
 *
 * A fast server may already be writing its next message in the same read.
 * The bytes past the end of a DROPPED message therefore go into the very
 * next read as its carry_in, which is the same trick the "100 Continue" code
 * of chttp_do_internal uses. What we report through leftover_out and
 * leftover_len_out is only what lies past the message we RETURN, matching
 * the contract of _chttp_read_message, and the caller decides what to do
 * with it: _chttp_early_resp_finish always treats it as trailing garbage,
 * while the "100 Continue" wait of _chttp_send_and_read carries it forward
 * into the real final read.
 *
 * A dropped interim message may read bytes off the wire and set
 * *any_bytes_read_out to true, and on purpose we never put that flag back to
 * false for a later message. A complete interim response (not a partial or
 * abandoned one) proves that the server got this request and started to
 * answer it. A caller-level recovery that retries the whole hop on a fresh
 * connection sends the request again from the start, so it can't pretend
 * nothing happened yet. Every other use of any_bytes_read_out in this file
 * rests on the same reasoning.
 *
 * discarded_before counts the interim responses of the same sequence that
 * the caller already dropped before this call, while the request was going
 * out, and the CHTTP_MAX_INTERIM_RESPONSES cap counts those as well.
 */
static ccol_retval_t _chttp_read_message_loop(
    chttp_conn_t *conn, chttp_parse_ctx_t *pctx, chttp_deadline_t *overall,
    chttp_deadline_t *first_byte, const char *carry_in, size_t carry_in_len,
    int stop_at_status, size_t discarded_before, bool *keep_alive_out,
    bool *any_bytes_read_out, char **leftover_out, size_t *leftover_len_out) {
  char *cur_carry = NULL;
  size_t cur_carry_len = 0;
  if (carry_in_len > 0) {
    cur_carry = (char *)_ccol_mem_alloc(pctx->mp, carry_in_len);
    if (!cur_carry) return ccol_not_enough_memory;
    memcpy(cur_carry, carry_in, carry_in_len);
    cur_carry_len = carry_in_len;
  }

  /* We compare n_discarded with the cap only AFTER reading a message and
   * finding that it is an interim one we can skip, never before a read. The
   * documented contract, in the comment of CHTTP_MAX_INTERIM_RESPONSES, is a
   * limit on how many responses the loop drops before it gives up.
   *
   * Comparing at the start of every read would apply the cap to interim and
   * final messages alike. After 64 dropped interim responses we would then
   * skip reading the next message and refuse a real final response sitting
   * in position 65, which is one message too early and breaks the contract of
   * this function (64 consecutive dropped interim responses are allowed, and
   * chttpclient_do(3) publishes that too). It would also disagree with the
   * same loop in the async engine, _async_on_readable_impl, which caps the
   * interim messages only and never the final response.
   *
   * Doing the comparison here, once we know what the message is, keeps the
   * two tiers in exact agreement: a final response can always be delivered,
   * whatever its position, and only the number of dropped interim responses
   * is limited. */
  for (size_t n_discarded = discarded_before;;) {
    char *leftover = NULL;
    size_t leftover_len = 0;
    ccol_retval_t rv = _chttp_read_message(
        conn, pctx, overall, first_byte, cur_carry, cur_carry_len,
        keep_alive_out, any_bytes_read_out, &leftover, &leftover_len);
    _ccol_mem_free(pctx->mp, cur_carry);
    cur_carry = NULL;
    cur_carry_len = 0;
    if (rv != ccol_success) {
      _ccol_mem_free(pctx->mp, leftover);
      return rv;
    }

    bool is_skippable_1xx = pctx->status_code >= 100 &&
                            pctx->status_code < 200 &&
                            pctx->status_code != stop_at_status;
    if (!is_skippable_1xx) {
      *leftover_out = leftover;
      *leftover_len_out = leftover_len;
      return ccol_success;
    }

    if (n_discarded >= CHTTP_MAX_INTERIM_RESPONSES) {
      _ccol_mem_free(pctx->mp, leftover);
      return ccol_http_transfer_aborted;
    }
    n_discarded++;

    ccol_retval_t rrv = _parse_ctx_reset_for_continue(pctx);
    if (rrv != ccol_success) {
      _ccol_mem_free(pctx->mp, leftover);
      return rrv;
    }
    cur_carry = leftover;
    cur_carry_len = leftover_len;
  }
}

/* ========================================================================== */
/*                         TLS CONTEXT MANAGEMENT                             */
/* ========================================================================== */

static bool _file_readable(const char *path) {
  return path && access(path, R_OK) == 0;
}

/*
 * Builds (or rebuilds) cli->tls_ctx from cli->tls. It runs when a client is
 * constructed, with the default configuration, and from
 * chttpclient_set_tls.
 *
 * ctls_ctx_cert_add and ctls_ctx_trust report a failure as an ordinary
 * ccol_retval_t and never end the process, so a missing or unreadable file
 * is always a recoverable error here and never an abort. chttpclient_set_tls
 * must accept a path that is syntactically fine but doesn't exist yet,
 * without crashing, because a caller may configure TLS long before its
 * first HTTPS request, or never make one at all. So we check the configured
 * paths with access() BEFORE calling into ctls. When a path fails that
 * check we build no context and the failure waits until connection time, in
 * chttp_do_internal, where it shows up as an ordinary ccol_retval_t.
 *
 * That check up front is what gives this function its contract (it always
 * returns ccol_success and a failure always waits). The graceful failure of
 * ctls_ctx_cert_add alone would not give us that.
 */
static ccol_retval_t _rebuild_tls_ctx_locked(struct chttpclient *cli) {
  /* We bump the generation before anything else, and the bump holds on every
   * way out of this function, the deferred-failure returns below included. A
   * pooled connection only cares that a new configuration replaced the one
   * its handshake ran under, whether the replacement built or not (see the
   * field comment of cli->tls_generation). Reaching UINT64_MAX would take one
   * reconfiguration per nanosecond for more than five centuries, so we don't
   * handle a wrap; the worst a wrap could do is give one pooled connection
   * one extra reuse. */
  cli->tls_generation++;
  if (cli->tls_ctx) {
    ctls_ctx_release(cli->tls_ctx);
    cli->tls_ctx = NULL;
  }
  cli->tls_ctx_usable = false;

  bool have_cert_pair = cli->tls.cert_path && cli->tls.key_path;
  if (have_cert_pair && (!_file_readable(cli->tls.cert_path) ||
                         !_file_readable(cli->tls.key_path))) {
    /* The failure waits. See the comment above. */
    return ccol_success;
  }
  if (cli->tls.ca_bundle_path && !_file_readable(cli->tls.ca_bundle_path)) {
    /* The failure waits. See the comment above. */
    return ccol_success;
  }

  ctls_ctx_t *ctx = ctls_ctx_new_mp(cli->m_procs, NULL);
  if (!ctx) return ccol_not_enough_memory;

  if (have_cert_pair) {
    if (ctls_ctx_cert_add(ctx, NULL, cli->tls.cert_path, cli->tls.key_path,
                          NULL, NULL) != ccol_success) {
      ctls_ctx_release(ctx);
      /* The failure waits. See the comment above. */
      return ccol_success;
    }
  }

  if (cli->tls.ca_bundle_path) {
    if (ctls_ctx_trust(ctx, cli->tls.ca_bundle_path, NULL) != ccol_success) {
      ctls_ctx_release(ctx);
      /* The failure waits. See the comment above. */
      return ccol_success;
    }
  } else if (!cli->tls.insecure_skip_verify) {
    /* No explicit bundle and the caller didn't opt out, so we verify the
     * chain against the system trust store. A zero-initialised
     * chttp_tls_config_t lands in this branch; see the field comment of
     * insecure_skip_verify. The chain check and the hostname check share one
     * switch, because matching a hostname against a certificate whose chain
     * nobody validated gives no security at all. */
    if (ctls_ctx_trust_system(ctx) != ccol_success) {
      /* No check up front can catch this one: ctls couldn't load the
       * platform's default CA store. A missing or unreadable CA-bundle file
       * is a different case, and the access() call above catches it before
       * anything touches ctls.
       *
       * We have to throw ctx away completely here instead of falling
       * through and installing it. ctx already committed SSL_VERIFY_PEER
       * (the verify_peer flag that ctls_ctx_trust_system sets) before this
       * rebuild failed, but the rebuild that would have loaded the trust
       * anchors never committed. Keeping ctx would leave the PREVIOUS
       * ctx_default from the first build of ctls_ctx_new_mp installed, and
       * that one predates verify_peer, so no certificate would be verified
       * at all. Every other failure in this function drops ctx for the same
       * reason. */
      ctls_ctx_release(ctx);
      /* The failure waits. See the comment above. */
      return ccol_success;
    }
  }

  cli->tls_ctx = ctx;
  cli->tls_ctx_usable = true;
  return ccol_success;
}

/* ========================================================================== */
/*                    SHARED STATIC REACTOR (chttpclient's OWN engine)        */
/* ========================================================================== */

/*
 * One static ccol_event_loop reactor for the whole process, shared by every
 * chttpcli instance in it, the default client that the library creates on
 * demand included. There are two separate reactors in the library and they
 * know nothing of each other; the other one belongs to chttpserver (see the
 * same lifecycle wrapper in chttpserver.c).
 *
 * Since chttpclient shares no process-wide singleton with chttpserver, this
 * wrapper doesn't need any ordering across modules, just a refcount over
 * the Tier 2 and Tier 3 callers. It has the same acquire, release and
 * reaper-thread shape as the g_reactor of chttpserver.c, and on top of it
 * chttpclient keeps resources of its own (cli_engine_bundler.dns_pool and
 * the deadline sweep), which go down in step with the reactor.
 */
static struct {
  ccol_event_loop reactor;
  size_t reactor_refs;
  _ccol_mutex_t mutex;
  _ccol_cond_var_t stopped_cv;
  _ccol_once_flag_t once;
  bool stopping;
  _ccol_thread_id_t reaper_thread;
  bool reaper_joinable;
  ccol_memmgmt_procs_t mprocs_storage;
  ccol_memmgmt_procs_t *mprocs;

  /* 0, the module default, means we detect the count with
   * sysconf(_SC_NPROCESSORS_ONLN); a positive value pins the reactor to
   * exactly that many OS threads (see the doc comment of
   * chttpcli_set_engine_num_reactor_threads). The value is baked into the
   * reactor when it gets built, so the same rule as for mprocs above holds:
   * set it before the first start, or after a full stop. */
  size_t num_reactor_threads;
  /* Whatever count (detected or set explicitly) we passed to
   * ccol_event_loop_create_mp the last time the reactor was built. Only the
   * test instrumentation reads it (see
   * _chttpclient_engine_num_reactor_threads_for_tests below). */
  size_t last_resolved_num_reactor_threads;

  /* Logger for diagnostics from chttpclient's reactor threads, such as TLS
   * handshake failures and connect errors. While the engine runs it is
   * user_logger if a caller installed one with chttpcli_set_engine_logger;
   * if not, _client_engine_acquire opens a fallback logger at the CLOG_FATAL
   * level on fd 2 every time the engine starts, and the reaper closes that
   * fallback when the engine stops. So the field is never NULL while the
   * engine runs.
   *
   * Other code depends on that, too. The teardown clog_info call of
   * _client_engine_reaper_fn needs SOME live logger to call through, so does
   * the call in the deadline sweep, and chttp1_parser has no logger of its
   * own. _clog_write doesn't guard against a NULL handle, so a call through a
   * NULL logger crashes instead of being skipped.
   *
   * The engine gets reaped with this field left NULL on one rare path only:
   * when allocating the logger itself fails with an out-of-memory error (see
   * the comment on that path in _client_engine_acquire).
   *
   * cli_engine_bundler.mutex only protects this field from a torn read or
   * write racing a concurrent chttpcli_set_engine_logger() call. clog by
   * itself is already thread-safe for concurrent log calls through one
   * handle. */
  clog logger;

  /* The logger that chttpcli_set_engine_logger derived, or CLOG_INVALID.
   * The engine owns it for the rest of the process, through every stop and
   * restart, until a later chttpcli_set_engine_logger call replaces it.
   * Stopping the engine never closes it; only that replacement in
   * chttpcli_set_engine_logger and this module's destructor do. If the
   * reaper closed it, every later engine start would run on the fallback
   * logger and the logger the caller installed once would be dropped without
   * a word. cli_engine_bundler.mutex guards it. */
  clog user_logger;

  /* Takes the DNS resolve and connect step, which can block, off the caller
   * threads. The pool lives and dies with the reactor, and both are created
   * and destroyed together under cli_engine_bundler.mutex. We don't make it
   * a separate on-demand singleton because nothing needs it once the
   * reactor is down.
   */
  ctpool dns_pool;

  /* How many engine references the async chains hold, a subset of
   * reactor_refs (the idle pool holds the rest). A chain whose future is
   * already fulfilled keeps its reference until its last connection is torn
   * down or pooled, and the reactor does that without waiting. _client_at_exit
   * waits for this count to drop to zero, and exit_waiting tells
   * _client_engine_acquire_chain and _client_engine_release_chain to wake
   * it. */
  size_t chains_live;
  bool exit_waiting;

  /* Guards the single registration of _client_at_exit; see the comment of
   * that function. */
  _ccol_once_flag_t exit_hook_once;
#if CCOL_FORK_SAFETY_REQUIRED
  /* Guards the single registration of the fork handlers; see
   * _client_atfork_install. */
  _ccol_once_flag_t atfork_once;
#endif
} cli_engine_bundler = {0};

/* The process that last built the engine, or that registered the exit hook
 * first, and _client_at_exit only acts in that process. A fork() child gets
 * the engine's state but none of its threads, and the reaper handle it
 * inherits names a thread of the parent. It is atomic because the exit hook
 * reads it before taking any lock: in such a child the engine mutex may have
 * been inherited locked. */
static _Atomic pid_t g_client_engine_pid = 0;

/* Async requests in the process, from any client, whose future isn't
 * fulfilled yet. _client_engine_acquire_chain adds one under the engine
 * mutex, and the first fulfilment of the future takes it off again before
 * the future becomes ready. The only application code that ever runs on an
 * engine thread is the write_fn of a streaming request, and that runs
 * strictly before its own future is fulfilled. A zero here therefore proves
 * to _client_at_exit that the thread calling exit() is not inside such a
 * callback and that no request is in flight. See that function. */
static _Atomic size_t g_async_requests_pending = 0;
/* Async requests whose future the deadline sweep fulfilled while their hop
 * is alive (see _client_deadline_sweep_once). That is a hop stuck in a name
 * lookup, which we can't interrupt, or a hop waiting for a reactor dispatch
 * that never comes, for example while the dispatch pool can't allocate. The
 * exit handler counts such a hop as in flight, just like a pending request,
 * so exit() never waits for it. */
static _Atomic size_t g_async_hops_abandoned = 0;

static ccol_retval_t _client_deadline_sweep_start(void);
static void _client_deadline_sweep_stop_and_join(void);
static void _client_exit_hook_install(void);
static void _default_client_destroy_if_idle(void);

#if CCOL_FORK_SAFETY_REQUIRED
static void _chttpcli_atfork_prepare(void);
static void _chttpcli_atfork_parent(void);
static void _chttpcli_atfork_child(void);
#endif

#if CCOL_FORK_SAFETY_REQUIRED
/* Registers this module's fork handlers. The first engine acquire runs it
 * once through cli_engine_bundler.atfork_once, so a process that never
 * touches Tier 2 or Tier 3 registers nothing and builds no state in the
 * modules below. */
static void _client_atfork_install(void) {
  /* pthread_atfork runs prepare handlers in reverse registration order. We
   * get ccol_event_loop, ctpool and clog registered first, so our prepare
   * handler always runs first and takes its locks before theirs. That
   * matches the order of every ordinary call here: cli_engine_bundler.mutex
   * is held while the reactor, the DNS pool and the fallback logger are
   * created and across log calls, and a deadline stripe is held across a
   * modify of a reactor registration. */
  _cthreadcomm_ensure_atfork_registered_before_caller();
  _ctpool_ensure_atfork_registered_before_caller();
  _clog_ensure_atfork_registered_before_caller();
  _ccol_at_fork(_chttpcli_atfork_prepare, _chttpcli_atfork_parent,
                _chttpcli_atfork_child);
}
#endif

#if defined(_CCOL_ATFORK_AT_LOAD) && CCOL_FORK_SAFETY_REQUIRED
/* See _CCOL_ATFORK_AT_LOAD in common.h. */
__attribute__((constructor)) static void _client_atfork_at_load(void) {
  _ccol_call_once(cli_engine_bundler.atfork_once, _client_atfork_install);
}
#endif

/* Set as the very first step of _client_engine_globals_init, so the
 * destructor below can tell whether this process ever initialized the
 * engine globals. */
static _Atomic bool g_engine_globals_entered = false;

static void _client_engine_globals_init(void) {
  atomic_store(&g_engine_globals_entered, true);
  if (_ccol_mutex_init(cli_engine_bundler.mutex) != 0)
    _ccol_fatal_err("chttpclient engine: failed to initialize mutex");
  if (_ccol_cond_var_init(cli_engine_bundler.stopped_cv) != 0)
    _ccol_fatal_err(
        "chttpclient engine: failed to initialize condition variable");

  /* SIGPIPE's disposition is left to the application. Nothing this module
   * writes can raise it: every plain write in the three tiers is a send()
   * with MSG_NOSIGNAL, and every TLS write (the close_notify of a teardown
   * too) goes through the socket BIO of ctls, which sends with MSG_NOSIGNAL
   * as well. That's the right split for a library: the disposition is a
   * process-wide setting, and changing it would affect code we know nothing
   * about. */
}

/*
 * Logs through cli_engine_bundler.logger, reading that pointer and making
 * the clog_info call inside ONE critical section.
 *
 * Copying the pointer under the lock and using it after unlocking is a
 * use-after-free. chttpcli_set_engine_logger() swaps in the new logger
 * under cli_engine_bundler.mutex, unlocks, and only then calls clog_close()
 * on the OLD one. clog_close() always frees the handle itself, whatever the
 * refcount of the shared backing store says (see clogger.c), so whoever
 * kept a copy from before the swap can end up calling clog_info() on freed
 * memory. Moving the log call out of the lock looks like an easy way to
 * shorten the critical section, and it's exactly that bug, so please don't.
 *
 * It's a macro instead of a function so that clog_info captures __FILE__,
 * __LINE__ and __func__ at the real call site and not inside this helper. */
#define _CLIENT_ENGINE_LOG_INFO(fmt, ...)                                  \
  do {                                                                     \
    _ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init); \
    _ccol_mutex_lock(cli_engine_bundler.mutex);                            \
    if (cli_engine_bundler.logger)                                         \
      clog_info(cli_engine_bundler.logger, fmt, ##__VA_ARGS__);            \
    _ccol_mutex_unlock(cli_engine_bundler.mutex);                          \
  } while (0)

#ifdef RUNNING_UNIT_TESTS
/* How many reaper threads _client_engine_spawn_reaper created and how many
 * we joined. The two match once the engine is quiescent, and a reaper handle
 * that got overwritten before anybody joined it shows up as a join that
 * never happens. */
static _Atomic unsigned g_reaper_created_for_tests = 0;
static _Atomic unsigned g_reaper_joined_for_tests = 0;
#endif

static void _client_engine_join_reaper_if_needed_locked(void) {
  if (cli_engine_bundler.reaper_joinable) {
    _ccol_thread_join(cli_engine_bundler.reaper_thread);
    cli_engine_bundler.reaper_joinable = false;
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add(&g_reaper_joined_for_tests, 1u);
#endif
  }
}

/* True while the engine runs without a reference because
 * _client_engine_spawn_reaper couldn't get a thread for the teardown. The
 * deadline sweep reads it on every tick and gives the teardown to a new
 * reaper as soon as a thread can be had. The sweep reads it before taking
 * cli_engine_bundler.mutex, hence the atomic, and a tick of an engine that
 * isn't stranded takes no engine lock at all. */
static _Atomic bool g_engine_reap_abandoned = false;

/*
 * A fork() child inherits the engine's state but none of its threads. Three
 * fields name a parent thread or wait for one: a reaper handle that nobody
 * joined yet, a `stopping` that only the parent's reaper clears, and a
 * reactor with no reference that waits to be reaped. The child must never
 * join or wait on any of them. A finished reaper isn't joined until the next
 * acquire (or, in a build with the fork handlers, until the next fork), and
 * glibc gives the descriptor of a thread the child didn't inherit to the
 * next thread the child starts. Joining the inherited handle then joins
 * some unrelated thread of the child and can block for good.
 *
 * We run under cli_engine_bundler.mutex, before any wait or join on that
 * state. If the engine belongs to another process we simply forget it, and
 * the child builds its own engine on its next acquire. The parent's reactor
 * and DNS pool stay allocated in the child and are never used there,
 * because tearing them down would join threads that don't exist in this
 * process.
 *
 * The logger of that engine run goes along with it. A fallback logger
 * belongs to the run and only the parent's reaper would close it, so the
 * child closes its own copy here. A logger the application installed stays
 * in user_logger for the engine the child builds. Without this, the child's
 * next engine keeps logging through the handle from the parent's run and
 * nothing in the child ever closes that handle.
 *
 * getpid() is a system call, so we only ask it in the states that can name
 * a thread of another process. A running engine holding references isn't
 * one of them, since a child can only reach it through an inherited handle,
 * which fork(2) doesn't support. The fast path of an acquire reads only the
 * fields it reads anyway and makes no system call. */
static void _client_engine_forget_inherited_locked(void) {
  if (!cli_engine_bundler.stopping && !cli_engine_bundler.reaper_joinable &&
      !(cli_engine_bundler.reactor && cli_engine_bundler.reactor_refs == 0))
    return;
  if (atomic_load(&g_client_engine_pid) == getpid()) return;
  clog inherited_logger = cli_engine_bundler.logger;
  cli_engine_bundler.logger = CLOG_INVALID;
  if (inherited_logger && inherited_logger != cli_engine_bundler.user_logger)
    clog_close(inherited_logger);
  cli_engine_bundler.reactor = CCOL_EVENT_LOOP_INVALID;
  cli_engine_bundler.dns_pool = CTPOOL_INVALID;
  cli_engine_bundler.reactor_refs = 0;
  cli_engine_bundler.stopping = false;
  cli_engine_bundler.reaper_joinable = false;
  cli_engine_bundler.chains_live = 0;
  cli_engine_bundler.exit_waiting = false;
  atomic_store(&g_async_requests_pending, 0);
  atomic_store(&g_async_hops_abandoned, 0);
  atomic_store(&g_engine_reap_abandoned, false);
}

/*
 * Runs on a new thread, never inline on the thread that dropped the last
 * reference. That thread is often a reactor callback thread itself
 * (_async_on_error tearing down the last live ctx is one example), and such
 * a thread must never block joining the deadline sweep or draining
 * cli_engine_bundler.dns_pool.
 *
 * We tear down the deadline sweep and cli_engine_bundler.dns_pool, then
 * destroy the reactor, and clear cli_engine_bundler.stopping last so that a
 * waiting acquirer can go on.
 */
#ifdef RUNNING_UNIT_TESTS
/* While g_reaper_final_hold_ms_for_tests isn't zero, the next reaper keeps
 * cli_engine_bundler.mutex in its final critical section for that many
 * milliseconds and sets g_reaper_final_hold_entered_for_tests as the hold
 * starts. The flag stays set until the next hold is armed, so a test that
 * polls for it can't miss a hold that is already over. The hold fires once
 * and is bounded, so a test that forks during it never hangs the
 * reaper. */
static _Atomic int g_reaper_final_hold_ms_for_tests = 0;
static _Atomic bool g_reaper_final_hold_entered_for_tests = false;

static void _reaper_final_hold_for_tests(void) {
  int ms = atomic_exchange(&g_reaper_final_hold_ms_for_tests, 0);
  if (ms <= 0) return;
  atomic_store(&g_reaper_final_hold_entered_for_tests, true);
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

/* While g_reaper_tail_hold_ms_for_tests isn't zero, the next reaper waits
 * that many milliseconds once it has closed the fallback logger (the last
 * thing it does before its thread exits), holding no lock. It sets
 * g_reaper_tail_hold_entered_for_tests as the wait starts and
 * g_reaper_tail_hold_left_for_tests as it ends. It fires once. */
static _Atomic int g_reaper_tail_hold_ms_for_tests = 0;
static _Atomic bool g_reaper_tail_hold_entered_for_tests = false;
static _Atomic bool g_reaper_tail_hold_left_for_tests = false;

static void _reaper_tail_hold_for_tests(void) {
  int ms = atomic_exchange(&g_reaper_tail_hold_ms_for_tests, 0);
  if (ms <= 0) return;
  atomic_store(&g_reaper_tail_hold_entered_for_tests, true);
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  atomic_store(&g_reaper_tail_hold_left_for_tests, true);
}
#endif

static void *_client_engine_reaper_fn(void *arg) {
  (void)arg;
  ccol_event_loop loop_to_destroy;
  ctpool dns_pool_to_destroy;
  _ccol_mutex_lock(cli_engine_bundler.mutex);
  loop_to_destroy = cli_engine_bundler.reactor;
  dns_pool_to_destroy = cli_engine_bundler.dns_pool;
  _ccol_mutex_unlock(cli_engine_bundler.mutex);

  /* The deadline sweep goes first, ahead of the reactor and the DNS pool,
   * since a sweep may be using them: a tick closes a fd and removes a
   * registration directly (see _client_deadline_sweep_once). */
  _client_deadline_sweep_stop_and_join();
  if (loop_to_destroy) ccol_event_loop_destroy(loop_to_destroy);
  /* ctpool_destroy runs here outside the lock, like the
   * ccol_event_loop_destroy call just above. It is an implicit drain
   * shutdown that joins every worker of dns_pool, and that can be slow when
   * a worker sits inside getaddrinfo() or connect().
   *
   * Holding cli_engine_bundler.mutex across that drain would block other
   * calls for its whole length: chttpcli_set_engine_logger,
   * chttpcli_set_engine_mem_mgmt_procs and
   * chttpcli_set_engine_num_reactor_threads are all cheap, all take this
   * mutex, and none of them waits on a condition variable that would hand
   * the mutex back.
   *
   * Nothing about ordering needs the lock here either, because reactor and
   * dns_pool are always cleared together in the single critical section
   * below, wherever the destroy call sits. */
  if (dns_pool_to_destroy) ctpool_destroy(dns_pool_to_destroy);

  _ccol_mutex_lock(cli_engine_bundler.mutex);
#ifdef RUNNING_UNIT_TESTS
  _reaper_final_hold_for_tests();
#endif
  cli_engine_bundler.dns_pool = CTPOOL_INVALID;
  cli_engine_bundler.reactor = CCOL_EVENT_LOOP_INVALID;
  cli_engine_bundler.stopping = false;
  /* logger may legitimately be NULL here. The fallback-logger allocation in
   * _client_engine_acquire can fail, and that function then reaps the engine
   * through this same reaper with no logger installed (see its comment).
   * _clog_write has no NULL-handle guard of its own, so an unguarded log
   * call here would crash instead of skipping the diagnostics. Every other
   * clog_info call site in this file has the same guard, as `if (el)` or
   * `if (logger)`. */
  if (cli_engine_bundler.logger)
    clog_info(cli_engine_bundler.logger,
              "The http client reactor engine has been destroyed");
  /* Only the fallback logger belongs to this run of the engine; the
   * caller's logger stays in user_logger for the next start. */
  clog old_logger = cli_engine_bundler.logger != cli_engine_bundler.user_logger
                        ? cli_engine_bundler.logger
                        : CLOG_INVALID;
  cli_engine_bundler.logger = CLOG_INVALID;
  _ccol_cond_var_broadcast(cli_engine_bundler.stopped_cv);
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
  if (old_logger) clog_close(old_logger);
#ifdef RUNNING_UNIT_TESTS
  _reaper_tail_hold_for_tests();
#endif
  return NULL;
}

/* Retry knobs for _client_engine_spawn_reaper: how many extra attempts it
 * makes and how long it pauses between two of them. After that it gives up
 * on getting a thread.
 *
 * pthread_create can report EAGAIN under a process or thread ceiling such
 * as RLIMIT_NPROC or a cgroup pids.max. That condition is ordinary and short
 * lived, it clears as soon as any other thread of the process exits, so a
 * few short retries turn most cases into a normal reap. We keep the total
 * pause small on purpose because this may run on a reactor dispatch thread,
 * and the fallback below, which abandons the reap, is correct on its own. */
#define CHTTP_REAPER_SPAWN_RETRIES 5
#define CHTTP_REAPER_SPAWN_RETRY_NS (1000L * 1000L) /* 1 ms */

#ifdef RUNNING_UNIT_TESTS
/* While this flag is true, _client_engine_spawn_reaper skips every
 * _ccol_thread_create attempt, counts each as a failure and creates no
 * thread. That lets a test drive the give-up path below deterministically,
 * because the real call almost never fails on an idle machine (a real
 * EAGAIN needs a tight `ulimit -u` or a similar cgroup ceiling). The test
 * resets the flag itself, and
 * _chttpclient_engine_reaper_abandoned_count_for_tests reports how many
 * times we took that path. */
static _Atomic bool g_force_reaper_spawn_fail_for_tests = false;
static _Atomic unsigned g_reaper_abandoned_count_for_tests = 0;

/* -1 until a shutdown site for an abandoned ctx has run at all. From then
 * on it is 1 if the shutdown ran with the ctx's idle_lock held, and 0 if it
 * ran after the unlock.
 *
 * Only 1 is safe. Once idle_lock is released, a reactor dispatch parked in
 * _async_dispatch_kind runs the real teardown, which close()s ctx->fd, and
 * a shutdown after that names an fd number the kernel may already have
 * handed to an unrelated socket of this process.
 *
 * The answer comes from a marker that the lock keeps up to date on every
 * lock and unlock (chttp_async_ctx_t.idle_lock_held_for_tests), so the
 * shutdown site never makes a claim about its own position. */
static _Atomic int g_abandon_shutdown_under_lock_for_tests = -1;

/* While g_reaper_spawn_hold_for_tests is true, the next reaper that
 * _client_engine_spawn_reaper creates is held between its creation and the
 * publishing of its handle. The spawning thread sets
 * g_reaper_spawn_held_for_tests and waits, ten seconds at most, for a test
 * to set g_reaper_spawn_gate_for_tests. It fires once, so a test can start
 * a second spawn inside the window of the first. */
static _Atomic bool g_reaper_spawn_hold_for_tests = false;
static _Atomic bool g_reaper_spawn_held_for_tests = false;
static _Atomic bool g_reaper_spawn_gate_for_tests = false;

static void _reaper_spawn_hook_for_tests(void) {
  if (!atomic_exchange(&g_reaper_spawn_hold_for_tests, false)) return;
  atomic_store(&g_reaper_spawn_held_for_tests, true);
  for (int i = 0; i < 10000 && !atomic_load(&g_reaper_spawn_gate_for_tests);
       ++i) {
    struct timespec ts = {0, 1000000L};
    nanosleep(&ts, NULL);
  }
}
#endif

/*
 * Gives the engine teardown to a dedicated thread. It must never run on the
 * thread that dropped the last reference, which is often a reactor dispatch
 * thread or a dns_pool worker. _client_engine_reaper_fn joins the deadline
 * sweep and then destroys the reactor and the dns_pool, so on such a thread
 * the event loop join becomes a self-join, and cthreadpool also spots a
 * worker calling ctpool_destroy on its own pool and stops the process with
 * _ccol_fatal_err(). Running the teardown inline because no thread was free
 * would turn a short resource shortage into an abort.
 *
 * If no thread can be had even after the retries above, we leave the
 * engine running. We clear `stopping` so the reference count can grow
 * again, and set g_engine_reap_abandoned, and the engine's deadline sweep
 * then retries the teardown on every tick, through
 * _client_engine_retry_abandoned_reap, until a thread is free. A release
 * that brings the count back to zero in the meantime goes through this
 * whole sequence on its own.
 *
 * What we pay is an engine that stays up while the process has no thread
 * to spare, and the inline teardown would be a crash.
 */
static void _client_engine_spawn_reaper(void) {
  _ccol_thread_id_t reaper;
  int rc = -1;
  for (int attempt = 0; attempt <= CHTTP_REAPER_SPAWN_RETRIES; attempt++) {
    if (attempt > 0) {
      struct timespec pause = {.tv_sec = 0,
                               .tv_nsec = CHTTP_REAPER_SPAWN_RETRY_NS};
      nanosleep(&pause, NULL);
    }
#ifdef RUNNING_UNIT_TESTS
    if (atomic_load(&g_force_reaper_spawn_fail_for_tests)) {
      rc = -1;
      continue;
    }
#endif
    /* The handle gets published in the same critical section that creates
     * the thread. The reaper takes this mutex before doing anything and
     * clears `stopping` under it, and a second spawn needs an acquire, which
     * waits for `stopping` to clear, so no second spawn can come before the
     * handle is published. Publishing after the unlock could land after the
     * reaper finished and after a second spawn published its own handle,
     * and we'd overwrite a handle nobody joined. */
    _ccol_mutex_lock(cli_engine_bundler.mutex);
    rc = _ccol_thread_create(reaper, _client_engine_reaper_fn, NULL);
    if (rc == 0) {
#ifdef RUNNING_UNIT_TESTS
      atomic_fetch_add(&g_reaper_created_for_tests, 1u);
      _reaper_spawn_hook_for_tests();
#endif
      /* A reaper that is joinable at this point has already finished: a
       * spawn needs `stopping` clear, and only the end of a reaper clears
       * it. So the join never blocks, and it keeps us from losing the
       * handle. */
      _client_engine_join_reaper_if_needed_locked();
      cli_engine_bundler.reaper_thread = reaper;
      cli_engine_bundler.reaper_joinable = true;
      atomic_store(&g_engine_reap_abandoned, false);
    }
    _ccol_mutex_unlock(cli_engine_bundler.mutex);
    if (rc == 0) break;
  }
  if (rc != 0) {
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add(&g_reaper_abandoned_count_for_tests, 1u);
#endif
    _ccol_mutex_lock(cli_engine_bundler.mutex);
    cli_engine_bundler.stopping = false;
    bool first_give_up = !atomic_exchange(&g_engine_reap_abandoned, true);
    _ccol_cond_var_broadcast(cli_engine_bundler.stopped_cv);
    _ccol_mutex_unlock(cli_engine_bundler.mutex);
    if (first_give_up)
      _CLIENT_ENGINE_LOG_INFO(
          "The http client reactor engine could not be reaped (no thread "
          "available); the deadline sweep retries the teardown");
    return;
  }
}

/* Retries the teardown of an engine that _client_engine_spawn_reaper left
 * running with no reference. Only the deadline sweep calls it, once a tick.
 * The sweep is an engine thread that runs no dispatch and holds no lock
 * here, and the reaper spawned from here joins it, a join that only waits
 * for the sweep to get back to its loop and see its stop flag. A stranded
 * engine goes down as soon as a thread is free, instead of waiting for an
 * acquire and a release that may never come. */
static void _client_engine_retry_abandoned_reap(void) {
  if (!atomic_load_explicit(&g_engine_reap_abandoned, memory_order_relaxed))
    return;
  bool reap = false;
  _ccol_mutex_lock(cli_engine_bundler.mutex);
  if (atomic_load(&g_engine_reap_abandoned) && cli_engine_bundler.reactor &&
      cli_engine_bundler.reactor_refs == 0 && !cli_engine_bundler.stopping) {
    cli_engine_bundler.stopping = true;
    reap = true;
  }
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
  if (reap) _client_engine_spawn_reaper();
}

/*
 * Takes a reference to chttpclient's reactor. The first call creates the
 * reactor, creates cli_engine_bundler.dns_pool and starts the deadline
 * sweep; every later call just adds one to cli_engine_bundler.reactor_refs.
 * Each call MUST be matched by exactly one _client_engine_release() call.
 */
static ccol_retval_t _client_engine_acquire_impl(bool for_chain) {
  _ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  /* The exit hook is registered after OpenSSL registers its cleanup,
   * so that the hook runs first. See _client_at_exit. */
  _ccol_call_once(cli_engine_bundler.exit_hook_once, _client_exit_hook_install);
#if CCOL_FORK_SAFETY_REQUIRED
  _ccol_call_once(cli_engine_bundler.atfork_once, _client_atfork_install);
#endif
  _ccol_mutex_lock(cli_engine_bundler.mutex);
  _client_engine_forget_inherited_locked();
  while (cli_engine_bundler.stopping)
    _ccol_cond_var_wait(cli_engine_bundler.stopped_cv,
                        cli_engine_bundler.mutex);
  _client_engine_join_reaper_if_needed_locked();

  if (!cli_engine_bundler.reactor) {
    atomic_store(&g_client_engine_pid, getpid());
    size_t nthreads = cli_engine_bundler.num_reactor_threads;
    if (nthreads == 0) {
      long cpus = sysconf(_SC_NPROCESSORS_ONLN);
      nthreads = (cpus > 0) ? (size_t)cpus : 1;
    }
    cli_engine_bundler.last_resolved_num_reactor_threads = nthreads;
    char *err = NULL;
    cli_engine_bundler.reactor = ccol_event_loop_create_mp(
        256, 4, nthreads, cli_engine_bundler.mprocs, &err);
    if (!cli_engine_bundler.reactor) {
      _ccol_mutex_unlock(cli_engine_bundler.mutex);
      return ccol_not_enough_memory;
    }

    char *dns_err = NULL;
    cli_engine_bundler.dns_pool =
        ctpool_create_mp(nthreads, 0, cli_engine_bundler.mprocs, &dns_err);
    if (!cli_engine_bundler.dns_pool) {
      /* We copy the reactor handle out and clear the field BEFORE unlocking,
       * so a concurrent _client_engine_acquire finds a clean "nothing built
       * yet" state and builds the engine itself. Otherwise it would see a
       * reactor field that looks live while this thread is busy joining
       * every reactor thread.
       *
       * The blocking destroy comes AFTER the unlock because, at this exact
       * point, nobody else needs the mutex to make progress: the new reactor
       * has no registrations and the deadline sweep thread isn't running
       * yet. Holding the mutex across the destroy would buy no correctness
       * and would only block every thread that wants to acquire, release or
       * reconfigure the engine for the whole join. */
      ccol_event_loop stale_reactor = cli_engine_bundler.reactor;
      cli_engine_bundler.reactor = CCOL_EVENT_LOOP_INVALID;
      _ccol_mutex_unlock(cli_engine_bundler.mutex);
      __ccol_event_loop_destroy(stale_reactor);
      return ccol_not_enough_memory;
    }

    if (_client_deadline_sweep_start() != ccol_success) {
      /* Same reasoning as the failed dns_pool branch just above: clear both
       * fields under the mutex, then unlock before the two blocking
       * destroys. The sweep thread never started here, so nothing can depend
       * on either handle yet. The failed-logger branch below is another
       * story, because there a sweep thread is already running. */
      ctpool stale_dns_pool = cli_engine_bundler.dns_pool;
      ccol_event_loop stale_reactor = cli_engine_bundler.reactor;
      cli_engine_bundler.dns_pool = CTPOOL_INVALID;
      cli_engine_bundler.reactor = CCOL_EVENT_LOOP_INVALID;
      _ccol_mutex_unlock(cli_engine_bundler.mutex);
      __ctpool_destroy(stale_dns_pool);
      __ccol_event_loop_destroy(stale_reactor);
      return ccol_unexpected_failure;
    }

    if (!cli_engine_bundler.logger && cli_engine_bundler.user_logger)
      cli_engine_bundler.logger = cli_engine_bundler.user_logger;
    if (!cli_engine_bundler.logger) {
      cli_engine_bundler.logger = clog_create_from_fd_mp(
          2, CLOG_FATAL, NULL, cli_engine_bundler.mprocs);
      if (!cli_engine_bundler.logger) {
        /* Unlike the three rollback branches above (failed reactor,
         * dns_pool or sweep), the sweep thread is ALREADY running here, so
         * we can't destroy everything inline the same way.
         *
         * The sweep thread logs with _CLIENT_ENGINE_LOG_INFO, which takes
         * cli_engine_bundler.mutex. Joining it while we hold that mutex is a
         * deadlock, since its current tick may be blocked on exactly this
         * lock.
         *
         * Unlocking first and joining afterwards isn't safe either. In that
         * window a concurrent _client_engine_acquire sees a non-NULL
         * cli_engine_bundler.reactor, skips the whole "create everything"
         * branch, and hands out a live reference to a reactor we are about
         * to destroy.
         *
         * So we do what _client_engine_release() does when the last
         * reference drops and the teardown must not block the caller: mark
         * the bundle as stopping, so a concurrent acquirer blocks in the wait
         * loop at the top instead of going on, and give the teardown to a new
         * reaper thread. The effect is the same as this acquire taking a
         * reference and dropping it at once, although it never grants one.
         *
         * Without this, a failed logger allocation leaks the reactor, the
         * dns_pool and the sweep thread for the rest of the process's life:
         * reactor_refs never leaves 0, so nothing ever calls
         * _client_engine_release() to reap them. */
        cli_engine_bundler.stopping = true;
        _ccol_mutex_unlock(cli_engine_bundler.mutex);
        _client_engine_spawn_reaper();
        return ccol_not_enough_memory;
      }
      clog_set_field(cli_engine_bundler.logger, "component",
                     "http-client-engine");
    }

    clog_info(cli_engine_bundler.logger,
              "New http client reactor engine has been created");
  }
  cli_engine_bundler.reactor_refs++;
  if (for_chain) {
    cli_engine_bundler.chains_live++;
    atomic_fetch_add_explicit(&g_async_requests_pending, 1,
                              memory_order_relaxed);
    /* An exit hook that is waiting gives up once a request is in flight. */
    if (cli_engine_bundler.exit_waiting)
      _ccol_cond_var_broadcast(cli_engine_bundler.stopped_cv);
  }
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
  return ccol_success;
}

static ccol_retval_t _client_engine_acquire(void) {
  return _client_engine_acquire_impl(false);
}

/* Takes the single engine reference of an async chain. The reference is
 * counted in chains_live, and the request it serves is counted in
 * g_async_requests_pending until its future gets fulfilled. Every success
 * pairs with exactly one _client_engine_release_chain(). */
static ccol_retval_t _client_engine_acquire_chain(void) {
  return _client_engine_acquire_impl(true);
}

/* Takes an async request off the pending count when its future is about
 * to be fulfilled. It runs once per request, ahead of the fulfilment, so a
 * caller that sees the future ready also sees the count without that
 * request. */
static inline void _client_async_request_settled(void) {
  atomic_fetch_sub_explicit(&g_async_requests_pending, 1, memory_order_release);
}

/*
 * Drops a reference that _client_engine_acquire() took. Once
 * cli_engine_bundler.reactor_refs is back at zero, the teardown goes to a
 * new reaper thread instead of running inline.
 *
 * It's done that way because a reactor callback thread often calls
 * this itself (_async_on_error tearing down the last live ctx is one such
 * caller), and that thread must never block joining the deadline sweep or
 * draining cli_engine_bundler.dns_pool. Doing either inline from a reactor
 * callback thread really hangs.
 */
static void _client_engine_release_impl(bool for_chain) {
  bool should_reap = false;
  _ccol_mutex_lock(cli_engine_bundler.mutex);
  if (for_chain && cli_engine_bundler.chains_live > 0 &&
      --cli_engine_bundler.chains_live == 0 && cli_engine_bundler.exit_waiting)
    _ccol_cond_var_broadcast(cli_engine_bundler.stopped_cv);
  if (cli_engine_bundler.reactor_refs > 0) cli_engine_bundler.reactor_refs--;
  if (cli_engine_bundler.reactor_refs == 0 && cli_engine_bundler.reactor) {
    cli_engine_bundler.stopping = true;
    should_reap = true;
  }
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
  if (should_reap) _client_engine_spawn_reaper();
}

static void _client_engine_release(void) { _client_engine_release_impl(false); }

/* Drops the reference that _client_engine_acquire_chain took. */
static void _client_engine_release_chain(void) {
  _client_engine_release_impl(true);
}

/*
 * Blocks until an in-flight reaper thread has finished tearing down this
 * module's resources (see _client_engine_release). When the engine isn't
 * stopping it returns at once, which covers an engine that isn't running at
 * all and one that stays up because other Tier 2 or Tier 3 references are
 * live.
 *
 * Unlike chttpcli_engine_wait, it won't wait for an engine that has
 * references to stop. Only the tests call it, to wait for a teardown they
 * triggered themselves, so it sits behind RUNNING_UNIT_TESTS.
 */
#ifdef RUNNING_UNIT_TESTS
static void _client_engine_wait_for_quiescence(void) {
  _ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  _ccol_mutex_lock(cli_engine_bundler.mutex);
  _client_engine_forget_inherited_locked();
  while (cli_engine_bundler.stopping)
    _ccol_cond_var_wait(cli_engine_bundler.stopped_cv,
                        cli_engine_bundler.mutex);
  _client_engine_join_reaper_if_needed_locked();
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
}
#endif /* RUNNING_UNIT_TESTS */

void chttpcli_engine_wait(void) {
  if (_chttpcli_runs_on_engine_thread())
    _ccol_fatal_err(
        "chttpcli_engine_wait: called on a thread of the async engine (a "
        "write_fn of chttpclient_do_async_streaming or "
        "chttpclient_do_pooled_streaming), which would wait for its own "
        "thread; call it from another thread");
  _ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  _ccol_mutex_lock(cli_engine_bundler.mutex);
  _client_engine_forget_inherited_locked();
  /* A reaper that couldn't start leaves the reactor up with no reference
   * and no stopping flag. The deadline sweep starts the teardown again and
   * its reaper broadcasts when done, so this wait covers that case too. */
  while (cli_engine_bundler.reactor || cli_engine_bundler.stopping)
    _ccol_cond_var_wait(cli_engine_bundler.stopped_cv,
                        cli_engine_bundler.mutex);
  _client_engine_join_reaper_if_needed_locked();
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
}

/*
 * Runs once from the atexit(3) list when the process exits, and also when
 * this library is unloaded, where glibc runs the exit handlers a shared
 * object registered. It stops every thread we started for Tier 2 and Tier 3
 * before OpenSSL releases its global state. Otherwise the engine stops in
 * the background on a reaper thread once its last reference goes, and a
 * process returning from main right then runs OpenSSL's cleanup while a
 * reactor thread, a DNS worker or the reaper itself is freeing OpenSSL's
 * per-thread state, which is a double free. A reaper nobody joins is also a
 * thread that a leak checker reports.
 *
 * Getting this to run first is a matter of registration order.
 * _client_exit_hook_install calls OPENSSL_init_ssl() first, which registers
 * OpenSSL's cleanup, and registers this function after it. The atexit list
 * runs in reverse registration order, so we run first. A destructor of the
 * shared object would run after every atexit handler, which is too late.
 *
 * The steps, in order:
 *  - Nothing in a process that didn't build the engine, such as a fork()
 *    child that inherited a running engine without its threads.
 *  - Nothing while an async request is in flight. Its write_fn may be the
 *    very code that called exit(), on a thread the teardown would join, and
 *    a request in flight at exit is one the application abandons.
 *  - Wait for every chain whose future is already fulfilled to let go of
 *    the engine, which the reactor does at once.
 *  - Destroy the default client when no Tier 1 call runs on it. Its Tier 2
 *    idle connections hold engine references, and its TLS state must go
 *    before OpenSSL does.
 *  - Wait for a teardown in progress, join the reaper, and on this thread
 *    tear down an engine that no reference holds any more.
 * An engine held through the idle connections of a client that the
 * application never destroyed keeps running, together with its threads. */
static void _client_at_exit(void) {
  if (atomic_load(&g_client_engine_pid) != getpid()) return;
  if (atomic_load_explicit(&g_async_requests_pending, memory_order_acquire) ||
      atomic_load_explicit(&g_async_hops_abandoned, memory_order_acquire))
    return;
  _ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);

  _ccol_mutex_lock(cli_engine_bundler.mutex);
  cli_engine_bundler.exit_waiting = true;
  while (cli_engine_bundler.chains_live > 0 &&
         atomic_load(&g_async_requests_pending) == 0 &&
         atomic_load(&g_async_hops_abandoned) == 0)
    _ccol_cond_var_wait(cli_engine_bundler.stopped_cv,
                        cli_engine_bundler.mutex);
  cli_engine_bundler.exit_waiting = false;
  bool in_flight = cli_engine_bundler.chains_live > 0;
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
  if (in_flight) return;

  _default_client_destroy_if_idle();

  _ccol_mutex_lock(cli_engine_bundler.mutex);
  while (cli_engine_bundler.stopping)
    _ccol_cond_var_wait(cli_engine_bundler.stopped_cv,
                        cli_engine_bundler.mutex);
  _client_engine_join_reaper_if_needed_locked();
  bool reap_here =
      cli_engine_bundler.reactor && cli_engine_bundler.reactor_refs == 0;
  if (reap_here) {
    cli_engine_bundler.stopping = true;
    atomic_store(&g_engine_reap_abandoned, false);
  }
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
  /* No reference and no reaper means the reaper couldn't be created. We are
   * not on an engine thread here, so we run the teardown ourselves. */
  if (reap_here) (void)_client_engine_reaper_fn(NULL);
}

static void _client_exit_hook_install(void) {
  atomic_store(&g_client_engine_pid, getpid());
  /* The first OPENSSL_init_ssl call registers OpenSSL's cleanup with
   * atexit(3); any later call just reports success. */
  (void)OPENSSL_init_ssl(0, NULL);
  (void)atexit(_client_at_exit);
}

ccol_retval_t chttpcli_set_engine_logger(clog cl) {
  if (!cl) {
    /* A release. The engine logs through this logger while it runs, and so
     * does a reaper in the middle of tearing it down, so we wait for a full
     * stop. After that nothing reads user_logger until the next start, and
     * that start opens the fallback logger. */
    _ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
    _ccol_mutex_lock(cli_engine_bundler.mutex);
    _client_engine_forget_inherited_locked();
    if (cli_engine_bundler.reactor || cli_engine_bundler.stopping) {
      _ccol_mutex_unlock(cli_engine_bundler.mutex);
      return ccol_not_permitted;
    }
    clog old_user = cli_engine_bundler.user_logger;
    cli_engine_bundler.user_logger = CLOG_INVALID;
    _ccol_mutex_unlock(cli_engine_bundler.mutex);
    if (old_user) clog_close(old_user);
    return ccol_success;
  }
  /* A closed or stale handle is the caller's mistake, which we report; it
   * is no reason to end the process. */
  ccol_retval_t derive_rv = ccol_success;
  clog derived = _clog_derive_checked(cl, &derive_rv);
  if (!derived) return derive_rv;
  clog_set_field(derived, "component", "http-client-engine");
  _ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  _ccol_mutex_lock(cli_engine_bundler.mutex);
  /* Whatever the engine logs through right now is either the previous
   * user_logger or a fallback of this engine run, and both get closed here.
   * We only replace a fallback while the engine runs, since that's the only
   * time one is open. */
  clog old_user = cli_engine_bundler.user_logger;
  clog old_active = cli_engine_bundler.logger;
  cli_engine_bundler.user_logger = derived;
  if (old_active) cli_engine_bundler.logger = derived;
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
  if (old_active && old_active != old_user) clog_close(old_active);
  if (old_user) clog_close(old_user);
  return ccol_success;
}

/* Closes the logger that chttpcli_set_engine_logger installed, when this
 * module unloads or the process exits, but only if the engine is fully
 * stopped. A running engine keeps logging through it, so in that case it
 * stays open like any other handle in use at exit. Ordering against
 * clogger's destructor doesn't matter, since whichever of the two runs last
 * releases clogger's slot table. */
__attribute__((destructor)) static void _cleanup_engine_user_logger(void) {
  /* If the engine globals were never initialized, no logger was installed
   * either. Initializing them here would create a mutex and a condition
   * variable that nothing destroys (on some systems each is a heap block)
   * in every program that loads the library, so we just return. */
  if (!atomic_load(&g_engine_globals_entered)) return;
  _ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  _ccol_mutex_lock(cli_engine_bundler.mutex);
  clog user = CLOG_INVALID;
  if (!cli_engine_bundler.reactor && !cli_engine_bundler.stopping &&
      cli_engine_bundler.logger != cli_engine_bundler.user_logger) {
    user = cli_engine_bundler.user_logger;
    cli_engine_bundler.user_logger = CLOG_INVALID;
  }
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
  if (user) clog_close(user);
}

ccol_retval_t chttpcli_set_engine_mem_mgmt_procs(ccol_memmgmt_procs_t *mp) {
  if (mp && (!mp->malloc || !mp->free || !mp->calloc || !mp->realloc))
    return ccol_invalid_args;
  _ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  _ccol_mutex_lock(cli_engine_bundler.mutex);
  _client_engine_forget_inherited_locked();
  if (cli_engine_bundler.reactor) {
    _ccol_mutex_unlock(cli_engine_bundler.mutex);
    return ccol_not_permitted;
  }
  if (mp) {
    cli_engine_bundler.mprocs_storage = *mp;
    cli_engine_bundler.mprocs = &cli_engine_bundler.mprocs_storage;
  } else {
    cli_engine_bundler.mprocs = NULL;
  }
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
  return ccol_success;
}

ccol_retval_t chttpcli_set_engine_num_reactor_threads(size_t num_threads) {
  _ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  _ccol_mutex_lock(cli_engine_bundler.mutex);
  _client_engine_forget_inherited_locked();
  if (cli_engine_bundler.reactor) {
    _ccol_mutex_unlock(cli_engine_bundler.mutex);
    return ccol_not_permitted;
  }
  cli_engine_bundler.num_reactor_threads = num_threads;
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
  return ccol_success;
}

/* ========================================================================== */
/*                    ASYNC CONNECTION STATE MACHINE (STEP A + B)             */
/* ========================================================================== */

/*
 * The engine behind Tier 2 and Tier 3. For each connection it runs one
 * non-blocking HTTP or HTTPS request and response cycle, and redirect
 * support sits on top of that single-hop pipeline. chttpclient's
 * ccol_event_loop reactor drives it (see the "SHARED STATIC REACTOR"
 * section above).
 *
 * TLS: we use the same reactor-agnostic client-mode ctls API as Tier 1
 * (ctls_conn_create_client, ctls_conn_handshake_step, ctls_conn_read and
 * ctls_conn_write), except that the on_readable and on_writable callbacks
 * drive it here instead of a blocking poll() loop.
 *
 * There is no tls_lock here because ccol_event_loop keeps a dispatch_lock
 * for each event_entry (see the cthreadcomm documentation). That lock makes
 * sure one registration's callback is never called concurrently with
 * itself, and it goes further: both directions of one fd, on_readable and
 * on_writable, are never dispatched concurrently with each other either.
 * That's what lets this module drive OpenSSL on one shared connection from
 * either callback without a lock of its own.
 *
 * Re-arming write readiness needs no special handling, because
 * ccol_event_loop is level-triggered and a registration stays live until it
 * is explicitly removed. We call ccol_event_loop_modify instead, and only
 * to flip the directions we're interested in, so there is no window where a
 * one-shot write interest could be forgotten, and that holds for a raw TLS
 * write that goes around the queue as well.
 *
 * A connection is identified by its raw fd plus the ccol_event_reg handle
 * that ccol_event_loop_add returned for its current registration. The
 * handle is CCOL_EVENT_REG_INVALID while no registration is active, for
 * example for a connection in the middle of its connect in the DNS and
 * connect pool, or for an idle pooled one. ccol_event_loop has its own
 * generation counter too, ccol_event_loop_reg_generation, which we could use
 * for defensive bookkeeping, but correctness doesn't depend on it: the
 * dispatch path of ccol_event_loop checks again under its own lock whether
 * a registration is live, so a stale entry from an epoll_wait batch already
 * fetched, for a registration we removed, is always a harmless no-op.
 *
 * ccol_event_loop_remove only protects ccol_event_loop's own internal
 * structures, through deferred, epoch-based frees. It does NOT protect this
 * module's ctx payload from a dispatch that was already in flight when the
 * remove happened. So every place that ends a connection goes through one
 * of two small explicit teardown helpers, _async_ctx_finish and
 * _async_idle_ctx_finish, defined after the idle pool section below.
 * Nothing relies on an implicit "on_close runs at some point" guarantee:
 * ccol_event_loop has no unconditional terminal callback, so this module
 * has to end a connection explicitly. A new way of ending a connection must
 * go through one of these two helpers as well; one that frees ctx directly
 * races every dispatch still in flight.
 *
 * Lifetime of a redirect chain: a chain spans more than one connection,
 * with one chttp_async_ctx_t per hop, matching the "a fresh
 * chttp_parse_ctx_t per hop" comment of Tier 1. All the same, the chain must
 * fulfil the caller's future exactly once, and free exactly one engine
 * reference for the whole chain.
 *
 * That shared state lives in chttp_async_chain_t, a small heap struct that
 * outlives the ctx of any single hop. It holds the future and the
 * fulfilled-once guard, an owned deep copy of the original request's
 * headers and body, the pinned TLS context (constant across hops, exactly
 * like Tier 1's tls_ctx local), and a refcount of how many hop ctx are
 * live right now.
 *
 * Every ctx teardown path drops one chain reference, and when the count
 * hits zero, _async_chain_release frees the chain. As a backstop, guarded
 * by the fulfilled-once flag, it also fulfils the future with a generic
 * error if nothing else got there first.
 */

/* The ctpool_future's result is a chttpcli_async_result_t, a public type
 * declared in chttpclient.h, and _async_fulfill_chain below builds it. */

typedef enum {
  CHTTP_ASYNC_CONNECTING,
  CHTTP_ASYNC_TLS_HANDSHAKING,
  CHTTP_ASYNC_WRITING,
  CHTTP_ASYNC_AWAITING_CONTINUE, /* Only for "Expect: 100-continue". The
                                  * whole header block is written and this
                                  * ctx is registered for READ, waiting up
                                  * to ctx->continue_deadline for one of
                                  * three things: an interim "100 Continue"
                                  * makes it write the body, a direct final
                                  * response means the body is never sent,
                                  * and the deadline sweep may flip the
                                  * registration to WRITE as a timeout
                                  * signal, which sends the body anyway. The
                                  * deadline only bounds the wait for the
                                  * first byte of a message; a message that
                                  * started before it is read to its end.
                                  * The field comments of
                                  * ctx->want_100_continue,
                                  * ctx->continue_decided and
                                  * ctx->continue_msg_started have the full
                                  * design. */
  CHTTP_ASYNC_READING,
  CHTTP_ASYNC_IDLE, /* The ctx sits in cli->idle_pools_async and no chain
                     * owns it, so ctx->chain is NULL. See the "ASYNC IDLE
                     * POOL" section below. */
} chttp_async_state_t;

/*
 * Shared state of a redirect chain: one per request, not one per hop. The
 * comment above explains the ownership and the lifetime.
 */
typedef struct {
  ccol_memmgmt_procs_t *mp;
  struct chttpclient *cli; /* The client that owns this chain.
                            * _async_submit_hop uses it to get at the async
                            * idle pool, cli->idle_pools_async, both to take
                            * a reusable connection out and to offer one
                            * back. */
  ctpool_future *future;   /* The caller has its own, separate reference
                            * (see what _chttp_do_async_internal returns).
                            * We hold the producer-side one and only ever
                            * call ctpool_future_fulfill on it. */

  chttpcli_write_fn write_fn; /* NULL for a buffered request from
                               * chttpclient_do_async, non-NULL for a
                               * streaming one from
                               * chttpclient_do_async_streaming.
                               * _async_submit_hop and _async_retry_hop plug
                               * it into ctx->pctx.requested_sink_fn of every
                               * hop. It stays the same for the whole chain,
                               * like the streaming and write_fn locals of
                               * Tier 1 in chttp_do_internal, and we call it
                               * on the reactor thread that drives this
                               * hop's readable dispatch. The public doc of
                               * this field on chttpclient_do_async_streaming
                               * has the contract that the function must not
                               * block. */
  void *write_ctx;            /* Handed to write_fn exactly as it is. */
  _Atomic bool sink_closed;   /* The deadline sweep sets it before fulfilling
                               * the future of a streaming request whose hop
                               * is alive, and from then on _sink_async_user
                               * refuses every byte. */
  _Atomic bool in_sink;       /* True while _sink_async_user is inside
                               * write_fn. Together with sink_closed it makes
                               * a store-then-load pair on each side, so
                               * either the sink sees the close or the sweep
                               * sees the call and waits for it. */

  chmap req_headers; /* Our own deep copy of the original request's headers,
                      * a chmap(char* -> char*) that we send again unchanged
                      * on every hop, like hop_req.headers in Tier 1. NULL
                      * when the original request had no headers. */
  void *body_data;   /* Our own deep copy of the original request's body
                      * bytes, or NULL when it had no body. A 307 or 308 hop
                      * sends it again exactly as it is, while a redirect
                      * that doesn't preserve the method (a 301, 302 or 303
                      * with a method other than HEAD) drops it. */
  size_t body_len;
  char *body_content_type; /* Our own copy, or NULL if there isn't one. */
  bool body_dropped;       /* Goes to true for good the first time a
                            * redirect that doesn't preserve the method
                            * drops the body.
                            *
                            * Tier 1's chttp_do_internal tracks the same
                            * thing differently: after a drop it overwrites
                            * its own cur_body loop local with an empty
                            * body, so a LATER 307 or 308 on the same chain
                            * keeps the current body (which may already be
                            * empty) and not the original one.
                            *
                            * _async_handle_redirect has nothing like that
                            * cur_body local per ctx, since a
                            * chttp_async_ctx_t only exists for the hop in
                            * flight, so this chain-level flag is what
                            * carries "once dropped, stays dropped" across
                            * hops here.
                            *
                            * Without it, a 307 or 308 hop after an earlier
                            * downgrade would bring back body_data, body_len
                            * and body_content_type from the ORIGINAL hop-0
                            * values in this struct, instead of the empty
                            * body the chain had moved to.
                            *
                            * Only _async_handle_redirect writes this field.
                            * It always runs synchronously on the thread
                            * that has just parsed a hop's response, and
                            * that thread calls _async_submit_hop for the
                            * next hop from the same call stack. So no lock
                            * is needed, for the same reason as for
                            * carried_auth and redirect_drops below (see
                            * the comments on those fields in this
                            * struct). */

  char *carried_auth;        /* The Authorization value we inject from the
                              * URL's userinfo, carried forward across hops
                              * just like the carried_auth local of Tier 1
                              * in chttp_do_internal (whose comment explains the
                              * contract: carried on the same origin,
                              * dropped for good across origins). NULL when
                              * the chain has seen no userinfo at all.
                              *
                              * Only _async_submit_hop writes it, at the same
                              * point where the method and body downgrade
                              * decision already writes this chain without a
                              * lock (that function's comment explains why
                              * no lock is needed). */
  char *carried_auth_origin; /* The origin_key the field above was derived
                              * for. */

  char *initial_origin_key; /* The origin_key of hop 0, the request as the
                             * caller submitted it.
                             * _chttp_do_async_internal captures it once,
                             * before the chain queues its first hop, and we
                             * only use it to detect a cross-origin redirect
                             * for the check below. It never changes during
                             * the chain's lifetime, while
                             * carried_auth_origin does. */
  unsigned redirect_drops;  /* CHTTP_REDIRECT_DROP_* flags of the hop being
                             * prepared. _async_submit_hop computes them
                             * again for every hop against
                             * initial_origin_key, like the redirect_drops
                             * local of chttp_do_internal in Tier 1, and
                             * CHTTP_REDIRECT_DROP_CREDENTIALS lists the
                             * headers each flag removes.
                             *
                             * Only _async_submit_hop writes it, at the same
                             * point where carried_auth and
                             * carried_auth_origin get written without a
                             * lock (see the comment of that field for why
                             * that is fine). */

  ctls_ctx_t *tls_ctx; /* Pinned once for the whole chain with a
                        * ctls_ctx_retain on cli->tls_ctx, like the tls_ctx
                        * local of Tier 1. A redirect may hop between http
                        * and https, so the context must outlive every
                        * hop, whatever scheme the chain started with. It
                        * gets one ctls_ctx_release when the chain is
                        * freed. */
  bool tls_ctx_usable;
  bool verify_host;
  /* cli->tls_generation as it was when we pinned tls_ctx above, so the two
   * describe one configuration for this chain's whole lifetime. Every ctx
   * this chain creates gets stamped with it, and we compare it with the
   * client's current value before reusing a pooled connection (see the
   * field comment of chttpclient.tls_generation). */
  uint64_t tls_generation;

  uint64_t connect_timeout_us;       /* Read again into
                                      * ctx->connect_deadline at the start of
                                      * every hop that really connects. The
                                      * reuse path never reads it, because
                                      * it skips CONNECTING and
                                      * TLS_HANDSHAKING (see the "ASYNC
                                      * DEADLINE SWEEP" section below). */
  chttp_deadline_t overall_deadline; /* Computed once, here, for the chain's
                                      * whole lifetime, like overall_dl in
                                      * Tier 1, which is computed once before
                                      * its hop loop and never reset per
                                      * hop. */
  size_t max_response_body_size;     /* Snapshot of
                                      * cli->max_response_body_size taken
                                      * once by _chttp_do_async_internal,
                                      * like connect_timeout_us and
                                      * overall_deadline above.
                                      * _async_submit_hop and
                                      * _async_retry_hop copy it into
                                      * ctx->bb.max_size of every hop, and
                                      * 0 means no limit. */
  /* Snapshot of prevent_tls_downgrade_on_redirect from the original
   * chttp_request_t. _chttp_do_async_internal takes it once, the same way
   * as expect_continue below, and every hop of the chain reads it. */
  bool prevent_tls_downgrade_on_redirect;
  bool expect_continue; /* Snapshot of the original chttp_request_t's
                         * expect_continue field, which
                         * _chttp_do_async_internal takes once, like
                         * connect_timeout_us and max_response_body_size
                         * above.
                         *
                         * _async_submit_hop decides per HOP whether that
                         * hop goes through the "Expect: 100-continue"
                         * wait, like the per-hop use_100_continue local of
                         * Tier 1, which splits a request-level flag from a
                         * per-hop decision the same way. A redirect can
                         * downgrade the method or drop the body, and then
                         * the wait makes no sense for a later hop, even
                         * though this flag never changes during the
                         * chain's lifetime. */

  _ccol_mutex_t lock; /* Guards fulfilled and refcount. Hop reactor
                       * callbacks change them and those callbacks can run
                       * concurrently (see the comment above this struct):
                       * an old hop's error or teardown dispatch may run at
                       * the same time as a new hop's readable or writable
                       * dispatch once the redirect handoff has queued the
                       * next connection. */
  bool fulfilled;
  /* The result that the single fulfilment of the future hands over. The
   * chain gets it ahead of time so that the fulfilment allocates nothing and
   * can never fall back to a NULL result, which would read as a cancelled
   * future. It goes NULL once the future owns it. */
  chttpcli_async_result_t *result;
  int refcount; /* How many live per-hop ctx point at this chain. It drops
                 * to zero exactly once, when the last hop is torn down and
                 * no other hop is queued to take over. */
} chttp_async_chain_t;

/* Sink of a Tier 2 or Tier 3 streaming request. It passes the bytes to the
 * caller's write_fn with a frame of the client pushed on the calling
 * thread, so a chttpclient_destroy of that client from inside write_fn gets
 * caught (see _chttpcli_call_frame_t). */
static void _async_fulfill_chain(chttp_async_chain_t *chain, ccol_retval_t rv,
                                 chttpcli_response *resp);

static size_t _sink_async_user(const void *data, size_t len, void *ctx) {
  chttp_async_chain_t *chain = (chttp_async_chain_t *)ctx;
  atomic_store(&chain->in_sink, true);
  if (atomic_load(&chain->sink_closed)) {
    /* The request timed out and the deadline sweep closed the sink, so it
     * will fulfil the future with ccol_timed_out. We fulfil first here when
     * the sweep is waiting for an earlier call to end; otherwise the short
     * count below, which ends the hop as aborted, would report the request
     * as aborted. */
    atomic_store(&chain->in_sink, false);
    _async_fulfill_chain(chain, ccol_timed_out, NULL);
    return 0;
  }
  _chttpcli_call_frame_t frame;
  _chttpcli_call_push(&frame, chain->cli, true);
  size_t n = chain->write_fn(data, len, chain->write_ctx);
  _chttpcli_call_pop(&frame);
  atomic_store(&chain->in_sink, false);
  return n;
}

/* Closes a streaming chain's sink and waits until no write_fn of the chain
 * is running, so the future can be fulfilled with no write_fn call coming
 * after it. */
static void _async_sink_close(chttp_async_chain_t *chain) {
  atomic_store(&chain->sink_closed, true);
  while (atomic_load(&chain->in_sink)) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
    nanosleep(&ts, NULL);
  }
}

/* See the "ASYNC HAPPY EYEBALLS" section. */
typedef struct chttp_connect_race_s chttp_connect_race_t;

/* The values of ctx->flip_watch. See _async_upload_pause. */
#define CHTTP_FLIP_NONE 0
/* The send blocked and waits for the write direction; if a response shows
 * up in the meantime, the registration turns to the read direction. */
#define CHTTP_FLIP_ON_INPUT 1
/* The send is paused in duplex mode and waits for the read direction; once
 * there is room to write, the registration turns to the write direction. */
#define CHTTP_FLIP_ON_ROOM 2

/* The struct needs a tag because deadline_prev and deadline_next below have
 * to point at the struct itself, and with an anonymous struct there's no
 * name to write such a pointer with. */
typedef struct chttp_async_ctx_s {
  ccol_memmgmt_procs_t *mp;
  struct chttpclient *cli; /* The client owning this ctx. While the ctx is
                            * idle, chain is NULL and this is how it gets to
                            * cli->idle_pools_async; while it is active, it
                            * needs the field to offer the connection back
                            * to the pool after a completion that leaves the
                            * connection reusable. */
  chttp_async_chain_t
      *_Atomic chain;        /* Shared state of the whole redirect chain,
                              * which the ctx doesn't own (see
                              * _async_ctx_teardown). NULL while the idle
                              * pool holds the ctx.
                              *
                              * Like state and fd below it is _Atomic, so the
                              * deadline sweep can read it without
                              * idle_lock; the comment of that lock and the
                              * "ASYNC DEADLINE SWEEP" section explain why
                              * single atomic loads are enough there. The
                              * stronger COMPOUND guarantee of idle_lock,
                              * covering state and chain together, is needed
                              * all the same, and this field doesn't disturb
                              * it at any idle_lock call site. */
  int hop;                   /* 0-based hop index of THIS connection. */
  chttp_method_t cur_method; /* The method we built THIS hop's wire bytes
                              * with. A redirect bases the next hop's method
                              * on it, like the cur_method loop variable of
                              * Tier 1. */
  _Atomic chttp_async_state_t state;
  _Atomic int fd; /* -1 until the connect task has a real fd. */
  _Atomic(ccol_event_reg)
      reg; /* The current ccol_event_loop registration, or
            * CCOL_EVENT_REG_INVALID while none is active, as for a
            * mid-connect ctx in the DNS and connect pool or an idle pooled
            * ctx that has no direction of interest yet.
            * _async_connect_task sets it exactly once, right after
            * ccol_event_loop_add returns.
            *
            * It's _Atomic because that assignment is NOT hidden
            * until a dispatch might care. The internal registration of
            * ccol_event_loop_add goes live inside the call itself and
            * another reactor thread can dispatch it from that moment, so
            * the call may finish and hand a callback to a DIFFERENT thread
            * before this thread's `ctx->reg = ccol_event_loop_add( ...)`
            * assignment completes. A loopback connect makes that very
            * likely, since it is often writable the moment we register it.
            *
            * Every dispatch callback reads this field without a lock, so a
            * plain non-atomic pointer here is a real data race and every
            * access must be atomic. ThreadSanitizer reports the non-atomic
            * form, while reading the code won't show it.
            *
            * Each dispatch callback gets its own registration handle from
            * ccol_event_loop_add and stores it here through _async_adopt_reg
            * before anything reads this field, so a dispatch that runs
            * before _async_connect_task's assignment completes sees the
            * right handle anyway. The adopted and the returned value are the
            * same handle, so the two stores never disagree. */
  _Atomic(ccol_event_reg)
      room_reg; /* A second, write-direction registration of the same fd.
                 * It only exists while a duplex send is paused on a full
                 * send buffer: reg watches for input and this one for room,
                 * so the send continues the moment there is room and a
                 * response is read at once. See
                 * _async_room_watch_start. It shares reg's dispatch lock,
                 * since both belong to one fd. */

  bool is_unix;
  char *unix_socket_path; /* Our own copy, NULL unless is_unix. */
  char *host;             /* Our own copy, or NULL when is_unix is true.
                           * port is passed separately. */
  uint16_t port;
  chttp_addr_list_t *addrs; /* Our own list of candidate addresses for a TCP
                             * target, which _async_connect_task resolves
                             * for the connect. A list with more than one
                             * address goes over to the race in ctx->race
                             * (see chttp_addr_list_t). NULL for a unix
                             * target, for a ctx that hasn't resolved yet,
                             * and once the connect has succeeded. */
  _Atomic(chttp_connect_race_t *)
      race;             /* This connect's Happy Eyeballs race, or NULL; see
                         * the "ASYNC HAPPY EYEBALLS" section. The connect
                         * task sets it before any attempt starts, and the ctx
                         * keeps one reference on it until the ctx is freed,
                         * which happens after it left the deadline registry,
                         * so the deadline sweep can always read it. */
  bool is_ipv6;         /* host is a raw IPv6 literal without brackets, as
                         * in chttp_url_t.is_ipv6.
                         * _async_handle_redirect needs it to put the
                         * brackets back when it rebuilds a redirect target.
                         * Without it the rebuild silently produces a
                         * malformed "scheme://<unbracketed-ipv6>:port/path"
                         * target, and the next hop fails to parse it. */
  char *path_and_query; /* Our own copy of THIS HOP's request path and
                         * query, such as "/a/b?x=1", as in
                         * chttp_url_t.path_and_query.
                         *
                         * _async_submit_hop captures it again on EVERY hop,
                         * a reused connection included, since the path may
                         * change from one request to the next while host,
                         * port and origin_key (so the idle pool origin)
                         * stay the same. _async_retry_hop carries it across
                         * a retry, exactly like host, unix_socket_path and
                         * origin_key.
                         *
                         * _async_handle_redirect needs it to resolve a
                         * Location header with a relative path against
                         * this hop's own URL, the way the per-hop
                         * chttp_url_t url local does in Tier 1.
                         *
                         * Without it, the chttp_url_t base that
                         * _async_handle_redirect builds by hand has this
                         * field NULL, and a really relative Location header
                         * (not an absolute path, not a full URL, and not
                         * protocol-relative) crashes the process with a
                         * NULL-pointer strchr() inside _merge_ref_path. */
  char *origin_key;     /* Our own copy, "scheme://host:port" or
                         * "unix://<path>", matching chttp_conn_t.origin_key
                         * of Tier 1. We use it to put this ctx into
                         * cli->idle_pools_async and to take it out. */
  bool reused;          /* True when this hop's connection came out of the
                         * idle pool instead of a fresh connect. It is about
                         * THIS attempt only, so a retry always sets it back
                         * to false (see _async_retry_hop). */
  bool any_bytes_read;  /* Goes true once we have read one or more response
                         * bytes for the CURRENT attempt. It gates the retry
                         * of a dead reused connection, just like Tier 1's
                         * any_bytes_read output parameter of the same
                         * name. */
  size_t interim_responses_seen; /* How many CONSECUTIVE interim 1xx
                                  * responses we dropped since the latest
                                  * message boundary that we didn't drop.
                                  *
                                  * It must survive between dispatch
                                  * callback calls, because the discard loop
                                  * in _async_on_readable_impl and in
                                  * _async_awaiting_continue_on_data may
                                  * return in the middle of a skip and wait
                                  * for more bytes in a later on_readable
                                  * dispatch. Tier 1's cap of the same kind
                                  * is just a stack loop counter in
                                  * _chttp_read_message_loop, because that
                                  * function blocks synchronously for the
                                  * whole loop.
                                  *
                                  * The comment of
                                  * CHTTP_MAX_INTERIM_RESPONSES explains why
                                  * there is a cap at all:
                                  * without it, a server that keeps sending
                                  * "103 Early Hints" forever can keep this
                                  * ctx alive forever, together with the
                                  * chain and the future waiting on it.
                                  *
                                  * We reset it to 0 on every fresh attempt,
                                  * a new connect or a retry through
                                  * _async_retry_hop. A connection reused
                                  * from the idle pool starts at 0 too,
                                  * because the reset block of
                                  * _async_idle_pool_offer zeroes every
                                  * per-attempt field before pooling the
                                  * connection.
                                  *
                                  * We ALSO reset it to 0 the moment
                                  * _async_awaiting_continue_on_data claims a
                                  * genuine "100 Continue", right before it
                                  * hands over to the read phase of the
                                  * final response (see the comment at that
                                  * call site). A confirmed "100 Continue"
                                  * is a real message that we don't drop, so
                                  * the run of consecutive drops counted
                                  * here starts over there.
                                  *
                                  * That matches _chttp_send_and_read of
                                  * Tier 1, which gives the continue wait
                                  * and the final response read to two
                                  * SEPARATE _chttp_read_message_loop calls,
                                  * each with a counter of its own.
                                  *
                                  * Without the reset, a hop that drops N
                                  * interim responses while waiting for
                                  * "100 Continue" would leave only 64 - N
                                  * for the final response's interim
                                  * responses instead of a full 64, and
                                  * that quietly breaks the documented
                                  * contract of the cap: 64 CONSECUTIVE
                                  * dropped interim responses, as
                                  * chttpclient_do_async(3) publishes. A
                                  * confirmed "100 Continue" ends the
                                  * consecutive run, so it must not count
                                  * against the responses after it. */
  struct timespec last_used;     /* Set when we offer the connection to the
                                  * idle pool, and the staleness check of
                                  * _async_idle_pool_take reads it. */
  bool upload_paused; /* A response began to arrive while the request was
                       * going out, so the send waits for the read path to
                       * decide whether it may go on. See
                       * _async_upload_pause. Only this ctx's dispatch
                       * reads and writes it. */
  _Atomic unsigned char
      flip_watch;         /* CHTTP_FLIP_*. The request send waits on one
                           * direction while the other one can matter too,
                           * and the deadline sweep keeps an eye on that
                           * other one; see _async_upload_pause. */
  bool answered_early;    /* A response message began while the send was
                           * paused and didn't turn out to be an interim
                           * one. Only this ctx's dispatch reads and writes
                           * it. See _async_request_sent. */
  bool duplex;            /* A final 200 to 299 response is streaming in
                           * while the request is going out. Only this ctx's
                           * dispatch reads and writes it. */
  size_t upload_check_at; /* The wire_sent value from which a successful
                           * write checks again for a response that came in
                           * meanwhile. See _async_upload_should_pause. */
  bool idle_tainted;      /* An idle ctx's dispatch found input or the end
                           * of the stream on it, but the ctx was taken for
                           * a new hop before that dispatch could evict it.
                           * Bytes that come after a complete response belong
                           * to no request, so the new hop must not read
                           * them as its answer (see
                           * _async_reused_conn_is_clean). Offering the ctx
                           * to the idle pool clears it. Only this ctx's
                           * dispatches read and write it. */
  bool reuse_checked;     /* _async_reused_conn_is_clean has run for this
                           * hop. _async_submit_hop clears it. */
  bool tls_read_blocked;  /* The read path's last ctls_conn_read returned
                           * EWOULDBLOCK. Whatever the TLS layer holds then
                           * is the start of a record that isn't complete
                           * yet: ctls_conn_has_pending_input reports it,
                           * but no read gets it until the rest arrives on
                           * the socket. A read that takes bytes or sees the
                           * end clears the flag. _async_input_pending reads
                           * it so that such a partial record doesn't pause a
                           * send the socket would let through, the same as
                           * the read_blocked of _chttp_send_all_watch in
                           * Tier 1. Only this ctx's dispatch reads and
                           * writes it. */
  bool resp_msg_started;  /* The parser holds the beginning of a response
                           * message, meaning a byte of it arrived after the
                           * last message the read path dropped. Only this
                           * ctx's dispatch reads and writes it. */
  char *wire; /* The request's serialized bytes, which we own. We keep
               * them for the whole send and free them once every byte is
               * written (wire_sent below tracks that), on the plain path
               * and the TLS path alike, since there's no queue to hand
               * them over to. The free in _async_ctx_free is always safe
               * and does nothing once the pointer is NULL. */
  size_t wire_len;
  size_t wire_sent; /* How many bytes of wire are written so far; both
                     * ctls_conn_write and a raw write(2) can do short
                     * writes as usual. */

  bool is_https;
  bool verify_host;
  /* We copy this from the chain each time a handshake is about to start on
   * this ctx and leave it alone while the ctx sits in the idle pool, so a
   * pooled Tier 2 or Tier 3 connection knows which TLS configuration its
   * handshake was made with (see the field comment of
   * chttpclient.tls_generation and the eligibility check of
   * _async_idle_pool_take). It means nothing unless is_https is true, and
   * we never read it otherwise. */
  uint64_t tls_generation;
#ifdef RUNNING_UNIT_TESTS
  /* Set while _async_idle_pool_take hands out its idle_lock hold and
   * cleared by the three unlocks that release it. A shutdown site for an
   * abandoned ctx reads it to report whether it ran before or after that
   * unlock, so it doesn't need to assert about its own position. See
   * _chttpclient_abandon_shutdown_under_lock_for_tests. */
  bool idle_lock_held_for_tests;
#endif
  _Atomic bool
      hop_completed; /* Set once the response of this hop is fully parsed,
                      * at the point where we either fulfil the future or
                      * hand off to a redirect.
                      *
                      * A spurious extra readable or writable dispatch
                      * would otherwise run that logic a second time, and
                      * _async_handle_redirect is not idempotent (the
                      * comment of that check says how such a dispatch can
                      * happen).
                      *
                      * _async_ctx_finish reads it too, together with
                      * reused, any_bytes_read and timed_out (see the
                      * comment at the top of the "ASYNC CONNECTION STATE
                      * MACHINE" section). Wherever a connection ends, that
                      * helper picks one of three actions: fulfil the
                      * chain's future, retry, or do nothing because the
                      * path that called _async_ctx_finish has already
                      * dealt with it.
                      *
                      * It is _Atomic since ccol_event_loop's own
                      * dispatch_lock doesn't protect it. The
                      * reused-connection path of _async_submit_hop writes
                      * it directly, and that path is plain application
                      * code running on whichever thread called
                      * chttpclient_do_async, outside any dispatch
                      * callback, while a dispatch for the same registered
                      * ctx can be in flight at the same moment.
                      *
                      * With a plain bool that is a real data race, which
                      * ThreadSanitizer reports, and the
                      * async_idle_pool.dead_connection_detected_and_retried
                      * scenario reaches it. The field comment of
                      * pending_app_teardown explains why a write of this
                      * field alone must never come with a direct
                      * _async_ctx_teardown call from an application
                      * thread. */
  _Atomic bool
      pending_app_teardown; /* Set by two failure paths that run on an
                             * application thread, the ones in
                             * _async_submit_hop and
                             * _async_submit_hop_fail where a reused
                             * connection can't be activated again. They
                             * always set it together with hop_completed
                             * and always before the
                             * shutdown(ctx->fd, ...) call.
                             *
                             * Those two paths must NEVER call
                             * _async_ctx_teardown themselves. The
                             * ccol_event_loop registration of the ctx is
                             * fully live at that point, since we just
                             * popped the ctx from the idle pool for
                             * reuse, so a reactor dispatch callback can
                             * be in flight or about to run on another
                             * thread right then, and the OS can preempt
                             * that thread for as long as it likes between
                             * ccol_event_loop's liveness check and its
                             * first touch of ctx.
                             *
                             * If an application thread frees ctx while
                             * that can happen, we get a real
                             * use-after-free, which AddressSanitizer
                             * reports. A per-ctx atomic refcount pinned
                             * by every dispatch callback doesn't close
                             * the hole, because the pin's very first
                             * read of that refcount can race a
                             * concurrent free in exactly the same way.
                             *
                             * So we close it by construction: no
                             * application thread ever runs the real,
                             * destructive teardown of a registered ctx.
                             * The two failure sites mark ctx as terminal,
                             * shut its fd down (which forces a genuine
                             * EPOLLIN or EPOLLERR dispatch, since the fd
                             * is registered for read) and return without
                             * touching ctx again.
                             *
                             * The dispatch callback then finds
                             * hop_completed already true (see
                             * _async_ctx_handle_if_abandoned, which all
                             * five such checks call across
                             * _async_on_readable_impl,
                             * _async_on_writable_impl and
                             * _async_on_error_impl). That helper uses
                             * this flag to tell two cases apart. False
                             * means an earlier dispatch already handled
                             * everything and nothing is left to do. True
                             * means an application thread left the real
                             * teardown to whichever dispatch notices it,
                             * and the helper runs _async_ctx_teardown
                             * for real. Doing that from a dispatch is
                             * safe because ccol_event_loop serialises it
                             * against every other dispatch of this exact
                             * registration, with its entry->dispatch_lock
                             * and its entry->refcount invariant of one
                             * job in flight per entry.
                             *
                             * Unlike hop_completed, this is written once
                             * per ctx. A ctx that ever has it true always
                             * ends in termination and is never pooled or
                             * reused again, so nothing resets it to
                             * false, whereas _async_idle_pool_offer does
                             * reset hop_completed. */
  ctls_conn_t *tls;         /* NULL until the connect succeeds and the
                             * handshake starts, and always NULL for plain
                             * HTTP. It has no lock of its own; the comment
                             * at the top of this section explains why the
                             * per-registration dispatch_lock of
                             * ccol_event_loop is enough. */

  _ccol_mutex_t idle_lock; /* Guards state and chain as a pair while a
                            * connection goes from idle to active and back.
                            *
                            * _async_idle_pool_take pops a connection out of
                            * cli->idle_pools_async, but the connection stays
                            * fully attached to the reactor all along, since
                            * there is no way to pause the poll of one fd.
                            * Taking it out of the POOL's bookkeeping doesn't
                            * stop the reactor, which can dispatch
                            * on_readable, on_writable or on_error for that
                            * fd on another thread as soon as the peer sends
                            * something or the connection dies. At the same
                            * time _async_submit_hop can be halfway through
                            * moving ctx->state off CHTTP_ASYNC_IDLE and
                            * ctx->chain off NULL for the new owner.
                            *
                            * Without the lock, a dispatch's IDLE-state guard
                            * can read a stale ctx->state on a ctx we popped
                            * but haven't reconfigured yet; the dispatch then
                            * falls through the guard and dereferences
                            * ctx->chain while it is NULL.
                            *
                            * We hold it only around the few statements that
                            * flip state and chain in either direction, and
                            * never across I/O.
                            *
                            * The deadline sweep does NOT take this lock on
                            * purpose. The "ASYNC DEADLINE SWEEP" section
                            * describes the lock-ordering hazard that taking
                            * it would create and the shutdown(fd) design
                            * that avoids it.
                            *
                            * state, chain, fd and timed_out are _Atomic so
                            * the sweep can read them without a lock, and the
                            * comments of those fields explain why single
                            * atomic reads are enough for the sweep and why it
                            * can do without the compound guarantee of this
                            * lock. Every OTHER user of state and chain (the
                            * dispatch callbacks, which reach them through
                            * _async_dispatch_kind) needs that full compound
                            * protection and gets it from here. */

  chttp_deadline_t connect_deadline; /* Meaningful only while state is
                                      * CHTTP_ASYNC_CONNECTING or
                                      * CHTTP_ASYNC_TLS_HANDSHAKING. The
                                      * thread about to submit the hop
                                      * attempt sets it fresh at the start
                                      * of every hop that really connects,
                                      * and never for a reused connection,
                                      * which skips both of those states.
                                      *
                                      * It is NOT _Atomic on purpose. We
                                      * write it exactly once, strictly
                                      * before this attempt's
                                      * _client_deadline_register call, and
                                      * the lock and unlock inside that
                                      * call form a release and acquire
                                      * pair which already hands a fully
                                      * initialized value to the sweep. The
                                      * sweep only ever sees a ctx it found
                                      * through the registry, so nothing
                                      * more is needed (see the "ASYNC
                                      * DEADLINE SWEEP" section). */
  _ccol_mutex_t deadline_lock;       /* Guards overall_deadline below and
                                      * continue_deadline. It also makes the
                                      * deadline sweep's verdict (its
                                      * snapshot of state and chain plus its
                                      * write of timed_out) a single step
                                      * against the idle/active transitions
                                      * of _async_idle_pool_offer and
                                      * _async_idle_pool_take, which do
                                      * those writes under it as well. It's
                                      * a small leaf lock: we never hold it
                                      * while trying to take idle_lock or a
                                      * deadline stripe's mutex, so it
                                      * can't form a lock-ordering cycle
                                      * with either.
                                      *
                                      * Writers and the reader both hold it
                                      * briefly. The writers are
                                      * _async_idle_pool_take,
                                      * _async_submit_hop and
                                      * _async_retry_hop, any of which may
                                      * already hold idle_lock when it
                                      * writes overall_deadline. The reader
                                      * is the deadline sweep, which holds
                                      * the mutex of this ctx's own stripe
                                      * while it walks that stripe.
                                      *
                                      * THIS field needs a real lock. For
                                      * connect_deadline above, the _Atomic
                                      * ctx->chain works as a publication
                                      * barrier because we publish it
                                      * exactly once through the mutex of
                                      * _client_deadline_register, but
                                      * overall_deadline can be REWRITTEN
                                      * on every idle-pool reuse cycle with
                                      * no new _client_deadline_register
                                      * call at all. That is a genuine data
                                      * race and ThreadSanitizer reports
                                      * it. */
  chttp_deadline_t overall_deadline; /* A ctx-local COPY of
                                      * chain->overall_deadline, which
                                      * _async_chain_create computes once
                                      * and which never changes while the
                                      * chain lives. We update the copy
                                      * under deadline_lock wherever a live
                                      * chain is assigned to ctx->chain:
                                      * the fresh and reused paths of
                                      * _async_submit_hop,
                                      * _async_idle_pool_take and
                                      * _async_retry_hop.
                                      *
                                      * The copy is there so the deadline
                                      * sweep never dereferences ctx->chain.
                                      * Reading chain->overall_deadline
                                      * through node->chain would be a
                                      * use-after-free: the sweep loads the
                                      * pointer atomically and dereferences
                                      * it later, nothing stops the chain's
                                      * last reference from going away in
                                      * between, and by then the chain can
                                      * be freed.
                                      *
                                      * So the sweep only compares
                                      * node->chain with NULL (a plain
                                      * pointer read, no dereference, no
                                      * lock) to tell whether this field
                                      * means anything right now, and reads
                                      * the field itself under
                                      * deadline_lock. */
  _Atomic bool abandoned;            /* Set by the deadline sweep when it
                                      * fulfils the future of a hop that is
                                      * alive: either its name lookup (or
                                      * its wait for a lookup thread) ran
                                      * past the deadline, or the sweep
                                      * marked it timed out on an earlier
                                      * tick and it is waiting for a
                                      * dispatch. From then on the hop
                                      * counts in g_async_hops_abandoned
                                      * until it is freed or offered to the
                                      * idle pool. */
  _Atomic bool timed_out;   /* The deadline sweep sets this without a lock
                             * just before it shuts this connection's fd
                             * down (see the "ASYNC DEADLINE SWEEP"
                             * section). The normal dispatch-driven
                             * teardown, _async_ctx_finish, then reports
                             * ccol_timed_out and skips the usual retry of a
                             * dead connection, since retrying past an
                             * expired deadline only makes the overrun
                             * longer. _Atomic for the same reason as state,
                             * chain and fd above. */
  bool deadline_registered; /* True once we have linked this ctx into its
                             * stripe's list
                             * (client_deadline_bundle.stripes[
                             * _deadline_stripe_index_for_ctx(ctx)]) at
                             * least once. The registration is idempotent
                             * and, once made, lasts for the whole life of
                             * the ctx, idle-pool cycles included. See the
                             * comment of _client_deadline_register. */
  struct chttp_async_ctx_s *deadline_prev,
      *deadline_next; /* Intrusive links of a doubly-linked list: this
                       * ctx's stripe inside the process-wide deadline
                       * registry, whose locks are sharded. They are
                       * guarded by that stripe's mutex, which is not
                       * idle_lock above. The comment of
                       * client_deadline_bundle gives the lock-ordering
                       * contract between the two, and it holds for every
                       * stripe's mutex. */

  chttp1_parser_t parser;
  chttp_parse_ctx_t pctx;
  chttp_bodybuf_t bb;

  /* "Expect: 100-continue" for Tier 2 and Tier 3, following the design of
   * chttp_do_internal and _chttp_send_and_read in Tier 1, whose doc comment
   * has the contract. There are three outcomes: on an interim 100 we send
   * the body, a direct answer from the server IS the final response and the
   * body is never sent, and when the wait expires we send the body anyway.
   * Here it's driven by dispatches instead of Tier 1's blocking wait
   * loop. */
  bool want_100_continue; /* Computed once per hop attempt by
                           * _async_submit_hop and _async_retry_hop, the
                           * same way as the use_100_continue local of
                           * Tier 1. The condition is
                           * req->expect_continue && !has_explicit_expect
                           * && body_carrying_method && body.data &&
                           * body.len > 0.
                           *
                           * Only the callbacks of ctx->reg (serialised by
                           * the dispatch_lock) and the hop-setup code
                           * (which runs strictly before we register the
                           * ctx) touch it, so it needs neither an atomic
                           * type nor a lock. */
  size_t header_len;      /* Meaningful only when want_100_continue is true.
                           * It's where the header block ends and the body
                           * starts inside `wire`, wire_len - body_len, the
                           * same as Tier 1's header_len local. The first
                           * write phase stops here instead of at wire_len
                           * (see the want_100_continue branch of
                           * _async_plain_try_write and of
                           * _async_tls_try_write). */
  _Atomic bool
      continue_decided; /* Settles the race between a read that sees the
                         * 100 and a write that sees the timeout, the way
                         * hop_completed settles which dispatch finishes a
                         * hop elsewhere in this file.
                         *
                         * Only one of two branches may decide to write the
                         * body, the "100 Continue seen" branch of
                         * _async_on_readable_impl or the
                         * "continue_deadline expired" branch of
                         * _async_on_writable_impl. They never run at the
                         * same time, since both directions of one fd share
                         * one entry and ccol_event_loop's per-entry
                         * dispatch_lock serialises that entry.
                         *
                         * Whichever callback gets there first sets the
                         * field with atomic_exchange and carries on only
                         * if it saw it flip from false. If the other one
                         * runs too, it sees true and does nothing.
                         *
                         * Reset to false at the start of every fresh
                         * want_100_continue hop attempt, just like
                         * interim_responses_seen. */
  _Atomic bool
      continue_msg_started; /* True while the parser holds part of a
                             * message that came in while we waited for a
                             * "100 Continue". continue_deadline only bounds
                             * the wait for a message's first byte, like the
                             * window of _chttp_send_and_read in Tier 1, so
                             * a message that started inside the window is
                             * read to the end under overall_deadline alone.
                             * While this is true, the window expiring
                             * neither sends the body nor touches the
                             * parser.
                             *
                             * Only this ctx's dispatches write it, ordered
                             * by ccol_event_loop's per-entry dispatch_lock.
                             * It's atomic because the deadline sweep reads
                             * it as well, to skip a flip to the write
                             * direction that the dispatch would just undo.
                             * Cleared wherever continue_decided is
                             * cleared. */
  chttp_deadline_t
      continue_deadline; /* The earlier of CHTTP_100_CONTINUE_WAIT_US and
                          * what is left of ctx->overall_deadline, combined
                          * by _deadline_earlier. Computed once, when this
                          * hop attempt moves into
                          * CHTTP_ASYNC_AWAITING_CONTINUE.
                          *
                          * The deadline sweep is its only reader, on its
                          * own thread, and a reused ctx rewrites it for
                          * every hop that waits for a continue. The writer
                          * stores it and the sweep copies it under
                          * deadline_lock, in the same critical section as
                          * its snapshot of state, so the copy and the
                          * state it gets judged against always come from
                          * the same hop. */
  bool retry_unsafe;     /* Same as the *retry_unsafe_out parameter of
                          * _chttp_send_and_read in Tier 1. It goes true the
                          * moment this hop puts the body on the wire, and
                          * that only happens AFTER the server sent an
                          * explicit "100 Continue" on THIS connection.
                          *
                          * From then on, a read failure with zero bytes of
                          * a final response doesn't mean nothing went out,
                          * so a retry isn't free (the doc comment of that
                          * function has the whole argument about
                          * submitting a non-idempotent request twice).
                          *
                          * _async_ctx_finish looks at it next to reused and
                          * any_bytes_read. Reset to false at the start of
                          * every fresh hop attempt. */
  bool replayable;       /* _request_is_replayable for this hop, as computed
                          * by _async_submit_hop. When a reused connection
                          * took every byte of a non-replayable request and
                          * then died without an answer, we fail the request
                          * instead of retrying it (see _async_ctx_finish). */
  bool request_close;    /* The request's Connection header named the
                          * "close" option, so this connection never goes
                          * back to the idle pool (see
                          * _async_finish_connection). _async_submit_hop
                          * sets it on every hop and _async_retry_hop copies
                          * it. */
  char *continue_carry;  /* Non-NULL only when a "100 Continue" line comes
                          * in the same read() as the start of the next
                          * message, normally the final response. A fast
                          * server can write both before it even reads the
                          * body, which RFC 7231 SS5.1.1 allows, and Tier 1
                          * feeds the same bytes to its early-response
                          * parser in _chttp_send_and_read.
                          *
                          * It's our own copy of those trailing bytes.
                          * _async_awaiting_continue_on_data stores it and
                          * pauses the body send at once, then
                          * _async_upload_pause feeds it to
                          * _async_process_reading_data before any body
                          * byte goes out, frees it and sets the field back
                          * to NULL. From those bytes the read path decides
                          * whether the body is sent at all. If the ctx is
                          * abandoned in between, its teardown frees the
                          * copy. */
  size_t continue_carry_len;
} chttp_async_ctx_t;

/* ========================================================================== */
/*                    ASYNC DEADLINE SWEEP (TIER 2)                           */
/* ========================================================================== */

/*
 * Enforces connect_timeout_us and request_timeout_us for Tier 2 and Tier 3
 * as absolute deadlines on CLOCK_MONOTONIC, with exactly the semantics of
 * _deadline_make and _deadline_remaining_ms in Tier 1. It also catches a
 * connection that never goes fully idle but blows its deadline anyway, for
 * example a slow trickle of bytes that always arrives just before an
 * activity-reset timeout would fire.
 *
 * ccol_event_loop has no timers of its own, so this is a small thread that
 * sweeps at the earliest deadline of the requests in flight, and at least
 * every CHTTP_DEADLINE_SWEEP_INTERVAL_MS (see g_sweep_next_ns and
 * _client_deadline_sweep_fn). It is separate from the reactor threads and
 * from the ctpool for DNS and connect, and we start and stop it next to them
 * (_client_deadline_sweep_start and _client_deadline_sweep_stop_and_join,
 * called from _client_engine_acquire and _client_engine_reaper_fn above).
 *
 * The registry of live async ctx nodes is split into
 * CHTTP_DEADLINE_SWEEP_STRIPES stripes in client_deadline_bundle.stripes[],
 * each with its own mutex and its own head of an intrusive doubly-linked
 * list, instead of one global mutex over one global list.
 *
 * A register or unregister locks only the stripe its ctx hashes into (the
 * index comes from _deadline_stripe_index_for_ctx), so registrations of
 * different ctx nodes for many requests in flight don't serialise against
 * each other. The sweep walks one stripe at a time and only blocks a
 * register or unregister that lands in that stripe, never every one in the
 * process.
 *
 * The register and the unregister of one request always land on the same
 * stripe by construction. The hash is a pure, stateless function of the ctx
 * pointer and both calls compute it the same way, so the ctx doesn't need a
 * field to remember its stripe.
 *
 * It's the same idea as the lock-striped fd registry of ccol_event_loop in
 * cthreadcomm.c (_stripe_index_for_fd), and for the same reason: with a real
 * hash key over a ctx, every lookup must find the same stripe.
 * ccol_event_loop gives a queue selectable its stripe round-robin instead,
 * which is only safe because a queue selectable is never looked up a second
 * time.
 *
 * client_deadline_bundle.mutex and client_deadline_bundle.cond_var are a
 * separate matter. They guard only the start, stop and sleep-wake lifecycle
 * of the sweep thread (client_deadline_bundle.stop and
 * client_deadline_bundle.thread) and play no part in protecting the
 * registry.
 *
 * A stripe's mutex serialises two things for that stripe. One is its
 * intrusive list (ctx->deadline_prev and ctx->deadline_next of every ctx
 * that hashes into it). The other is the sweep, which reads the fields of a
 * registered ctx, against any other thread that frees that same ctx at the
 * same time. That second point is why we unregister a ctx only from inside
 * _async_ctx_free, the one reliable place where its memory really gets
 * freed.
 *
 * _client_deadline_unregister can't finish while the sweep is walking that
 * ctx's own stripe with the mutex held, so _async_ctx_free can't go ahead
 * and free the ctx. A walk of a DIFFERENT stripe in the same tick is not
 * affected and runs concurrently.
 *
 * Registration through _client_deadline_register is idempotent, and once
 * made it lasts for the whole life of a ctx, idle-pool cycles included. An
 * idle pooled ctx just sits in its stripe, and the sweep's checks (state ==
 * CONNECTING or TLS_HANDSHAKING, and chain != NULL) skip it while it is
 * idle. That is simpler than unregistering on every idle-pool offer and
 * registering again on every reuse, and just as correct.
 *
 * Lock ordering: a caller CAN legitimately call _client_deadline_register
 * while holding some ctx's idle_lock. The reused-connection branch of
 * _async_submit_hop for a failed ccol_event_loop_modify does exactly that:
 * it holds the abandoned ctx's idle_lock across its call to
 * _async_retry_hop, which registers the brand-new retry ctx before the
 * unlock. So we do NOT always take a stripe mutex outside every
 * ctx->idle_lock.
 *
 * That is deadlock-free only because the reverse order never happens.
 * Neither _client_deadline_register nor _client_deadline_unregister touches
 * any ctx's idle_lock while it holds a stripe mutex, since both only work on
 * that one stripe's list. The sweep, _client_deadline_sweep_once, is the
 * only other user of a stripe mutex and it never takes idle_lock at all; it
 * reads every field it needs with plain _Atomic loads (see the comment of
 * that function).
 *
 * THAT one-way invariant is what matters: no code may hold a stripe mutex
 * while it takes any ctx->idle_lock. Don't "fix" the sweep, the register
 * function or the unregister function to take idle_lock as well before you
 * work this reasoning out again, because the idle_lock-then-stripe-mutex
 * order above is in real use and such a change turns it into a genuine
 * AB-BA lock-order inversion.
 *
 * Nothing in this file holds two stripe mutexes at once either. Every
 * operation touches exactly one ctx and so exactly one stripe, and the sweep
 * locks a stripe, walks it and unlocks it completely before it moves on, so
 * the striping adds no lock-ordering surface beyond the stripe mutex and
 * idle_lock rule above.
 *
 * How the sweep neuters an expired connection: a raw fd protects nothing by
 * itself. It can be closed and its number reused by an unrelated connection
 * between the moment the sweep captures it and the moment the sweep acts on
 * it, so an fd operation on a captured int after an unlock can hit somebody
 * else's connection. A fresh connect() that reuses the closed fd number gets
 * us into exactly that state.
 *
 * The sweep avoids it by construction, without a generation check of its
 * own. Every teardown path (_async_ctx_finish and _async_idle_ctx_finish,
 * defined after the idle pool section below) calls
 * _client_deadline_unregister strictly BEFORE it closes ctx->fd, and
 * _client_deadline_unregister needs that ctx's stripe mutex, so as long as a
 * ctx is linked into its stripe its fd is open.
 *
 * So the sweep calls shutdown(fd, SHUT_RDWR) directly while it holds that
 * ctx's stripe mutex, with no separate collect-then-act-after-unlock phase.
 * shutdown() is a plain kernel operation with no synchronous application
 * callback, so calling it here can't cause reentrancy.
 *
 * shutdown() kills both directions of the connection, and the next
 * epoll_wait reports the fd as readable with an error and writable with an
 * error. The sweep never touches the ctx's memory, its idle_lock or its
 * ccol_event_loop registration. Once the reactor sees the fd, the normal
 * dispatch path (_async_on_readable, _async_on_writable and _async_on_error)
 * takes over and handles it like any other connection that failed by
 * itself.
 */
#define CHTTP_DEADLINE_SWEEP_STRIPES 16

typedef struct {
  _ccol_mutex_t mutex;
  chttp_async_ctx_t *head;
} chttp_deadline_stripe_t;

static struct {
  _ccol_mutex_t mutex;
  _ccol_cond_var_t cond_var;
  /* These two (the bundle's mutex and condition variable) only guard the
   * start, stop and sleep-wake lifecycle of the sweep thread, meaning the
   * fields stop and thread. They never guard the ctx registry, which is
   * locked per stripe (see stripes[] below).
   *
   * Both are initialized lazily at run time under a _ccol_call_once guard
   * (see _client_deadline_init_globals) of their own, separate from the
   * once-guard of the async engine pair above. A static constant
   * initializer won't do, because on a backend that isn't pthreads the
   * equivalent primitive can need real setup work that a compile-time
   * constant can't express.
   *
   * Every function below that touches any of these globals starts with
   * _ccol_call_once(client_deadline_bundle.once, ...). */
  _ccol_once_flag_t once;
  chttp_deadline_stripe_t stripes[CHTTP_DEADLINE_SWEEP_STRIPES];
  _ccol_thread_id_t thread;
  bool stop;
} client_deadline_bundle = {0};

/* The earliest moment, in CLOCK_MONOTONIC nanoseconds, when the sweep has
 * something to do: the earliest connect, request or 100-continue deadline,
 * or Happy Eyeballs attempt start, that lies in the future. The sweep thread
 * sleeps until then, but never longer than CHTTP_DEADLINE_SWEEP_INTERVAL_MS.
 * Each tick sets it to UINT64_MAX before its walk and lowers it to the
 * earliest instant the walk finds, and a thread setting a deadline lowers it
 * too. So a deadline set during a walk can't get lost: either the walk sees
 * it or it is left in this value afterwards. Every deadline is lowered into
 * it only after the ctx is in the registry, and that's what lets the walk
 * see it. */
static _Atomic uint64_t g_sweep_next_ns = UINT64_MAX;

/* When the sleeping sweep thread plans to wake, in CLOCK_MONOTONIC
 * nanoseconds, or 0 while it is awake. A thread lowering g_sweep_next_ns
 * wakes the sweep only if the new instant is earlier than this, so a
 * deadline after the planned wake costs no wake at all. The sweep stores
 * this and then reads g_sweep_next_ns again, while a thread setting a
 * deadline lowers g_sweep_next_ns and then reads this. All four accesses are
 * sequentially consistent, so either the sweep sees the new deadline or the
 * other thread sees the planned instant and wakes it. */
static _Atomic uint64_t g_sweep_planned_ns = 0;

/* True on the sweep thread. It plans its own wait after each walk, so a
 * deadline it sets itself needs no wake, and at that point it holds a stripe
 * lock under which client_deadline_bundle.mutex must not be taken (the fork
 * handler takes that mutex before the stripes). */
static __thread bool t_on_sweep_thread = false;

#ifdef RUNNING_UNIT_TESTS
static atomic_ulong g_sweep_ticks_for_tests;

/* How many sweep ticks the process has run. */
unsigned long _chttp_sweep_ticks_for_tests(void) {
  return atomic_load(&g_sweep_ticks_for_tests);
}

/* The CLOCK_MONOTONIC instant, in nanoseconds, that the sweep thread planned
 * the current walk for. Only the sweep thread writes and reads it. */
static uint64_t g_sweep_walk_plan_ns_for_tests;

/* For each kind of timed work a walk acts on (0: a deadline the walk finds
 * expired, 1: the start of the next Happy Eyeballs attempt), how many times
 * a walk acted on it, and the largest gap between the walk's planned instant
 * and the instant the work was due. The planned instant is our own decision
 * and doesn't depend on how late the scheduler gets the sweep thread
 * running, so a test can assert on it even on a loaded machine. A walk
 * planned for the instant the work is due gives a gap of at most 0, and one
 * planned on a fixed cadence gives a positive gap. */
static atomic_ulong g_sweep_due_events_for_tests[2];
static _Atomic int64_t g_sweep_due_max_late_ns_for_tests[2];

static void _sweep_note_due_for_tests(int kind, uint64_t due_ns) {
  int64_t late = (int64_t)g_sweep_walk_plan_ns_for_tests - (int64_t)due_ns;
  atomic_fetch_add(&g_sweep_due_events_for_tests[kind], 1);
  int64_t cur = atomic_load(&g_sweep_due_max_late_ns_for_tests[kind]);
  while (late > cur &&
         !atomic_compare_exchange_weak(&g_sweep_due_max_late_ns_for_tests[kind],
                                       &cur, late)) {
  }
}

/* Resets the counts and the maxima above. */
void _chttp_sweep_due_reset_for_tests(void) {
  for (int k = 0; k < 2; k++) {
    atomic_store(&g_sweep_due_events_for_tests[k], 0);
    atomic_store(&g_sweep_due_max_late_ns_for_tests[k], INT64_MIN);
  }
}

/* Returns the count for `kind` since the last reset and stores its largest
 * lateness, in nanoseconds, in *max_late_ns; with a count of 0 the lateness
 * is INT64_MIN. */
unsigned long _chttp_sweep_due_for_tests(int kind, int64_t *max_late_ns) {
  *max_late_ns = atomic_load(&g_sweep_due_max_late_ns_for_tests[kind]);
  return atomic_load(&g_sweep_due_events_for_tests[kind]);
}
#endif

static size_t _deadline_stripe_index_for_ctx(const chttp_async_ctx_t *ctx) {
  uintptr_t p = (uintptr_t)ctx;
  /* Fold the whole pointer down to 32 bits, xor of the high half and the low
   * half, before the multiplicative hash. Just truncating to the low 32 bits
   * is worse, since the low bits of a heap pointer are often fixed by
   * alignment (always a multiple of 16, say) and truncation would drop the
   * entropy that sits in the high bits. The Knuth multiplicative constant is
   * the same one _stripe_index_for_fd uses in cthreadcomm.c.
   *
   * The `p >> 32` fold only makes sense, and only compiles, where uintptr_t
   * is wider than 32 bits. On a 32-bit target like i386 uintptr_t IS the
   * 32-bit value and there is no high half, so `p >> 32` shifts by the full
   * width of the type, which is undefined behavior and, with our build
   * flags, a hard -Werror=shift-count-overflow error.
   *
   * We guard it with a preprocessor #if and not a run-time if, like the
   * `#if SIZE_MAX == UINT64_MAX` convention of chashmap.c and for the same
   * reason: a run-time check doesn't keep the compiler from fully type
   * checking the branch that is dead on this platform, and rejecting it. */
  uint32_t folded;
#if UINTPTR_MAX > 0xFFFFFFFFu
  folded = (uint32_t)p ^ (uint32_t)(p >> 32);
#else
  folded = (uint32_t)p;
#endif
  uint32_t h = folded * 2654435761u;
  return (size_t)h % CHTTP_DEADLINE_SWEEP_STRIPES;
}

static void _client_deadline_init_globals(void) {
  if (_ccol_mutex_init(client_deadline_bundle.mutex) != 0)
    _ccol_fatal_err("chttpclient deadline sweep: failed to initialize mutex");
  for (size_t i = 0; i < CHTTP_DEADLINE_SWEEP_STRIPES; i++) {
    if (_ccol_mutex_init(client_deadline_bundle.stripes[i].mutex) != 0)
      _ccol_fatal_err(
          "chttpclient deadline sweep: failed to initialize stripe mutex");
  }
  /* CLOCK_MONOTONIC, to match the wake deadline of
   * _client_deadline_sweep_fn, which comes from
   * clock_gettime(CLOCK_MONOTONIC, ...). _ccol_cond_var_init defaults to
   * CLOCK_REALTIME, and with that the deadline comparison makes no sense: a
   * monotonic timespec is a small value counted from boot, and the condition
   * variable would compare it with the wall clock internally.
   *
   * _ccol_cond_var_timedwait would then return ETIMEDOUT immediately on every
   * call instead of sleeping until the next planned sweep, and the sweep
   * thread would spin at 100 percent CPU for as long as the engine lives.
   *
   * srv->requests_done_cv in chttpserver.c is initialized the same way for
   * the same reason. _ccol_cond_var_init_monotonic, this file's shared
   * helper for exactly this pattern, has the full reasoning. */
  if (_ccol_cond_var_init_monotonic(&client_deadline_bundle.cond_var) != 0)
    _ccol_fatal_err(
        "chttpclient deadline sweep: failed to initialize condition "
        "variable");
}

#define CHTTP_DEADLINE_SWEEP_INTERVAL_MS 100
/* A soft cap on how many expired connections one sweep tick shuts down. It
 * heals itself rather than being a hard limit: a connection past the cap on
 * one tick stays registered and isn't marked timed_out yet, and the next
 * tick (which runs at once after a tick that hits the cap) finds it again
 * and makes its shutdown() call. */
#define CHTTP_DEADLINE_SWEEP_BATCH 256

/*
 * Adds ctx to the deadline registry unless it is there already. A caller
 * may hold some ctx's idle_lock while calling this, and the retry of an
 * abandoned reused connection does. That is fine because we only take one
 * stripe's mutex here and never an idle_lock, which is the order the lock
 * ordering note above allows.
 *
 * Only the one stripe that ctx hashes into gets locked. No other stripe,
 * and not client_deadline_bundle.mutex, which is about the sweep thread's
 * lifecycle and has nothing to do with the registry.
 */
static void _client_deadline_register(chttp_async_ctx_t *ctx) {
  _ccol_call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  chttp_deadline_stripe_t *stripe =
      &client_deadline_bundle.stripes[_deadline_stripe_index_for_ctx(ctx)];
  _ccol_mutex_lock(stripe->mutex);
  if (!ctx->deadline_registered) {
    ctx->deadline_prev = NULL;
    ctx->deadline_next = stripe->head;
    if (stripe->head) stripe->head->deadline_prev = ctx;
    stripe->head = ctx;
    ctx->deadline_registered = true;
  }
  _ccol_mutex_unlock(stripe->mutex);
}

static uint64_t _timespec_to_ns(const struct timespec *t) {
  if (t->tv_sec < 0) return 0;
  if ((uint64_t)t->tv_sec >= UINT64_MAX / 1000000000u) return UINT64_MAX - 1;
  return (uint64_t)t->tv_sec * 1000000000u + (uint64_t)t->tv_nsec;
}

static uint64_t _monotonic_now_ns(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return _timespec_to_ns(&now);
}

/* Lowers g_sweep_next_ns to at_ns. If that moves it before the planned wake
 * of the sleeping sweep thread and we aren't on the sweep thread ourselves,
 * we wake the sweep so it can plan its wait again; the wake only replans,
 * and the actual walk of the registry happens at the planned instant. We
 * take client_deadline_bundle.mutex only when the deadline is earlier than
 * anything the sweep knows about, so a request whose deadline falls after
 * one that is already pending pays one atomic load. The caller holds no
 * stripe lock, except the sweep thread, which never takes the mutex here. */
static void _client_deadline_note_ns(uint64_t at_ns) {
  uint64_t cur = atomic_load(&g_sweep_next_ns);
  while (at_ns < cur) {
    if (atomic_compare_exchange_weak(&g_sweep_next_ns, &cur, at_ns)) {
      if (t_on_sweep_thread) return;
      if (at_ns >= atomic_load(&g_sweep_planned_ns)) return;
      _ccol_call_once(client_deadline_bundle.once,
                      _client_deadline_init_globals);
      _ccol_mutex_lock(client_deadline_bundle.mutex);
      _ccol_cond_var_signal(client_deadline_bundle.cond_var);
      _ccol_mutex_unlock(client_deadline_bundle.mutex);
      return;
    }
  }
}

static void _client_deadline_note(const chttp_deadline_t *d) {
  if (d->active) _client_deadline_note_ns(_timespec_to_ns(&d->deadline));
}

/* Removes ctx from the deadline registry if it is in it. It is called
 * exactly once, as the first statement of _async_ctx_free (the comment of
 * that function explains why that's the one right place for it).
 *
 * _deadline_stripe_index_for_ctx is a pure, stateless function of ctx's own
 * address, so we compute and lock the same stripe that
 * _client_deadline_register used for this ctx, without having to remember
 * anywhere which stripe held it. */
static void _client_deadline_unregister(chttp_async_ctx_t *ctx) {
  _ccol_call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  chttp_deadline_stripe_t *stripe =
      &client_deadline_bundle.stripes[_deadline_stripe_index_for_ctx(ctx)];
  _ccol_mutex_lock(stripe->mutex);
  if (ctx->deadline_registered) {
    if (ctx->deadline_prev) {
      ctx->deadline_prev->deadline_next = ctx->deadline_next;
    } else {
      stripe->head = ctx->deadline_next;
    }
    if (ctx->deadline_next)
      ctx->deadline_next->deadline_prev = ctx->deadline_prev;
    ctx->deadline_prev = ctx->deadline_next = NULL;
    ctx->deadline_registered = false;
  }
  _ccol_mutex_unlock(stripe->mutex);
}

/*
 * One sweep pass. It walks the CHTTP_DEADLINE_SWEEP_STRIPES stripes one
 * after another and holds only that stripe's mutex for the whole walk of
 * it, never two stripe locks at once and never client_deadline_bundle.mutex.
 *
 * For each ctx there are two deadlines to check: connect_deadline, only
 * while the ctx really is in a connect phase, and the chain's
 * overall_deadline whenever the ctx has a live chain.
 *
 * Every connection that has just expired gets its fd shut down right there,
 * with the stripe lock held. The comment at the top of this section explains
 * why that is safe; there's no reentrancy and no race over a reused fd.
 *
 * n_shutdown is counted over the whole tick, across all stripes, so
 * CHTTP_DEADLINE_SWEEP_BATCH bounds the work of one tick in total and not
 * per stripe.
 *
 * state, chain, fd and timed_out are read with plain loads, and since each
 * of them is _Atomic every single load is race-free. We do NOT read them
 * under the ctx's idle_lock, on purpose (see the lock ordering note above),
 * because some paths legitimately hold a ctx's idle_lock and then lock a
 * deadline stripe. One of them is the failure branch of _async_submit_hop
 * for a write on a reused connection, which calls _async_retry_hop, which
 * calls _client_deadline_register. If the sweep took idle_lock while holding
 * a stripe mutex, that would be a classic AB-BA lock-order inversion on that
 * stripe.
 *
 * We can live without the COMPOUND guarantee of idle_lock (state and chain
 * together) because of what we do with a snapshot that may be torn.
 * idle_lock really protects two points, the idle-to-active transition in
 * _async_idle_pool_take and the active-to-idle one in
 * _async_idle_pool_offer.
 *
 * At either point, every torn combination of state and chain this code can
 * see falls into one of two cases. Either it skips both deadline checks
 * because chain is NULL or state isn't a connect state, or it reads a chain
 * that is certainly alive, maybe a moment away from release, because the
 * chain reference is dropped strictly after the state and chain pair is
 * updated.
 *
 * Long story short, the worst a torn read can do is shut down one
 * connection a few instructions too early or too late, a connection we
 * would otherwise have pooled cleanly or moved on to the next hop. That
 * loses an optimisation but never wrongly aborts some unrelated request in
 * flight. Every OTHER user of state and chain (the dispatch callbacks,
 * through _async_dispatch_kind) needs the full guarantee of idle_lock and
 * takes it.
 *
 * We mark ctx->timed_out before shutting its fd down, for two reasons. It
 * tells the normal teardown path that this is a timeout and not a plain
 * connection failure, so that path reports ccol_timed_out and skips the
 * usual retry of a dead connection (retrying past an expired deadline only
 * makes the overrun longer). And it keeps repeated sweep ticks idempotent
 * for a ctx that is in the middle of tearing down after an earlier tick's
 * shutdown() call.
 */
#ifdef RUNNING_UNIT_TESTS
/* Arming g_sweep_race_armed_for_tests makes the next sweep tick declare the
 * first ctx it finds in CHTTP_ASYNC_READING expired, as if the sweep's clock
 * had passed that hop's deadline while the hop's own dispatch finished just
 * inside it. The sweep then parks twice on that ctx, in phase 1 between its
 * verdict and its write of timed_out, and in phase 2 between the end of its
 * critical section and its shutdown() of the fd. The phase it waits in shows
 * up in g_sweep_race_parked_for_tests, and the test lets a phase go by
 * writing its number into g_sweep_race_release_for_tests. Each park is
 * bounded, so a test that fails before releasing it can't hang the sweep for
 * good. */
static _Atomic bool g_sweep_race_armed_for_tests = false;
static void *_Atomic g_sweep_race_forced_ctx_for_tests = NULL;
static _Atomic int g_sweep_race_parked_for_tests = 0;
static _Atomic int g_sweep_race_release_for_tests = 0;
/* The offer path bumps the first counter before the critical section that
 * makes a ctx idle and the second one after it. */
static _Atomic unsigned g_offer_entered_for_tests = 0;
static _Atomic unsigned g_offer_done_for_tests = 0;

static bool _sweep_race_take_forced_verdict_for_tests(void *node) {
  if (!atomic_exchange(&g_sweep_race_armed_for_tests, false)) return false;
  atomic_store(&g_sweep_race_forced_ctx_for_tests, node);
  return true;
}

static void _sweep_race_park_for_tests(int phase) {
  atomic_store(&g_sweep_race_parked_for_tests, phase);
  struct timespec ts = {0, 1000000};
  for (int i = 0;
       i < 10000 && atomic_load(&g_sweep_race_release_for_tests) < phase; i++)
    nanosleep(&ts, NULL);
  atomic_store(&g_sweep_race_parked_for_tests, 0);
}

void _chttp_sweep_race_arm_for_tests(void) {
  atomic_store(&g_sweep_race_forced_ctx_for_tests, NULL);
  atomic_store(&g_sweep_race_release_for_tests, 0);
  atomic_store(&g_sweep_race_parked_for_tests, 0);
  atomic_store(&g_sweep_race_armed_for_tests, true);
}

int _chttp_sweep_race_parked_phase_for_tests(void) {
  return atomic_load(&g_sweep_race_parked_for_tests);
}

void _chttp_sweep_race_release_for_tests(int phase) {
  atomic_store(&g_sweep_race_release_for_tests, phase);
}

void _chttp_sweep_race_disarm_for_tests(void) {
  atomic_store(&g_sweep_race_armed_for_tests, false);
  atomic_store(&g_sweep_race_release_for_tests, 1000);
  atomic_store(&g_sweep_race_forced_ctx_for_tests, NULL);
}

unsigned _chttp_offer_entered_count_for_tests(void) {
  return atomic_load(&g_offer_entered_for_tests);
}

unsigned _chttp_offer_done_count_for_tests(void) {
  return atomic_load(&g_offer_done_for_tests);
}

/* Arming g_continue_sweep_armed_for_tests makes the next sweep tick that
 * snapshots a ctx in CHTTP_ASYNC_AWAITING_CONTINUE park right after its
 * critical section under deadline_lock, before it judges the continue
 * window. It stays parked until the test releases it, 10 s at the most.
 * g_continue_deadline_stores_for_tests counts the stores of
 * continue_deadline and is relaxed on both sides on purpose: a test waiting
 * on it learns that a store happened but gets no ordering with the storing
 * thread, so ThreadSanitizer judges the sweep's accesses against that store
 * on their own. */
static _Atomic bool g_continue_sweep_armed_for_tests = false;
static _Atomic bool g_continue_sweep_parked_for_tests = false;
static _Atomic bool g_continue_sweep_release_for_tests = false;
static _Atomic unsigned g_continue_deadline_stores_for_tests = 0;

static void _continue_sweep_park_for_tests(void) {
  if (!atomic_exchange(&g_continue_sweep_armed_for_tests, false)) return;
  atomic_store(&g_continue_sweep_parked_for_tests, true);
  struct timespec ts = {0, 1000000};
  for (int i = 0;
       i < 10000 && !atomic_load(&g_continue_sweep_release_for_tests); i++)
    nanosleep(&ts, NULL);
  atomic_store(&g_continue_sweep_parked_for_tests, false);
}

static void _continue_deadline_stored_for_tests(void) {
  atomic_fetch_add_explicit(&g_continue_deadline_stores_for_tests, 1,
                            memory_order_relaxed);
}

void _chttp_continue_sweep_arm_for_tests(void) {
  atomic_store(&g_continue_sweep_release_for_tests, false);
  atomic_store(&g_continue_sweep_parked_for_tests, false);
  atomic_store(&g_continue_sweep_armed_for_tests, true);
}

bool _chttp_continue_sweep_parked_for_tests(void) {
  return atomic_load(&g_continue_sweep_parked_for_tests);
}

void _chttp_continue_sweep_release_for_tests(void) {
  atomic_store(&g_continue_sweep_armed_for_tests, false);
  atomic_store(&g_continue_sweep_release_for_tests, true);
}

unsigned _chttp_continue_deadline_stores_for_tests(void) {
  return atomic_load_explicit(&g_continue_deadline_stores_for_tests,
                              memory_order_relaxed);
}

/* White-box hook. While it is armed, the first sweep that meets a ctx in
 * CHTTP_ASYNC_CONNECTING with no fd and no race yet waits, before judging
 * it, first until the connect task has registered its fd and then until the
 * connect deadline has passed (2 s at most for each). That puts the deadline
 * after the connect task's check and before the sweep's judgement, which can
 * happen for real when a connect starts just before its deadline. */
static _Atomic bool g_sweep_fd_wait_armed_for_tests = false;
static _Atomic bool g_sweep_fd_wait_held_for_tests = false;

static void _sweep_fd_wait_for_tests(chttp_async_ctx_t *node) {
  if (!atomic_load(&g_sweep_fd_wait_armed_for_tests) || node->fd >= 0 ||
      node->state != CHTTP_ASYNC_CONNECTING || atomic_load(&node->race) ||
      !atomic_exchange(&g_sweep_fd_wait_armed_for_tests, false))
    return;
  atomic_store(&g_sweep_fd_wait_pending_for_tests, false);
  struct timespec ts = {0, 1000000};
  int i = 0;
  for (; i < 2000 && !atomic_load(&node->reg); i++) nanosleep(&ts, NULL);
  if (i == 2000) return;
  for (i = 0; i < 2000; i++) {
    int ms;
    _ccol_mutex_lock(node->deadline_lock);
    bool passed = !_deadline_remaining_ms(&node->connect_deadline, &ms);
    _ccol_mutex_unlock(node->deadline_lock);
    if (passed) break;
    nanosleep(&ts, NULL);
  }
  atomic_store(&g_sweep_fd_wait_held_for_tests, true);
}

void _chttp_sweep_fd_wait_arm_for_tests(void) {
  atomic_store(&g_sweep_fd_wait_held_for_tests, false);
  atomic_store(&g_sweep_fd_wait_pending_for_tests, true);
  atomic_store(&g_sweep_fd_wait_armed_for_tests, true);
}

/* Tells whether the sweep did wait as described above, and disarms it. */
bool _chttp_sweep_fd_wait_held_for_tests(void) {
  atomic_store(&g_sweep_fd_wait_armed_for_tests, false);
  atomic_store(&g_sweep_fd_wait_pending_for_tests, false);
  return atomic_load(&g_sweep_fd_wait_held_for_tests);
}

/* How many hops have a future the sweep already fulfilled while the hop is
 * alive and its ctx exists. */
size_t _chttp_async_hops_abandoned_for_tests(void) {
  return atomic_load(&g_async_hops_abandoned);
}
#endif /* RUNNING_UNIT_TESTS */

static uint64_t _async_race_tick(chttp_connect_race_t *r, bool expired);

static void _async_chain_retain(chttp_async_chain_t *chain);
static void _async_chain_release(chttp_async_chain_t *chain);
static void _async_fulfill_chain(chttp_async_chain_t *chain, ccol_retval_t rv,
                                 chttpcli_response *resp);

/* Returns the earlier of next_ns and d's instant, provided d is active and
 * the caller's check just found time left on it. */
static uint64_t _sweep_earlier(uint64_t next_ns, const chttp_deadline_t *d,
                               bool remaining) {
  if (!remaining || !d->active) return next_ns;
  uint64_t at = _timespec_to_ns(&d->deadline);
  return at < next_ns ? at : next_ns;
}

static void _client_deadline_sweep_once(void) {
  _ccol_call_once(client_deadline_bundle.once, _client_deadline_init_globals);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_sweep_ticks_for_tests, 1);
#endif
  size_t n_shutdown = 0;
  /* The earliest future instant at which some registered ctx has work for
   * the sweep; see g_sweep_next_ns. */
  uint64_t next_ns = UINT64_MAX;
  /* Chains of hops whose deadline passed before their connect started, each
   * with a reference this sweep took; see below. */
  chttp_async_chain_t *abandoned[CHTTP_DEADLINE_SWEEP_BATCH];
  size_t n_abandoned = 0;

  for (size_t s = 0; s < CHTTP_DEADLINE_SWEEP_STRIPES &&
                     n_shutdown < CHTTP_DEADLINE_SWEEP_BATCH;
       s++) {
    chttp_deadline_stripe_t *stripe = &client_deadline_bundle.stripes[s];
    _ccol_mutex_lock(stripe->mutex);
    chttp_async_ctx_t *node = stripe->head;
    while (node && n_shutdown < CHTTP_DEADLINE_SWEEP_BATCH) {
      chttp_async_ctx_t *next = node->deadline_next;
#ifdef RUNNING_UNIT_TESTS
      _sweep_fd_wait_for_tests(node);
#endif

      bool expired = false;
      /* Snapshotting state and chain, judging, and writing timed_out all
       * happen in one critical section under node->deadline_lock, and
       * _async_idle_pool_offer clears chain, sets state to IDLE and clears
       * timed_out under the same lock. So a verdict about the hop that just
       * finished can't land on the ctx after the offer made it idle. Either
       * the verdict comes first and the offer clears it, or the offer comes
       * first and our snapshot has no chain. A mark landing after the offer
       * would poison the next request reusing the connection, which would
       * report ccol_timed_out where it should retry a dead connection. The
       * field comment of node->deadline_lock explains why this leaf lock is
       * also what makes reading overall_deadline safe. */
      _ccol_mutex_lock(node->deadline_lock);
      chttp_async_state_t state = node->state;
      chttp_deadline_t continue_dl = node->continue_deadline;
      /* We compare this pointer with NULL, and only use it for a hop that
       * hasn't started its connect, whose ctx holds a reference on it (see
       * below). Any other ctx may hand its chain on while the sweep runs
       * (see the field comment of node->overall_deadline), and dereferencing
       * the pointer then is a use-after-free, which ThreadSanitizer
       * reports. */
      chttp_async_chain_t *chain_snap = node->chain;
      bool has_chain = (chain_snap != NULL);
      if (node->timed_out) {
        /* An earlier tick marked this hop and shut its fd down, and the hop
         * is here: the dispatch that ends it hasn't come. It won't come as
         * long as the reactor's dispatch pool can't allocate a task node, so
         * the sweep answers the future itself with ccol_timed_out, the same
         * way it answers a stalled lookup, and the hop ends on its own once a
         * dispatch finally runs. As long as node->chain is set under
         * deadline_lock, the ctx owns a reference on that chain (every path
         * that hands the chain on clears node->chain under this lock first,
         * and a teardown unregisters the ctx under the stripe lock this walk
         * holds before it releases the chain), so the chain is alive for the
         * reference we take here. chain->lock is a leaf. */
        if (has_chain && n_abandoned < CHTTP_DEADLINE_SWEEP_BATCH &&
            !atomic_exchange(&node->abandoned, true)) {
          atomic_fetch_add(&g_async_hops_abandoned, 1);
          _async_chain_retain(chain_snap);
          abandoned[n_abandoned++] = chain_snap;
          n_shutdown++;
        }
      } else {
        int ms;
        if (state == CHTTP_ASYNC_CONNECTING ||
            state == CHTTP_ASYNC_TLS_HANDSHAKING) {
          bool left = _deadline_remaining_ms(&node->connect_deadline, &ms);
          if (!left) {
            expired = true;
#ifdef RUNNING_UNIT_TESTS
            _sweep_note_due_for_tests(
                0, _timespec_to_ns(&node->connect_deadline.deadline));
#endif
          }
          next_ns = _sweep_earlier(next_ns, &node->connect_deadline, left);
        }
        if (!expired && has_chain) {
          bool left = _deadline_remaining_ms(&node->overall_deadline, &ms);
          if (!left) {
            expired = true;
#ifdef RUNNING_UNIT_TESTS
            _sweep_note_due_for_tests(
                0, _timespec_to_ns(&node->overall_deadline.deadline));
#endif
          }
          next_ns = _sweep_earlier(next_ns, &node->overall_deadline, left);
        }
        if (!expired && state == CHTTP_ASYNC_AWAITING_CONTINUE &&
            !atomic_load(&node->continue_decided) &&
            !atomic_load(&node->continue_msg_started))
          next_ns = _sweep_earlier(next_ns, &continue_dl,
                                   _deadline_remaining_ms(&continue_dl, &ms));
#ifdef RUNNING_UNIT_TESTS
        bool forced = !expired && has_chain && state == CHTTP_ASYNC_READING &&
                      _sweep_race_take_forced_verdict_for_tests(node);
        if (forced) {
          expired = true;
          _sweep_race_park_for_tests(1);
        }
#endif
        if (expired) node->timed_out = true;
      }
      _ccol_mutex_unlock(node->deadline_lock);
      /* We read the fd after storing timed_out. The connect task stores its
       * fd and then reads timed_out, and all four accesses are sequentially
       * consistent, so at least one side sees the other: either we see the
       * fd and shut it down, or the task sees the mark and ends the hop by
       * itself. If we read it before the mark, an fd of -1 could send a hop
       * whose connect has already started down the path for a lookup that
       * never ends, which shuts nothing down, and the hop would run with no
       * deadline at all. */
      int fd = node->fd;
#ifdef RUNNING_UNIT_TESTS
      if (state == CHTTP_ASYNC_AWAITING_CONTINUE)
        _continue_sweep_park_for_tests();
#endif

      /* While its Happy Eyeballs race runs, a ctx has no fd of its own. The
       * race holds the fds of its attempts, and a tick ends them or starts
       * the next one (see the "ASYNC HAPPY EYEBALLS" section). */
      chttp_connect_race_t *race =
          (state == CHTTP_ASYNC_CONNECTING) ? atomic_load(&node->race) : NULL;
      /* No fd and no race means the hop hasn't started its connect. It is
       * either waiting for a DNS pool thread or running its name lookup, and
       * neither can be interrupted, so we fulfil its future here with
       * ccol_timed_out at the deadline and the hop ends on its own when its
       * task sees timed_out. A hop like this is a fresh one whose ctx keeps a
       * reference on the chain read above until it is freed, and the
       * registry keeps the ctx alive while this stripe is locked, so the
       * chain is alive for the reference we take here. */
      if (expired && fd < 0 && !race && has_chain &&
          state == CHTTP_ASYNC_CONNECTING &&
          n_abandoned < CHTTP_DEADLINE_SWEEP_BATCH &&
          !atomic_exchange(&node->abandoned, true)) {
        atomic_fetch_add(&g_async_hops_abandoned, 1);
        _async_chain_retain(chain_snap);
        abandoned[n_abandoned++] = chain_snap;
      }
      if (expired) {
#ifdef RUNNING_UNIT_TESTS
        if (atomic_load(&g_sweep_race_forced_ctx_for_tests) == node)
          _sweep_race_park_for_tests(2);
#endif
        if (fd >= 0) shutdown(fd, SHUT_RDWR);
        if (race) (void)_async_race_tick(race, true);
        n_shutdown++;
      } else if (race) {
        uint64_t start_ns = _async_race_tick(race, false);
        if (start_ns < next_ns) next_ns = start_ns;
      } else if (fd >= 0 && atomic_load(&node->flip_watch) != CHTTP_FLIP_NONE) {
        /* The request send is waiting on one direction while the other one
         * may matter (see _async_upload_pause). Once that other direction is
         * ready the sweep turns the registration to it, and the dispatch
         * that follows drives the exchange from there. If another dispatch
         * already moved the ctx on, the flip costs one dispatch, the same as
         * the continue flip below. */
        unsigned char watch = atomic_load(&node->flip_watch);
        bool on_input = (watch == CHTTP_FLIP_ON_INPUT);
        struct pollfd p = {
            .fd = fd, .events = on_input ? POLLIN : POLLOUT, .revents = 0};
        if (poll(&p, 1, 0) > 0 && (p.revents & p.events) != 0) {
          ccol_event_reg reg = atomic_load(&node->reg);
          if (reg && atomic_load(&node->flip_watch) == watch)
            ccol_event_loop_modify(
                cli_engine_bundler.reactor, reg,
                on_input ? ccol_select_read : ccol_select_write);
        }
      } else if (state == CHTTP_ASYNC_AWAITING_CONTINUE) {
        /* A soft deadline for one phase of one hop, separate from
         * connect_deadline and overall_deadline above. When
         * CHTTP_100_CONTINUE_WAIT_US runs out it does NOT mean the request
         * timed out. overall_deadline is the real backstop for that, and
         * the check above always runs it since has_chain is true here as
         * well, while this branch doesn't touch that deadline at all.
         *
         * It means the wait for a "100 Continue" is over and we send the
         * body anyway (the doc comment of CHTTP_ASYNC_AWAITING_CONTINUE has
         * the whole contract with its three outcomes).
         *
         * We deliver that decision by flipping the registration to the
         * write direction. That dispatches to the
         * CHTTP_ASYNC_AWAITING_CONTINUE branch of _async_on_writable_impl,
         * which acts on it under that dispatch's own dispatch_lock, so the
         * sweep thread never changes the ctx's fields itself.
         *
         * We only peek at continue_decided here and don't claim it. The real
         * claim is made inside the dispatch the flip triggers, like every
         * other exit from _async_awaiting_continue_on_data.
         *
         * If it reads true, an on_readable dispatch in flight for the same
         * ctx has already claimed a genuine "100 Continue" or a final
         * response. Skipping the modify then saves a wasted flip to the
         * write direction, which would be harmless but would race that
         * dispatch's transition, already a correct one.
         *
         * Both continue_decided and the state are read again right before
         * the modify, so a concurrent transition can slip past only in a
         * window of a few instructions and not across the whole deadline
         * computation above.
         *
         * A write flip can land on a ctx that has left
         * CHTTP_ASYNC_AWAITING_CONTINUE, or on a later hop of a pooled ctx
         * waiting in a window of its own that continue_dl doesn't describe.
         * The cost is exactly one wasted dispatch. That dispatch judges its
         * own hop's window and sends no body while it is open, and the
         * fall-through of _async_on_writable_impl puts the read direction
         * back for every state with nothing left to flush, under the same
         * ctx->idle_lock that reactivating a pooled ctx holds. It never
         * hangs for good, never crashes and never affects any OTHER
         * request. */
        int cms;
        if (!_deadline_remaining_ms(&continue_dl, &cms) &&
            !atomic_load(&node->continue_decided) &&
            !atomic_load(&node->continue_msg_started)) {
          ccol_event_reg reg = atomic_load(&node->reg);
          if (reg && node->state == CHTTP_ASYNC_AWAITING_CONTINUE &&
              !atomic_load(&node->continue_decided)) {
            ccol_event_loop_modify(cli_engine_bundler.reactor, reg,
                                   ccol_select_write);
          }
        }
      }
      node = next;
    }
    _ccol_mutex_unlock(stripe->mutex);
  }
  /* A tick that stopped at the batch limit didn't look at every
   * registration, so the next tick runs immediately. */
  if (n_shutdown >= CHTTP_DEADLINE_SWEEP_BATCH) next_ns = 0;
  _client_deadline_note_ns(next_ns);

  /* Fulfil with no stripe locked, since a thread waiting on the future can
   * start a new request immediately and that registers in a stripe. The
   * write_fn of a streaming request never runs after its future is
   * fulfilled, because the caller may free write_ctx at that point. So we
   * close the chain's sink first and wait for a write_fn that is running
   * right now (it must not block, so the wait is short). */
  for (size_t i = 0; i < n_abandoned; i++) {
    if (abandoned[i]->write_fn) _async_sink_close(abandoned[i]);
    _async_fulfill_chain(abandoned[i], ccol_timed_out, NULL);
    _async_chain_release(abandoned[i]);
  }

  if (n_shutdown > 0) {
    _CLIENT_ENGINE_LOG_INFO("deadline sweep shut down %zu connection(s)",
                            n_shutdown);
  }
}

/* The sweep thread. It sleeps until the earliest instant at which some
 * registered request has work (g_sweep_next_ns), and at most until
 * CHTTP_DEADLINE_SWEEP_INTERVAL_MS after the end of its previous walk, and
 * that interval also paces the work with no deadline of its own (flipping a
 * stalled send, retrying a reap). A deadline registered earlier than the
 * planned wake wakes it up, but every wake only replans the wait. The walk,
 * whose cost grows with the number of requests in flight, happens only when
 * the planned instant has come, so requests starting one after another don't
 * cost a walk each. */
static void *_client_deadline_sweep_fn(void *arg) {
  (void)arg;
  t_on_sweep_thread = true;
  _ccol_call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  const uint64_t interval_ns =
      (uint64_t)CHTTP_DEADLINE_SWEEP_INTERVAL_MS * 1000000u;
  uint64_t last_walk_ns = _monotonic_now_ns();
  _ccol_mutex_lock(client_deadline_bundle.mutex);
  while (!client_deadline_bundle.stop) {
    uint64_t now_ns = _monotonic_now_ns();
    uint64_t wake_ns = last_walk_ns + interval_ns;
    /* Publish the planned instant before the last read of the earliest
     * deadline; see g_sweep_planned_ns. */
    for (;;) {
      uint64_t next_ns = atomic_load(&g_sweep_next_ns);
      uint64_t plan_ns = next_ns < wake_ns ? next_ns : wake_ns;
      atomic_store(&g_sweep_planned_ns, plan_ns > 0 ? plan_ns : 1);
      if (atomic_load(&g_sweep_next_ns) == next_ns) {
        wake_ns = plan_ns;
        break;
      }
    }
    if (wake_ns > now_ns) {
      struct timespec wake = {.tv_sec = (time_t)(wake_ns / 1000000000u),
                              .tv_nsec = (long)(wake_ns % 1000000000u)};
      _ccol_cond_var_timedwait(client_deadline_bundle.cond_var,
                               client_deadline_bundle.mutex, wake);
      /* Timeout, a wake for an earlier deadline, a stop or a spurious wake:
       * all of them just replan the wait, which walks once the instant has
       * come. */
      continue;
    }
    atomic_store(&g_sweep_planned_ns, 0);
    _ccol_mutex_unlock(client_deadline_bundle.mutex);
    atomic_store(&g_sweep_next_ns, UINT64_MAX);
#ifdef RUNNING_UNIT_TESTS
    g_sweep_walk_plan_ns_for_tests = wake_ns;
#endif
    _client_deadline_sweep_once();
    _client_engine_retry_abandoned_reap();
    /* Counting the interval from the end of the walk gives a hop this walk
     * shut down the whole interval to end through its own dispatch, before
     * a walk that no deadline asked for looks at it again. */
    last_walk_ns = _monotonic_now_ns();
    _ccol_mutex_lock(client_deadline_bundle.mutex);
  }
  atomic_store(&g_sweep_planned_ns, 0);
  _ccol_mutex_unlock(client_deadline_bundle.mutex);
  return NULL;
}

/* Starts the sweep thread. _client_engine_acquire calls it next to the code
 * that starts the engine's reactor thread (that function also shows how a
 * failure is rolled back). */
static ccol_retval_t _client_deadline_sweep_start(void) {
  client_deadline_bundle.stop = false;
  int rc = _ccol_thread_create(client_deadline_bundle.thread,
                               _client_deadline_sweep_fn, NULL);
  return (rc == 0) ? ccol_success : ccol_unexpected_failure;
}

/* Signals the sweep thread and joins it. _client_engine_reaper_fn calls it
 * next to the join of the engine's reactor thread; the comment of that
 * function explains why teardown always happens on a dedicated reaper thread
 * and never inline from a reactor callback. */
static void _client_deadline_sweep_stop_and_join(void) {
  _ccol_call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  _ccol_mutex_lock(client_deadline_bundle.mutex);
  client_deadline_bundle.stop = true;
  _ccol_cond_var_broadcast(client_deadline_bundle.cond_var);
  _ccol_mutex_unlock(client_deadline_bundle.mutex);
  _ccol_thread_join(client_deadline_bundle.thread);
}

#if CCOL_FORK_SAFETY_REQUIRED
/* ========================================================================== */
/*                    FORK SAFETY (pthread_atfork)                            */
/* ========================================================================== */

/*
 * fork() duplicates only the calling thread, so any lock that another thread
 * of this module held at that instant stays locked for good in the child.
 * The engine has threads of its own (the reaper, the sweep and the reactor
 * threads) that take cli_engine_bundler.mutex, the deadline sweep's
 * lifecycle mutex and its stripe mutexes, and a process that destroys its
 * last client and forks right away (a supported pattern) can fork while its
 * engine's reaper is running.
 *
 * So this handler waits for such a reaper to finish, which keeps the child
 * from inheriting an engine halfway through its teardown, and then joins it.
 * The reaper keeps running for a bit after it clears `stopping`: it closes
 * the fallback logger, which frees memory through the engine's allocator,
 * and then its thread exits. A fork inside that window hands the child
 * whatever lock the reaper holds there, in the engine's allocator or in a
 * sanitizer runtime's allocator, and the child's first allocation waits on
 * it for ever. After the join no engine thread exists at the fork. Then the
 * handler takes every lock of this module, in the same order as every
 * ordinary call does: the engine mutex, the sweep's lifecycle mutex, then
 * the stripes by index. No code holds two stripes at once, and no code
 * takes the engine mutex or the lifecycle mutex while it holds a stripe, so
 * there is no inversion in that order. It runs before the prepare handlers
 * of ccol_event_loop, ctpool and clog; see _client_engine_globals_init.
 *
 * The wait can't deadlock. A reaper runs only when no reference to the
 * engine is left, and the only application code that runs on a thread the
 * reaper joins is a streaming request's write_fn, which holds a reference.
 * The join can't deadlock either: once the reaper has cleared `stopping` it
 * takes no lock of this module, and the clog locks its logger close needs
 * are free here because this handler runs before clog's. The wait and the
 * join only apply in the process that built the engine, since a process
 * that inherited an engine has no reaper to wait for.
 */
static void _chttpcli_atfork_prepare(void) {
#ifdef RUNNING_UNIT_TESTS
  _ccol_atfork_order_record(_ccol_atfork_module_chttpclient);
#endif
  _ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  _ccol_call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  _ccol_mutex_lock(cli_engine_bundler.mutex);
  if (atomic_load(&g_client_engine_pid) == getpid()) {
    while (cli_engine_bundler.stopping)
      _ccol_cond_var_wait(cli_engine_bundler.stopped_cv,
                          cli_engine_bundler.mutex);
    _client_engine_join_reaper_if_needed_locked();
  }
  _ccol_mutex_lock(client_deadline_bundle.mutex);
  for (size_t i = 0; i < CHTTP_DEADLINE_SWEEP_STRIPES; i++)
    _ccol_mutex_lock(client_deadline_bundle.stripes[i].mutex);
}

static void _chttpcli_atfork_unlock_all(void) {
  for (size_t i = CHTTP_DEADLINE_SWEEP_STRIPES; i-- > 0;)
    _ccol_mutex_unlock(client_deadline_bundle.stripes[i].mutex);
  _ccol_mutex_unlock(client_deadline_bundle.mutex);
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
}

static void _chttpcli_atfork_parent(void) { _chttpcli_atfork_unlock_all(); }

/* The thread that called fork() holds every lock here, in the child just as
 * in the parent, so a plain unlock is right on both sides. Before that, the
 * child forgets an engine whose threads it didn't inherit; see
 * _client_engine_forget_inherited_locked, which the acquire path runs too
 * for a build without this handler. */
static void _chttpcli_atfork_child(void) {
  _client_engine_forget_inherited_locked();
  _chttpcli_atfork_unlock_all();
}
#endif /* CCOL_FORK_SAFETY_REQUIRED */

/* Deep copy of a chmap(char* -> char*) header map. A redirect chain gets its
 * own copy of the original request's headers this way, so the caller can
 * free its chttp_request_t as soon as chttpclient_do_async returns, long
 * before some later hop serializes those headers again.
 *
 * Returns NULL when it runs out of memory. A NULL input isn't an error, it
 * just means there are no headers, and it returns NULL for that too, so on
 * their own the two cases look the same. A caller that needs to tell them
 * apart checks the source map first, as every other call site in this file
 * does, and all of them treat "no headers" as normal. */
static chmap _clone_headers_map(ccol_memmgmt_procs_t *mp, chmap src) {
  char *err = NULL;
  chmap dst = chmap_create_full(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,
                                ccol_string, ccol_string, mp, NULL, NULL, &err);
  if (!dst) return NULL;
  /* We walk with the insertion-order cursor, which allocates nothing and
   * can't fail, so we either get a complete copy or no copy. A copy that
   * quietly lost headers would send the request without them. It also keeps
   * the order in which the caller set them. */
  for (const ccol_chmap_entry_ref *e = ccol_chmap_oldest_entry(src); e;) {
    const cmap_pair *k, *v;
    e = ccol_chmap_entry_read(e, &k, &v);
    if (chmap_insert_elem(dst, k, v) != ccol_success) {
      __chmap_destroy(dst);
      return NULL;
    }
  }
  return dst;
}

/*
 * Allocates the shared state of a whole redirect chain (see the comment
 * above chttp_async_chain_t).
 *
 * On success it takes ownership of tls_ctx and frees it once, with
 * ctls_ctx_release, when the chain is freed. On failure tls_ctx stays with
 * the caller, who must free it.
 *
 * req_headers, body_data and body_content_type are NOT kept by reference.
 * Whatever it needs from them is deep-copied, and the originals are never
 * kept.
 */
static chttp_async_chain_t *_async_chain_create(
    ccol_memmgmt_procs_t *mp, struct chttpclient *cli, ctpool_future *future,
    chmap req_headers, const void *body_data, size_t body_len,
    const char *body_content_type, const char *initial_origin_key,
    ctls_ctx_t *tls_ctx, bool tls_ctx_usable, bool verify_host,
    uint64_t tls_generation, uint64_t connect_timeout_us,
    uint64_t request_timeout_us, size_t max_response_body_size,
    chttpcli_write_fn write_fn, void *write_ctx, bool expect_continue,
    bool prevent_tls_downgrade_on_redirect) {
  chttp_async_chain_t *chain =
      (chttp_async_chain_t *)_ccol_mem_calloc(mp, 1, sizeof(*chain));
  if (!chain) return NULL;
  if (_ccol_mutex_init(chain->lock) != 0) {
    _ccol_mem_free(mp, chain);
    return NULL;
  }
  chain->mp = mp;
  chain->cli = cli;
  chain->future = future;
  chain->tls_ctx = tls_ctx;
  chain->tls_ctx_usable = tls_ctx_usable;
  chain->verify_host = verify_host;
  chain->tls_generation = tls_generation;
  chain->connect_timeout_us = connect_timeout_us;
  chain->overall_deadline = _deadline_make(request_timeout_us);
  chain->max_response_body_size = max_response_body_size;
  chain->write_fn = write_fn;
  chain->write_ctx = write_ctx;
  chain->expect_continue = expect_continue;
  chain->prevent_tls_downgrade_on_redirect = prevent_tls_downgrade_on_redirect;

  if (req_headers) {
    chain->req_headers = _clone_headers_map(mp, req_headers);
    if (!chain->req_headers) goto fail;
  }
  if (body_data && body_len > 0) {
    chain->body_data = _ccol_mem_alloc(mp, body_len);
    if (!chain->body_data) goto fail;
    memcpy(chain->body_data, body_data, body_len);
    chain->body_len = body_len;
  }
  if (body_content_type) {
    chain->body_content_type = ccol_strdup(mp, body_content_type);
    if (!chain->body_content_type) goto fail;
  }
  if (initial_origin_key) {
    chain->initial_origin_key = ccol_strdup(mp, initial_origin_key);
    if (!chain->initial_origin_key) goto fail;
  }

  /* The chain is live and about to go to _async_submit_hop, so we count it
   * in cli's async in-flight total (see that field's comment on struct
   * chttpclient), which is what __chttpclient_destroy waits on. The one
   * matching subtraction is in _async_chain_release, once this chain's
   * refcount reaches zero. */
  _ccol_mutex_lock(cli->async_count_lock);
  cli->async_in_flight_count++;
  _ccol_mutex_unlock(cli->async_count_lock);

  return chain;

fail:
  if (chain->req_headers) __chmap_destroy(chain->req_headers);
  _ccol_mem_free(mp, chain->body_data);
  _ccol_mem_free(mp, chain->body_content_type);
  _ccol_mem_free(mp, chain->initial_origin_key);
  _ccol_mutex_destroy(chain->lock);
  _ccol_mem_free(mp, chain);
  return NULL;
}

/*
 * A response and an async result outlive the client that made them, since
 * the caller may free either of them after chttpclient_destroy, when the
 * client's procs are gone. So each one carries its own copy of the
 * allocator procs, in the same allocation right after the struct, with
 * _m_procs pointing at it. A NULL mp means the default allocator and there
 * is nothing to copy. The free functions read the copy into a local before
 * freeing the block it lives in.
 */
static void *_alloc_with_procs_copy(ccol_memmgmt_procs_t *mp, size_t size,
                                    ccol_memmgmt_procs_t **copy_out) {
  size_t off = (size + _Alignof(ccol_memmgmt_procs_t) - 1) &
               ~(size_t)(_Alignof(ccol_memmgmt_procs_t) - 1);
  char *block = (char *)_ccol_mem_calloc(
      mp, 1, mp ? off + sizeof(ccol_memmgmt_procs_t) : size);
  if (!block) return NULL;
  *copy_out = NULL;
  if (mp) {
    ccol_memmgmt_procs_t *copy = (ccol_memmgmt_procs_t *)(block + off);
    *copy = *mp;
    *copy_out = copy;
  }
  return block;
}

static chttpcli_response *_resp_alloc(ccol_memmgmt_procs_t *mp) {
  ccol_memmgmt_procs_t *copy;
  chttpcli_response *resp = (chttpcli_response *)_alloc_with_procs_copy(
      mp, sizeof(chttpcli_response), &copy);
  if (resp) resp->_m_procs = copy;
  return resp;
}

static chttpcli_async_result_t *_async_result_alloc(ccol_memmgmt_procs_t *mp) {
  ccol_memmgmt_procs_t *copy;
  chttpcli_async_result_t *result =
      (chttpcli_async_result_t *)_alloc_with_procs_copy(
          mp, sizeof(chttpcli_async_result_t), &copy);
  if (result) result->_m_procs = copy;
  return result;
}

/* Forward declaration for the backstop in _async_chain_release. The
 * function itself comes further below, once the code that builds a
 * chttp_async_result_t is in scope. */
static void _async_fulfill_chain(chttp_async_chain_t *chain, ccol_retval_t rv,
                                 chttpcli_response *resp);

/* Adds one live-hop reference to chain, and every call MUST be matched by
 * exactly one _async_chain_release. The caller knows the chain is live:
 * either we just created it and the refcount goes from 0 to 1, or a
 * redirect handoff retains it again while the old hop's ctx is alive. */
static void _async_chain_retain(chttp_async_chain_t *chain) {
  _ccol_mutex_lock(chain->lock);
  chain->refcount++;
  _ccol_mutex_unlock(chain->lock);
}

/*
 * Drops one live-hop reference. Reaching zero means no further hop took
 * over from the last one we tore down.
 *
 * That is the one reliable place for two things. One is a backstop that
 * fulfils the future, and does nothing if some hop already did, since it
 * goes through the same fulfilled guard that _async_fulfill_chain checks.
 * The other is freeing the chain itself, together with the one engine
 * reference the chain holds for its whole life.
 */
static void _async_chain_release(chttp_async_chain_t *chain) {
  if (!chain) return;
  _ccol_mutex_lock(chain->lock);
  int remaining = --chain->refcount;
  _ccol_mutex_unlock(chain->lock);
  if (remaining > 0) return;

  struct chttpclient *cli = chain->cli;

  _async_fulfill_chain(chain, ccol_http_transfer_aborted, NULL);
  if (chain->tls_ctx) ctls_ctx_release(chain->tls_ctx);
  if (chain->req_headers) __chmap_destroy(chain->req_headers);
  _ccol_mem_free(chain->mp, chain->body_data);
  _ccol_mem_free(chain->mp, chain->body_content_type);
  _ccol_mem_free(chain->mp, chain->carried_auth);
  _ccol_mem_free(chain->mp, chain->carried_auth_origin);
  _ccol_mem_free(chain->mp, chain->initial_origin_key);
  _ccol_mutex_destroy(chain->lock);
  _ccol_mem_free(chain->mp, chain);
  _client_engine_release_chain();

  /* Matches the addition in _async_chain_create. The field comment of cli's
   * async_in_flight_count explains why we use a dedicated leaf lock here
   * and not cli->lock: this function runs from inside ccol_event_loop
   * dispatch callbacks as often as from a safe synchronous context. */
  _ccol_mutex_lock(cli->async_count_lock);
  if (cli->async_in_flight_count > 0) cli->async_in_flight_count--;
  if (cli->async_in_flight_count == 0)
    _ccol_cond_var_broadcast(cli->async_count_drained);
  _ccol_mutex_unlock(cli->async_count_lock);
}

static chttp_async_ctx_t *_async_ctx_create(ccol_memmgmt_procs_t *mp) {
  chttp_async_ctx_t *ctx =
      (chttp_async_ctx_t *)_ccol_mem_calloc(mp, 1, sizeof(*ctx));
  if (!ctx) return NULL;
  ctx->mp = mp;
  ctx->fd = -1;
  /* _ccol_mem_calloc above already leaves pending_app_teardown and
   * hop_completed at their right default of false, so there's nothing to
   * set for them. */
  if (_ccol_mutex_init(ctx->idle_lock) != 0) {
    _ccol_mem_free(mp, ctx);
    return NULL;
  }
  if (_ccol_mutex_init(ctx->deadline_lock) != 0) {
    _ccol_mutex_destroy(ctx->idle_lock);
    _ccol_mem_free(mp, ctx);
    return NULL;
  }
  return ctx;
}

/*
 * The one reliable place where a ctx, the connection state of one hop, is
 * freed. _async_ctx_teardown calls it through _async_ctx_finish and
 * _async_idle_ctx_finish, the two explicit terminal-teardown helpers below,
 * which give this module a teardown point that runs exactly once (the
 * comment at the top of the "ASYNC CONNECTION STATE MACHINE" section says
 * why ccol_event_loop needs an explicit point and not an implicit one). A
 * failing path also calls it directly, for a failure that happens before
 * the connection was ever registered with the reactor.
 *
 * Every field can be freed or destroyed unconditionally. A field whose
 * ownership moved elsewhere (headers and bb.buf go to a successfully built
 * response) is NULL or cleared where the transfer happened, and
 * _ccol_mem_free, _parse_ctx_free_fields, a NULL tls, a NULL reg and a
 * negative fd are all no-ops.
 *
 * ctx->chain is NOT touched here because it is shared by the whole chain;
 * _async_ctx_teardown pairs this call with the matching release of the
 * chain.
 *
 * ccol_event_loop_remove and close() are always called here, and both are
 * idempotent and safe, so no caller needs to do them first. That's the same
 * treatment ctx->tls and ctx->wire get, which are always safe to free here
 * whatever the caller did already.
 *
 * _client_deadline_unregister runs FIRST, strictly before close(ctx->fd),
 * and that order is load-bearing (see the comment of the "ASYNC DEADLINE
 * SWEEP" section). The deadline sweep's shutdown() works on an fd, and it
 * is safe from a race over a reused fd only because every teardown path
 * leaves that registry before it can close its fd, which an unrelated
 * connection could otherwise pick up.
 */
/* Gets ctx->parser ready for the ctx's next response message. A parser the
 * ctx has used before may hold the heap buffer of a long header line, and
 * that goes first; a ctx fresh from _ccol_mem_calloc has a zeroed parser,
 * which the release doesn't touch. Response header lines can be up to
 * CHTTP1_MAX_SPILL_LINE_LEN bytes; the parser's fixed buffer handles the
 * common case and only a longer line allocates. */
static void _async_parser_start(chttp_async_ctx_t *ctx) {
  _ccol_call_once(client_http1_settings_bundler.once, _init_chttp1_settings);
  chttp1_parser_release(&ctx->parser);
  chttp1_parser_init(&ctx->parser, &client_http1_settings_bundler.settings);
  ctx->parser.data = &ctx->pctx;
  (void)chttp1_parser_enable_line_spill(&ctx->parser, ctx->mp);
}

static void _async_race_detach(chttp_async_ctx_t *ctx);
static ccol_retval_t _async_reg_modify(chttp_async_ctx_t *ctx,
                                       ccol_select_dir dir);
static void _async_ctx_handle_if_abandoned(chttp_async_ctx_t *ctx);
static inline bool _async_upload_may_resume(const chttp_async_ctx_t *ctx);
static void _async_upload_resume(chttp_async_ctx_t *ctx);

static void _async_ctx_destroy_now(chttp_async_ctx_t *ctx) {
  /* A short lock and unlock, held across nothing below. Taking it makes
   * sure the window of _async_connect_task after ccol_event_loop_add is
   * over (see the comment of that function, right after it takes this same
   * idle_lock). Without it that window can be open while we free ctx out
   * from under it.
   *
   * Every other call site of _async_ctx_free and _async_ctx_teardown has
   * already dropped idle_lock before calling in here, if it took it at
   * all, so this can't deadlock against a caller. */
  _ccol_mutex_lock(ctx->idle_lock);
  _ccol_mutex_unlock(ctx->idle_lock);
  _client_deadline_unregister(ctx); /* A no-op for a ctx we never
                                     * registered, and it MUST run before
                                     * the close() below. */
  /* The sweep only marks a registered ctx, so it can't mark this one. */
  if (atomic_load(&ctx->abandoned))
    atomic_fetch_sub(&g_async_hops_abandoned, 1);
  _async_race_detach(ctx); /* Must come after the unregister, since the
                            * sweep reads the race of a registered ctx. */
  if (ctx->reg) ccol_event_loop_remove(cli_engine_bundler.reactor, ctx->reg);
  ccol_event_reg room = atomic_exchange(&ctx->room_reg, CCOL_EVENT_REG_INVALID);
  if (room) ccol_event_loop_remove(cli_engine_bundler.reactor, room);
  if (ctx->tls) ctls_conn_destroy(ctx->tls);
  if (ctx->fd >= 0) close(ctx->fd);
  chttp1_parser_release(&ctx->parser);
  _ccol_mutex_destroy(ctx->idle_lock);
  _ccol_mutex_destroy(ctx->deadline_lock);
  _ccol_mem_free(ctx->mp, ctx->wire);
  _ccol_mem_free(ctx->mp, ctx->continue_carry);
  _ccol_mem_free(ctx->mp, ctx->unix_socket_path);
  _ccol_mem_free(ctx->mp, ctx->host);
  _ccol_mem_free(ctx->mp, ctx->addrs);
  _ccol_mem_free(ctx->mp, ctx->path_and_query);
  _ccol_mem_free(ctx->mp, ctx->origin_key);
  _parse_ctx_free_fields(&ctx->pctx);
  _ccol_mem_free(ctx->mp, ctx->bb.buf);
  _ccol_mem_free(ctx->mp, ctx);
}

/*
 * The one place that physically frees a ctx. It's safe to call without any
 * condition because of this module's invariant (see the field comment of
 * pending_app_teardown and the rule it upholds): no application thread
 * ever frees a registered ctx.
 *
 * So we only get here in two situations. Either we never registered ctx
 * with the reactor at all (a failure before the connect), and nothing else
 * can reference it, or the call runs inside a reactor dispatch callback.
 * In the second case ccol_event_loop already makes sure such a callback
 * never runs concurrently with another dispatch for the same registration,
 * through its entry->dispatch_lock and the entry->refcount invariant of at
 * most one job in flight per entry.
 *
 * Neither case needs a reference count of its own on ctx, and ctx doesn't
 * have one.
 */
static void _async_ctx_free(chttp_async_ctx_t *ctx) {
  if (!ctx) return;
  _async_ctx_destroy_now(ctx);
}

/* Frees the per-connection state of one hop together with that hop's chain
 * reference. Every terminal path of a ctx ends here, whether it's a
 * dispatch-driven teardown or an early failure before we ever registered
 * the ctx with the reactor.
 *
 * chain goes into a local first because _async_ctx_free frees ctx itself. */
static void _async_ctx_teardown(chttp_async_ctx_t *ctx) {
  chttp_async_chain_t *chain = ctx->chain;
  _async_ctx_free(ctx);
  _async_chain_release(chain);
}

/*
 * Hands a terminal result to the caller's future exactly once, guarded by
 * chain->fulfilled under chain->lock. The comment of that field explains
 * why it needs a lock and why a plain bool isn't enough: a redirect chain
 * can run the callbacks of two hops concurrently.
 *
 * Calling this from more than one exit path is fine (an error path and
 * then the backstop call of _async_chain_release, for example), only the
 * first call has any effect.
 *
 * The result struct is chain->result, which _chttp_do_async_internal
 * allocated before the request was queued. We allocate nothing here, so
 * every request reports its own code, including an allocation failure on
 * the reactor.
 */
static void _async_fulfill_chain(chttp_async_chain_t *chain, ccol_retval_t rv,
                                 chttpcli_response *resp) {
  _ccol_mutex_lock(chain->lock);
  bool already = chain->fulfilled;
  chain->fulfilled = true;
  chttpcli_async_result_t *result = chain->result;
  chain->result = NULL;
  _ccol_mutex_unlock(chain->lock);
  if (already) {
    chttpclient_resp_free(resp);
    return;
  }
  _client_async_request_settled();
  result->rv = rv;
  result->resp = resp;
  ctpool_future_fulfill(chain->future, result);
}

static void _async_fulfill(chttp_async_ctx_t *ctx, ccol_retval_t rv,
                           chttpcli_response *resp) {
  /* Mark the ctx terminal so the guard at the top of _async_on_readable and
   * _async_on_writable skips any further work for it (see the field comment
   * of ctx->hop_completed). A stray extra callback after this point has to
   * do nothing, and in particular it must not run logic that isn't
   * idempotent, like _async_handle_redirect or a second
   * ctls_conn_handshake_step on a handshake that already failed. */
  ctx->hop_completed = true;
  _async_fulfill_chain(ctx->chain, rv, resp);
}

/* Builds the chttpcli_response from the finished parse, the same way the
 * response-building code at the end of chttp_do_internal does it in Tier 1.
 *
 * Ownership of ctx->pctx.headers and ctx->bb.buf moves out of ctx and both
 * fields are set to NULL there. We call this BEFORE the idle-pool-offer
 * path of _async_finish_connection, which resets ctx for its idle life and
 * would otherwise free exactly those fields. The handling of CHTTP1_PAUSED
 * in _async_process_reading_data shows why this order (build the response,
 * finish the connection, then fulfil the future) matters for its own
 * reasons too.
 *
 * For a streaming request ctx->chain->write_fn is set, and the body bytes
 * went to the caller's callback as they came off the wire (see how
 * _async_submit_hop and _async_retry_hop wire the sink). ctx->bb was never
 * the sink, so there's nothing to move into resp->body and it stays NULL
 * with a length of 0, which is how chttpclient_do_streaming of Tier 1
 * leaves chttpcli_response.body too.
 *
 * We parse the headers anyway, because redirect detection needs them
 * whatever the sink is, but here they are freed instead of exposed. That
 * matches the documented contract of chttpclient_do_streaming, where the
 * response headers aren't accessible. */
static chttpcli_response *_async_build_response(chttp_async_ctx_t *ctx) {
  chttpcli_response *resp = _resp_alloc(ctx->mp);
  if (!resp) return NULL;
  resp->status_code = ctx->pctx.status_code;
  if (ctx->chain->write_fn) {
    _parse_ctx_free_fields(&ctx->pctx);
  } else {
    resp->body = ctx->bb.buf;
    resp->body_len = ctx->bb.len;
    resp->headers = ctx->pctx.headers;
    ctx->pctx.headers = NULL; /* Ownership moves to resp. */
    resp->_field_lines = ctx->pctx.field_lines;
    ctx->pctx.field_lines = NULL; /* Ownership moves to resp. */
    ctx->bb.buf = NULL;           /* Ownership moves to resp. */
  }
  return resp;
}

/* Builds the response and fulfils the future with it in one go, for the
 * completion path that an EOF drives. That path never pools its connection
 * (a peer that closes to mark the end of the body isn't offering
 * keep-alive), so there's no ordering hazard with _async_finish_connection
 * to worry about. */
static void _async_fulfill_success(chttp_async_ctx_t *ctx) {
  chttpcli_response *resp = _async_build_response(ctx);
  _async_fulfill(ctx, resp ? ccol_success : ccol_not_enough_memory, resp);
}

/* ========================================================================== */
/*                         ASYNC IDLE POOL (TIER 2)                           */
/* ========================================================================== */

/*
 * The keep-alive idle pool of Tier 2, a
 * chmap(char *origin_key -> cvec of chttp_async_ctx_t*). Every chttpcli
 * keeps its own in cli->idle_pools_async.
 *
 * Shape and policy are the ones of idle_pools, _idle_pool_take and
 * _idle_pool_offer in Tier 1, with the same CHTTP_MAX_IDLE_PER_ORIGIN,
 * CHTTP_MAX_IDLE_TOTAL and CHTTP_IDLE_MAX_AGE_MS caps, but the mechanism
 * has to be different.
 *
 * A pooled connection here stays attached to the shared reactor, since you
 * can't detach the registration of a live connection without closing it.
 * Pooling a ctx therefore means moving it to CHTTP_ASYNC_IDLE, and the
 * IDLE-state branches of the dispatch callbacks keep driving it there.
 *
 * Any activity on such a connection, the natural EOF or hangup of a dead
 * one included, makes it unusable, and we tear it down through the same
 * on_close path as any failed connection, just entered from another state.
 *
 * We can't peek at a reactor-owned fd synchronously the way the MSG_PEEK
 * liveness probe of Tier 1 does, so a connection can die in the short
 * window between the pop out of the pool and the actual reuse. The
 * retry-once mechanism at the point of use catches that case, through
 * reused and any_bytes_read (see _async_retry_hop), and the two together
 * give Tier 2 the same effective guarantee as the probe-and-retry pair of
 * Tier 1.
 *
 * A pooled ctx holds its OWN engine reference. _async_idle_pool_offer takes
 * it, and whichever path claims the ctx next frees it: the reuse path of
 * _async_idle_pool_take or the IDLE-state on_close path.
 *
 * It's separate from any chain's reference because the chain that produced
 * this connection is already fulfilled and torn down (or about to be),
 * while the pooled connection needs to keep the engine alive for as long as
 * it sits in the pool.
 */

#ifdef RUNNING_UNIT_TESTS
/*
 * Fault-injection flags for the tests, nothing else reads them. Each one is
 * consumed (and reset to false) the first time we read it, so a failure
 * injected by one test can't leak into an unrelated test later in the same
 * process. The setters live under "White-box test helpers" further below.
 *
 * We need the first two because the usual allocator-failure injection (the
 * g_hop_fail_mp pattern of tests.c) can't reach the branches they simulate.
 *
 * The cvector_push_back of _async_idle_pool_offer can never fail here.
 * CHTTP_MAX_IDLE_PER_ORIGIN is 4, the same as cvector's minimum_capacity,
 * so the list of an origin is always created with room for every element
 * that the has_room check of _idle_pool_offer lets through. That push_back
 * never grows the array with a realloc, so an allocator that runs out of
 * memory can't make it fail.
 *
 * The ccol_event_loop_modify on the reused-connection path of
 * _async_submit_hop is in the same position. It allocates nothing itself,
 * and none of its other failure conditions (bad arguments, a selectable
 * that isn't an fd, a registration we already removed, a target direction
 * that is taken) can happen for a connection we just popped from the idle
 * pool, because such a connection is always registered for read only.
 *
 * Both branches are real code all the same. A later change can raise
 * CHTTP_MAX_IDLE_PER_ORIGIN past 4, or give ccol_event_loop a new failure
 * mode, and either branch turns into a use-after-free if it gets the
 * ownership of a ctx wrong. The hooks are there only to keep both branches
 * under permanent, deterministic test coverage.
 *
 * The third flag has the same reason behind it and is about how
 * _async_on_readable_impl tells a hard transport error from a real EOF (see
 * the comment of that function).
 *
 * In production a genuine TCP RST, or a fatal TLS-level error, can arrive
 * in a SEPARATE dispatch from the data in front of it, and its timing
 * against data we already buffered and consumed is an OS-level race that
 * no test can pin down over a real socket. A graceful close is another
 * matter, the eof_delimited_body_without_content_length tests of this file
 * exercise that one reliably.
 *
 * The hook makes the NEXT read dispatch for a ctx behave as if recv() or
 * ctls_conn_read() returned -1 with ECONNRESET, without touching a real
 * socket at all. It only applies to a ctx that already had at least one
 * real successful read, never to the very first read of a fresh or reused
 * connection.
 */
static _Atomic bool g_force_offer_push_fail_for_tests = false;
static _Atomic bool g_force_reactivate_fail_for_tests = false;
static _Atomic bool g_force_async_hard_read_error_for_tests = false;

/*
 * A fourth flag, for the one race that the write-direction fall-through of
 * _async_on_writable_impl exists for.
 *
 * To deliver a "100 Continue" timeout the deadline sweep samples a ctx's
 * state and then flips its registration to the write direction. A dispatch
 * on another thread can move the same ctx out of
 * CHTTP_ASYNC_AWAITING_CONTINUE in between, so the flip may land on a ctx
 * that has armed its own read direction already and has nothing to write.
 *
 * Who wins is OS scheduling, which no test can pin down over a real
 * socket, so the flag produces the losing order directly. The next
 * plain-HTTP hop that finishes writing its request arms the write
 * direction again right after arming read, and that's exactly the state
 * the race leaves behind.
 *
 * g_stray_write_arm_restores_for_tests counts how many times the
 * fall-through really restored the read direction. The increment sits on
 * a reactor dispatch path that several threads run at once, so it's a
 * relaxed atomic rather than a plain counter. Read it only once the
 * request under test has completed.
 *
 * g_force_stray_idle_write_arm_for_tests does the same one transition
 * later, for a connection that made it into the idle pool rather than one
 * reading its response, since the state the sweep sampled can be that
 * stale as well. A pooled connection has answered its caller already, so
 * the worst this variant does is make the shared reactor spin on a
 * dispatch that does nothing, which is why it's the bounded half of the
 * pair.
 */
static _Atomic bool g_force_stray_write_arm_for_tests = false;
static _Atomic bool g_force_stray_idle_write_arm_for_tests = false;
static _Atomic unsigned long long g_stray_write_arm_restores_for_tests = 0;

/*
 * Widens the window on both sides of the "Expect: 100-continue" carry-over
 * replay in _async_upload_pause.
 *
 * The replay can fulfil its chain and drop the chain's last reference,
 * which is what lets a chttpclient_destroy waiting on async_in_flight_count
 * go on. After that the replay frees the bytes through the client's own
 * allocator, and that destroy is free to have taken it away by then.
 *
 * How much of the destroy fits in between is pure scheduling, so the delay
 * makes all of it fit. The leading half lets the destroy reach its wait
 * before the replay starts and the trailing half lets it run to the end
 * before the free. Zero (the default) turns both halves off.
 */
static _Atomic long g_continue_carry_replay_delay_ms_for_tests = 0;
#ifdef RUNNING_UNIT_TESTS
/* See _chttpclient_arm_stray_write_at_take_for_tests. */
static _Atomic bool g_stray_write_at_take_for_tests = false;
/* See _chttpclient_arm_stray_write_before_take_for_tests: 0 off, 1 armed,
 * 2 the take armed the write direction, 3 the dispatch classified the ctx
 * as IDLE and waits, 4 the take moved the ctx to WRITING. */
static _Atomic int g_stray_before_take_stage_for_tests = 0;

/* Waits, for at most 2 s, until the stage reaches at least `stage`. */
static void _stray_before_take_wait_for_tests(int stage) {
  for (int i = 0; i < 2000; i++) {
    if (atomic_load(&g_stray_before_take_stage_for_tests) >= stage) return;
    struct timespec ts = {0, 1000000L};
    nanosleep(&ts, NULL);
  }
}
#endif

static void _chttp_test_continue_carry_replay_delay(void) {
  long ms = atomic_load(&g_continue_carry_replay_delay_ms_for_tests);
  if (ms <= 0) return;
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

/*
 * Holds the IDLE branch of _async_on_readable_impl once, between deciding
 * that the ctx is idle and reading, so a test can have
 * _async_idle_pool_take grab the ctx for a new hop inside that window. That
 * gives the order where the take beats the dispatch that evicts the ctx.
 * 0 is off, 1 armed, 2 holding and 3 released, and the hold ends after 5
 * seconds whatever the test does.
 *
 * Only a dispatch that finds input waiting on the socket is held. A stale
 * or duplicate dispatch of an idle ctx (the IDLE branch documents those)
 * can run before the peer has sent anything, and holding that one would
 * let the take run while the input is in flight, where no probe sees it.
 * The test would then measure the in-flight case and not the race.
 */
static _Atomic int g_idle_probe_hold_for_tests = 0;

static void _chttp_test_idle_probe_hold(int fd) {
  int pending = 0;
  if (atomic_load(&g_idle_probe_hold_for_tests) != 1) return;
  if (ioctl(fd, FIONREAD, &pending) != 0 || pending <= 0) return;
  int armed = 1;
  if (!atomic_compare_exchange_strong(&g_idle_probe_hold_for_tests, &armed, 2))
    return;
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
  for (int i = 0; i < 5000 && atomic_load(&g_idle_probe_hold_for_tests) == 2;
       i++)
    nanosleep(&ts, NULL);
  atomic_store(&g_idle_probe_hold_for_tests, 0);
}
#endif /* RUNNING_UNIT_TESTS */

/* Decrements idle_total_count_async, and once it hits zero wakes every
 * waiter on idle_async_drained, i.e. __chttpclient_destroy waiting for all
 * pooled connections to be torn down. Only call it with cli->lock held. */
static void _async_idle_count_dec_locked(struct chttpclient *cli) {
  if (cli->idle_total_count_async > 0) cli->idle_total_count_async--;
  if (cli->idle_total_count_async == 0)
    _ccol_cond_var_broadcast(cli->idle_async_drained);
}

/* Removes `target` from its origin's idle list if the list holds it, by
 * swapping it with the last element (cvector only has push_back and
 * pop_back), and then subtracts one from the count.
 *
 * When target isn't there it does nothing and returns false. That happens
 * when the staleness eviction of _async_idle_pool_take popped it just
 * before the IDLE-state readable or error dispatch ALSO saw it die
 * naturally, and both sides can call this with no condition. Only call it
 * with cli->lock held. */
/* We deliberately do NOT call _async_idle_count_dec_locked here. The
 * decrement, and the broadcast of idle_async_drained that it can trigger
 * at zero, belong to the caller, which holds both back until the ctx is
 * REALLY freed.
 *
 * Decrementing at removal time opens a use-after-free.
 * __chttpclient_destroy would see the count reach zero and free cli, and
 * cli->m_procs with it, while _async_idle_ctx_finish (the only caller of
 * this function) is in the middle of its teardown on a reactor worker
 * thread and reads that freed cli->m_procs through ctx->mp in its own
 * _async_ctx_free call.
 *
 * ThreadSanitizer reports it and valgrind doesn't, and it only shows up
 * under a workload that cycles many connections through a real death that
 * the server starts. The comment of _async_idle_ctx_finish has the
 * ordering this depends on. */
static bool _async_idle_remove_locked(struct chttpclient *cli,
                                      chttp_async_ctx_t *target) {
  if (!cli->idle_pools_async || !target->origin_key) return false;
  cmap_pair kp = {.ptr = target->origin_key,
                  .size = strlen(target->origin_key) + 1};
  cmap_pair vp;
  if (chmap_get_elem_ref(cli->idle_pools_async, &kp, &vp) != ccol_success)
    return false;
  cvec list = _read_cvec(vp.ptr);
  if (!list) return false;
  size_t n = cvector_elem_count(list);
  for (size_t i = 0; i < n; i++) {
    chttp_async_ctx_t **slot = (chttp_async_ctx_t **)cvector_at(list, i);
    if (*slot != target) continue;
    chttp_async_ctx_t *last = NULL;
    cvector_pop_back(list, &last); /* Popping the last element can shrink
                                    * the backing array, so every pointer
                                    * into it taken before this call is
                                    * stale. */
    if (i < cvector_elem_count(list)) {
      chttp_async_ctx_t **slot2 = (chttp_async_ctx_t **)cvector_at(list, i);
      *slot2 = last;
    }
    /* The list of this origin is empty, so drop it and its map entry (the
     * sibling _idle_pool_take of Tier 1 has the same comment). Without it
     * a long-running client that talks to many origins grows the number
     * of entries without bound, and it's always safe because
     * _async_idle_pool_offer creates a fresh entry for an origin that has
     * none. */
    if (cvector_elem_count(list) == 0) {
      __cvector_destroy(list);
      chmap_delete_elem(cli->idle_pools_async, &kp);
    }
    return true;
  }
  return false;
}

/*
 * Tries to pop a usable idle connection for `origin_key`. On success it
 * fills *out and returns true. When there's no fresh candidate it returns
 * false and the caller falls back to a fresh connect.
 *
 * IMPORTANT: on a true return ctx->idle_lock is LOCKED. The caller,
 * _async_submit_hop, keeps it held while it reconfigures ctx for the new
 * hop and tries the write, and unlocks only once ctx is fully consistent
 * again, every field set and the write tried.
 *
 * That's not a question of style. Popping a candidate out of the pool's
 * cvec hides it from a concurrent _async_idle_pool_take, but it does
 * nothing against the reactor. The connection stayed fully attached to
 * the reactor the whole time it sat in the pool, so a readable, writable
 * or error callback for it can be dispatched on another thread at any
 * moment, the same race as everywhere else in this section.
 *
 * The caller holds idle_lock across the WHOLE reconfiguration and not only
 * around the state and chain pair. With a narrower hold a concurrent
 * on_close can see ctx half reconfigured (state already off
 * CHTTP_ASYNC_IDLE while this thread is writing hop, pctx, wire and the
 * rest) and tear it down, freeing ctx and destroying idle_lock itself
 * while our caller is using it. That use-after-free goes deeper than a
 * torn write of state and chain.
 *
 * _async_dispatch_kind, which on_readable, on_writable and on_error all go
 * through, takes the same lock, so any of them racing this function just
 * blocks until the caller unlocks, and by then ctx is self-consistent one
 * way or the other.
 *
 * Classifying and popping for one origin happens in a single critical
 * section over `cli->lock`. We scan the origin's cvec from back to front,
 * from the highest index (the warmest candidate, offered most recently)
 * down to the oldest one at index 0.
 *
 * The LIFO preference is on purpose. Scanning front to back would pick the
 * OLDEST fresh candidate, and an older idle connection is statistically
 * more likely to have been closed by the peer already.
 *
 * A candidate older than CHTTP_IDLE_MAX_AGE_MS gets a shutdown right here.
 * Reading its fd is safe because the whole walk holds cli->lock, and every
 * removal from this vector (the only thing that can invalidate a
 * candidate) is gated by that same lock.
 *
 * The stale candidate stays exactly where it is in the vector. The walk
 * writes nothing to state, chain or hop_completed and removes nothing,
 * because this function does NOT tear a stale candidate down itself.
 *
 * shutdown() makes a genuine EPOLLIN or EOF happen on the candidate's fd,
 * which is registered for read, so the idle branch of
 * _async_on_readable_impl gets to _async_idle_ctx_finish and that path
 * reaps the candidate for real from a dispatch context. It makes its own
 * exclusive claim with _async_idle_remove_locked and frees the pooled
 * engine reference too.
 *
 * The first genuinely fresh candidate the walk finds is popped and
 * returned, swapped with the last element like in
 * _async_idle_remove_locked.
 *
 * Popping a stale candidate and tearing it down with _async_ctx_finish
 * right from this application thread would be a use-after-free, and
 * AddressSanitizer reports it. A reactor dispatch callback for that exact
 * registered ctx can legitimately be in flight on another thread at the
 * same moment, or about to run, and the OS can preempt that thread for an
 * unbounded time between the liveness check of ccol_event_loop and its
 * first touch of ctx. During that gap this application thread could free
 * the ctx out from under the callback.
 *
 * A per-ctx atomic refcount pinned by every dispatch callback doesn't
 * close this either, since the pin's very first read of the refcount can
 * race a concurrent free in the same way.
 *
 * What does close it by construction, and what we do, is that no
 * application thread ever tears down a registered ctx. The walk only marks
 * a stale candidate, with shutdown() and not with any field of the ctx,
 * and then the SAME dispatch-context reaping path that a naturally dying
 * idle connection goes through takes it. That path is safe by nature
 * because ccol_event_loop serialises it against every other dispatch for
 * the same registration, through its entry->dispatch_lock and the
 * entry->refcount invariant of one job in flight per entry.
 *
 * The price is that a stale candidate which got its shutdown but wasn't
 * reaped yet keeps counting against CHTTP_MAX_IDLE_PER_ORIGIN and
 * CHTTP_MAX_IDLE_TOTAL in the capacity checks of _async_idle_pool_offer
 * until the dispatch-driven reap runs, which is usually one turnaround of
 * the reactor. The pool's effective capacity drops a little for a short
 * time, which is harmless, while tearing it down here would bring the bug
 * above back.
 */
static bool _async_idle_pool_take(struct chttpclient *cli,
                                  const char *origin_key,
                                  chttp_async_chain_t *chain,
                                  chttp_async_ctx_t **out) {
  chttp_async_ctx_t *ctx = NULL;
  _ccol_mutex_lock(cli->lock);
  if (cli->idle_pools_async) {
    cmap_pair kp = {.ptr = (void *)origin_key, .size = strlen(origin_key) + 1};
    cmap_pair vp;
    if (chmap_get_elem_ref(cli->idle_pools_async, &kp, &vp) == ccol_success) {
      cvec list = _read_cvec(vp.ptr);
      if (list) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        size_t n = cvector_elem_count(list);
        for (size_t idx = n; idx-- > 0;) {
          chttp_async_ctx_t **slot =
              (chttp_async_ctx_t **)cvector_at(list, idx);
          chttp_async_ctx_t *cand = *slot;
          int64_t age_ms = _ms_elapsed_floor(&cand->last_used, &now);
          /* chttpclient_set_tls can replace the configuration a TLS
           * candidate did its handshake with. We refuse such a candidate
           * the same way as an over-age one, with a shutdown() only, and
           * leave the real teardown to the dispatch-context reaping path
           * described above.
           *
           * The pool is keyed on the origin alone, so this check is all
           * that keeps a request issued after a reconfiguration from
           * going out over a connection that used the old policy. That
           * reconfiguration may pin a private CA instead of the system
           * bundle, turn verification back on or rotate a client
           * certificate.
           *
           * Tier 2 skips CONNECTING and TLS_HANDSHAKING for a reused
           * connection, so nothing further down would apply the new
           * policy. */
          if (age_ms > CHTTP_IDLE_MAX_AGE_MS ||
              (cand->is_https && cand->tls_generation != cli->tls_generation)) {
            int fd = cand->fd;
            if (fd >= 0) shutdown(fd, SHUT_RDWR);
            continue;
          }
          chttp_async_ctx_t *last = NULL;
          cvector_pop_back(list, &last); /* May shrink the backing array,
                                          * which makes every pointer into
                                          * it from before this call
                                          * stale. */
          if (idx < cvector_elem_count(list)) {
            chttp_async_ctx_t **slot2 =
                (chttp_async_ctx_t **)cvector_at(list, idx);
            *slot2 = last;
          }
          _async_idle_count_dec_locked(cli);
          ctx = cand;
          /* The origin's list is empty, so drop it and its map entry (the
           * same comment on _idle_pool_take, its Tier 1 sibling, says why
           * that's needed and always safe).
           *
           * A stale candidate that the `continue` above skipped stays in
           * the list on purpose, for a dispatch to reap later (see the
           * comment above this function). So this can only fire on the
           * way out for a genuine reuse, never in the middle of the
           * scan. */
          if (cvector_elem_count(list) == 0) {
            __cvector_destroy(list);
            chmap_delete_elem(cli->idle_pools_async, &kp);
          }
          break;
        }
      }
    }
  }
  _ccol_mutex_unlock(cli->lock);
  if (!ctx) return false;

#ifdef RUNNING_UNIT_TESTS
  int stray_armed = 1;
  bool stray_before_take = atomic_compare_exchange_strong(
      &g_stray_before_take_stage_for_tests, &stray_armed, 2);
  if (stray_before_take) {
    (void)ccol_event_loop_modify(cli_engine_bundler.reactor, ctx->reg,
                                 ccol_select_write);
    _stray_before_take_wait_for_tests(3);
  }
#endif
  _ccol_mutex_lock(ctx->idle_lock); /* Stays held on return, see the
                                     * contract above. */
#ifdef RUNNING_UNIT_TESTS
  ctx->idle_lock_held_for_tests = true;
#endif
  /* The deadline sweep never takes idle_lock on purpose, so it's the
   * dedicated leaf lock ctx->deadline_lock (see its field comment) that
   * lets the sweep read this field concurrently. */
  _ccol_mutex_lock(ctx->deadline_lock);
  ctx->overall_deadline = chain->overall_deadline;
  /* A reused connection starts its hop without any verdict of the sweep.
   * The offer cleared the flag under this lock and nothing sets it while
   * the ctx is idle, but clearing it here keeps the start of every hop
   * the same, whatever the ctx went through while idle. */
  ctx->timed_out = false;
  ctx->chain = chain;
  ctx->state = CHTTP_ASYNC_WRITING;
  _ccol_mutex_unlock(ctx->deadline_lock);
  /* An idle ctx doesn't leave the deadline registry. */
  _client_deadline_note(&chain->overall_deadline);
#ifdef RUNNING_UNIT_TESTS
  if (stray_before_take) {
    /* Give the held dispatch time to run while we're building the hop
     * under idle_lock. */
    atomic_store(&g_stray_before_take_stage_for_tests, 4);
    struct timespec ts = {0, 200000000L};
    nanosleep(&ts, NULL);
  }
#endif
  *out = ctx;
  return true;
}

/*
 * Offers a connection that is good and eligible for keep-alive back to
 * cli's async idle pool, under the same per-origin and total caps as in
 * Tier 1.
 *
 * The return has exactly two meanings. `false` says we didn't take the
 * connection and left ctx COMPLETELY UNTOUCHED, attached to its original
 * chain and in whatever state it came in, and the caller falls back to an
 * ordinary close. As the matching comment in Tier 1 puts it, that's a
 * lost optimisation and never a correctness problem.
 *
 * `true` says we handled it and the caller must not touch ctx again. Two
 * different outcomes hide behind it: the normal case, where ctx really
 * joins the idle pool, and the cvector_push_back failure below, which
 * only happens when memory runs out (so very rarely) and where we have
 * already torn ctx down ourselves.
 *
 * Returning `false` for that second outcome would be a double free, since
 * the caller's fallback for `false` calls _async_ctx_finish and that tears
 * down a ctx whose reference we freed already. It's the same kind of
 * lifetime hazard that the field comment of pending_app_teardown
 * describes, and it's closed the same way, by construction rather than by
 * a refcount.
 *
 * On success ctx is detached from its original chain and the one chain
 * reference this hop held for it is freed. The hop is over and the idle
 * pool, not the chain, owns the connection. Without that free the
 * reference simply leaks, because until it's reused or evicted a pooled
 * connection never goes through the ordinary _async_ctx_teardown path,
 * the only OTHER place that frees a ctx's chain reference.
 *
 * The capacity check comes FIRST and runs entirely under cli->lock. It
 * asks whether the pool has room for this origin, creates the origin's
 * list on demand, and knows for certain whether the offer will succeed
 * before it touches ctx AT ALL.
 *
 * Only once that's certain do we reset ctx to a clean idle state (clear
 * the hop's parse, body and wire buffers, mark it CHTTP_ASYNC_IDLE) and
 * insert it. Both steps happen in the SAME critical section over
 * cli->lock, so no other thread can pop from the list in between.
 *
 * The order matters for two separate reasons. One is that ctx must never
 * be visible and poppable through _async_idle_pool_take with the reset
 * half done. _async_idle_pool_take overwrites whatever it finds and waits
 * for nothing, so a concurrent take() could start reconfiguring ctx for a
 * brand-new hop while we are freeing and resetting those very fields for
 * the OLD hop, a real data race on the fields and not only a logical
 * inconsistency.
 *
 * The other is that resetting ctx too early, before we know there's room,
 * would break the contract above that leaves ctx untouched on failure.
 */
static bool _async_idle_pool_offer(chttp_async_ctx_t *ctx) {
  struct chttpclient *cli = ctx->cli;

  /* The ctx is about to belong to the idle pool and to no chain, so it
   * needs an engine reference of its own, independent of the chain whose
   * life just ended, to keep the reactor alive while it sits here. We take
   * it before anything else so a failure has no pool state to unwind. */
  if (_client_engine_acquire() != ccol_success) return false;

  cvec list = NULL;
  bool has_room = false;
  _ccol_mutex_lock(cli->lock);
  if (!cli->destroying && cli->idle_pools_async &&
      cli->idle_total_count_async < CHTTP_MAX_IDLE_TOTAL) {
    cmap_pair kp = {.ptr = ctx->origin_key,
                    .size = strlen(ctx->origin_key) + 1};
    cmap_pair vp;
    if (chmap_get_elem_ref(cli->idle_pools_async, &kp, &vp) == ccol_success) {
      list = _read_cvec(vp.ptr);
    }
    /* A genuinely new origin only gets a fresh entry while the cap on
     * distinct origins has room (see the comment of CHTTP_MAX_IDLE_ORIGINS),
     * exactly like its Tier 1 sibling _idle_pool_offer. At the cap list
     * stays NULL and we fall through to the same "doesn't fit, don't pool
     * it" path that the has_room check below gives anyway. */
    if (!list && chmap_elem_count(cli->idle_pools_async) <
                     _effective_max_idle_origins()) {
      char *cverr = NULL;
      list = cvector_create_full(sizeof(chttp_async_ctx_t *), cli->m_procs,
                                 &cverr);
      if (list) {
        cmap_pair vp2 = {.ptr = &list, .size = sizeof(list)};
        if (chmap_insert_elem(cli->idle_pools_async, &kp, &vp2) !=
            ccol_success) {
          __cvector_destroy(list);
          list = NULL;
        }
      }
    }
    has_room = (list && cvector_elem_count(list) < CHTTP_MAX_IDLE_PER_ORIGIN);
  }
  if (!has_room) {
    _ccol_mutex_unlock(cli->lock);
    _client_engine_release();
    return false; /* ctx is untouched, the caller does an ordinary
                   * close. */
  }

  /* Decided: this ctx WILL leave its chain and join the idle pool. Keep the
   * chain reference aside for the free below, then reset ctx while we
   * hold cli->lock (the comment above this function says why). */
  chttp_async_chain_t *old_chain = ctx->chain;

  /* _parse_ctx_free_fields frees location and headers and sets both to
   * NULL, but every other field keeps whatever the finished hop left there:
   * the status code, the sink wiring and the per-message flags.
   *
   * At its other call sites that's fine. _async_ctx_free calls it right
   * before freeing the whole ctx, and Tier 1 calls it on a per-hop
   * chttp_parse_ctx_t that is a fresh stack struct for every hop, and both
   * throw the struct away right after.
   *
   * Here we do NOT throw it away. ctx is about to be reused for a later
   * hop, and a stale flag or status code of the finished hop would be read
   * as describing the next one.
   *
   * So the memset below, which runs once _parse_ctx_free_fields has freed
   * everything that needs freeing, puts back the all-zero state that a ctx
   * from _ccol_mem_calloc starts with, and that every other user of
   * chttp_parse_ctx_t depends on. */
  _parse_ctx_free_fields(&ctx->pctx);
  memset(&ctx->pctx, 0, sizeof(ctx->pctx));
  /* A long header line in the finished hop may have left a heap line
   * buffer in the parser, and an idle connection shouldn't hold one. */
  chttp1_parser_release(&ctx->parser);
  _ccol_mem_free(ctx->mp, ctx->bb.buf);
  ctx->bb.buf = NULL;
  ctx->bb.len = ctx->bb.cap = 0;
  ctx->bb.oom = false;
  ctx->bb.too_large = false;
  _ccol_mem_free(ctx->mp, ctx->wire);
  ctx->wire = NULL;
  ctx->wire_len = ctx->wire_sent = 0;
  /* The "Expect: 100-continue" state goes stale the moment this hop is
   * done. Every fresh hop attempt computes want_100_continue and header_len
   * again in _async_submit_hop and _async_retry_hop, and both also arm
   * continue_decided to false.
   *
   * We reset it here anyway, to follow the rule of this function above and
   * give back the all-zero state of a ctx from _ccol_mem_calloc.
   *
   * continue_carry should always be NULL by the time a hop gets here, since
   * _async_upload_pause consumes it in the same dispatch that stores it
   * (see the comment of that field). It's freed anyway as a defence, like
   * wire and bb just above. */
  _ccol_mem_free(ctx->mp, ctx->continue_carry);
  ctx->continue_carry = NULL;
  ctx->continue_carry_len = 0;
  ctx->want_100_continue = false;
  ctx->header_len = 0;
  ctx->retry_unsafe = false;
  ctx->upload_paused = false;
  ctx->resp_msg_started = false;
  ctx->tls_read_blocked = false;
  ctx->idle_tainted = false;
  ctx->upload_check_at = 0;
  ctx->answered_early = false;
  ctx->duplex = false;
  atomic_store(&ctx->flip_watch, CHTTP_FLIP_NONE);
  atomic_store(&ctx->continue_decided, false);
  atomic_store(&ctx->continue_msg_started, false);
  /* Goes stale as soon as the ctx is idle. It was the request path of the
   * hop that just finished and means nothing while the pool holds the ctx,
   * so we free it here (like wire and bb above) instead of leaving it
   * dangling until the next _async_submit_hop overwrites it. */
  _ccol_mem_free(ctx->mp, ctx->path_and_query);
  ctx->path_and_query = NULL;
  ctx->hop_completed = false;
  ctx->reused = false;
  ctx->any_bytes_read = false;
  ctx->interim_responses_seen = 0;
  /* ctx->chain and ctx->state flip together under idle_lock (see the
   * comment of that field). Without the lock a concurrent readable,
   * writable or error dispatch on another thread can see the two writes
   * torn, state IDLE with chain not NULL yet or the other way round.
   *
   * ctx->timed_out is reset in the same critical section, as late as
   * possible. The deadline sweep only sets that flag for a ctx in a
   * connect state, which this one isn't since it just finished a hop
   * cleanly, or for has_chain, meaning node->chain != NULL.
   *
   * Resetting it right where chain is cleared and state goes IDLE closes a
   * window (see the comment of _client_deadline_sweep_once about
   * has_chain) where the sweep could mark this ctx timed out from the
   * stale overall_deadline of the hop that just finished. That would
   * poison some FUTURE, unrelated hop that reuses the ctx from the idle
   * pool.
   *
   * A stale ctx->timed_out left by a hop that succeeded would starve the
   * dead-connection retry of that future hop (the
   * `reused && !any_bytes_read` branch of _async_ctx_finish), and we would
   * report ccol_timed_out to a caller whose own deadline was never in
   * danger. */
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_offer_entered_for_tests, 1);
#endif
  _ccol_mutex_lock(ctx->idle_lock);
  /* Under deadline_lock this is a single step as far as the deadline
   * sweep's verdict goes, because the sweep takes its snapshot and writes
   * timed_out under that lock (see _client_deadline_sweep_once). It's a
   * leaf lock, so nesting it inside idle_lock can't create an ordering
   * cycle. */
  _ccol_mutex_lock(ctx->deadline_lock);
  ctx->chain = NULL;
  ctx->state = CHTTP_ASYNC_IDLE;
  ctx->timed_out = false;
  /* A hop that the sweep answered but which then finished cleanly goes
   * idle. An idle connection isn't in flight, and a later hop on it may be
   * answered again. */
  if (atomic_exchange(&ctx->abandoned, false))
    atomic_fetch_sub(&g_async_hops_abandoned, 1);
  _ccol_mutex_unlock(ctx->deadline_lock);
  _ccol_mutex_unlock(ctx->idle_lock);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_offer_done_for_tests, 1);
#endif

  clock_gettime(CLOCK_MONOTONIC, &ctx->last_used);
  bool pushed;
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_force_offer_push_fail_for_tests, false)) {
    /* Pretend cvector_push_back failed. We do NOT make the real call, so
     * list is left exactly as a genuine allocation failure would leave it,
     * with nothing inserted. The comment of
     * g_force_offer_push_fail_for_tests explains why the real call can
     * never fail here. */
    pushed = false;
  } else
#endif
  {
    pushed = (cvector_push_back(list, &ctx) == ccol_success);
  }
  if (pushed) cli->idle_total_count_async++;
  _ccol_mutex_unlock(cli->lock);

  if (!pushed) {
    /* The rare case where cvector_push_back itself runs out of memory,
     * even though the check above saw room. ctx is already detached from
     * old_chain and reset to an idle shape, so unlike the has_room ==
     * false path we can't hand it back to the caller as an active ctx to
     * close, and we tear it down right here.
     *
     * ctx is freed BEFORE old_chain, never after. Dropping the chain's
     * last reference is what lets a chttpclient_destroy go on, which
     * blocks until async_in_flight_count reaches zero and then frees the
     * client and, for a client from chttpclient_create_mp, the caller's
     * own ccol_memmgmt_procs_t too. That's the very allocator
     * _async_ctx_free uses to free ctx. */
    _async_ctx_free(ctx);
    _client_engine_release();
    _async_chain_release(old_chain);
    return true; /* true and NOT false: we tore ctx down ourselves here,
                  * unlike the has_room == false path above, which
                  * returns false with ctx completely untouched. The
                  * caller, _async_finish_connection, must not ALSO call
                  * _async_ctx_finish on this ctx.
                  *
                  * In the contract of _async_finish_connection false
                  * means "fall back to an ordinary teardown of ctx". For
                  * this failure that would call _async_ctx_finish a
                  * second time on a ctx whose reference we already freed,
                  * a genuine double free, and the same kind of lifetime
                  * hazard that the field comment of pending_app_teardown
                  * describes. */
  }

#ifdef RUNNING_UNIT_TESTS
  /* See g_force_stray_idle_write_arm_for_tests. Arm the write direction on
   * a connection that just joined the idle pool, the way a flip by the
   * deadline sweep on a stale sampled state would. Each arming consumes
   * the flag once. */
  if (atomic_exchange(&g_force_stray_idle_write_arm_for_tests, false)) {
    _async_reg_modify(ctx, ccol_select_write);
  }
#endif
  _async_chain_release(old_chain);
  return true;
}

/*
 * Called once a hop's response is fully consumed and before we pick
 * between a redirect and a fulfil. With `reusable` true the connection
 * goes back to cli's idle pool, otherwise it's closed.
 *
 * It's the same pair of reusable and _idle_pool_offer as in
 * chttp_do_internal of Tier 1 and it applies whether this hop turns out to
 * be a redirect or the final response. Both _async_handle_redirect and the
 * CHTTP1_PAUSED handling of _async_process_reading_data call it before
 * they do anything else with the connection.
 */
/*
 * Where an ACTIVE ctx (one the idle pool doesn't hold) gets torn down, and
 * it runs exactly once. Every place that ends such a connection calls it
 * exactly once: a hard error, a timeout, an explicit close after a
 * successful completion, or a redirect we didn't pool. We need an explicit
 * point like this because ccol_event_loop has no unconditional terminal
 * callback we could rely on instead (see the comment at the top of the
 * "ASYNC CONNECTION STATE MACHINE" section).
 *
 * We do NOT fulfil the future with a generic error before checking
 * whether a retry is possible. Such a fulfil would set ctx->hop_completed
 * and then wrongly skip a legitimate retry of a reused connection.
 *
 * When neither timed_out nor the reused-retry case applies, nothing is
 * fulfilled here. The chain release in _async_ctx_teardown below has its
 * own backstop guarded by chain->fulfilled, which fulfils the future with
 * the generic ccol_http_transfer_aborted exactly once, and every caller
 * that doesn't need a more specific code relies on that.
 */
/* Forward declaration. The function below calls _async_retry_hop, which is
 * defined much further down, after the "ASYNC IDLE POOL" section, next to
 * the TLS and plain write helpers that need it too. */
static void _async_retry_hop(chttp_async_ctx_t *ctx);

static void _async_ctx_finish(chttp_async_ctx_t *ctx) {
  if (!ctx->hop_completed) {
    if (ctx->timed_out) { /* The field is _Atomic, so a plain read
                           * is race-free. */
      _async_fulfill(ctx, ccol_timed_out, NULL);
    } else if (ctx->reused && !ctx->any_bytes_read && !ctx->retry_unsafe &&
               (ctx->replayable || ctx->wire_sent < ctx->wire_len)) {
      /* A request that didn't go out in full never reached the server as a
       * whole, so we send it again. One that did only goes again when it's
       * replayable (see _request_is_replayable). */
      _async_retry_hop(ctx); /* Sets ctx->hop_completed to true by
                              * itself. */
    }
  }
  _async_ctx_teardown(ctx);
}

/*
 * The teardown point for a ctx that sits in the idle pool: remove it from
 * the pool and free it. Such a ctx holds its own engine reference (taken
 * by _async_idle_pool_offer) and no chain reference, since ctx->chain is
 * NULL while the ctx is idle.
 *
 * We only really tear ctx down when THIS call is the one that removes it
 * from the pool's cvec, and cli->lock alone decides who owns it (see the
 * bool return of _async_idle_remove_locked). If the removal finds nothing
 * because a concurrent caller got there first, we return at once and
 * don't touch ctx again.
 *
 * That check carries real weight. N reactor threads share one epoll
 * instance without any deduplication like EPOLLEXCLUSIVE, and a genuinely
 * dead connection's EOF is PERSISTENT, not a one-shot event like readable
 * data. Every later recv() or peek on an fd that's closed keeps reporting
 * EOF and never EWOULDBLOCK.
 *
 * So two stale dispatches in a row for the same idle ctx can both
 * legitimately see a real EOF through the peek in the idle branch of
 * _async_on_readable and both decide to evict, which is why
 * _async_ctx_free and _client_engine_release run ONLY when the removal
 * above found something.
 *
 * Calling them unconditionally is a double free in exactly that
 * two-dispatch case, and ordinary testing doesn't show it because you need
 * a pooled connection that the server really closed for good, not just a
 * stale but harmless spurious dispatch.
 *
 * idle_total_count_async is decremented in a THIRD separate locked
 * section, which may also broadcast idle_async_drained and only runs once
 * _async_ctx_free and _client_engine_release below have completely run,
 * rather than as part of the removal step above.
 *
 * __chttpclient_destroy takes a count of zero as the signal that every
 * pooled connection has really finished its teardown, and goes on to free
 * cli and cli->m_procs with it, while ctx->mp points at that same
 * cli->m_procs.
 *
 * Decrementing at removal time would fire that signal too early, while
 * our _async_ctx_free on this thread is using cli->m_procs, a
 * use-after-free that ThreadSanitizer reports and valgrind alone doesn't.
 * The comment of _async_idle_remove_locked has the whole story.
 */
static void _async_idle_ctx_finish(chttp_async_ctx_t *ctx) {
  struct chttpclient *cli = ctx->cli;
  _ccol_mutex_lock(cli->lock);
  bool removed = _async_idle_remove_locked(cli, ctx);
  _ccol_mutex_unlock(cli->lock);
  if (!removed) return;
  _async_ctx_free(ctx);
  _client_engine_release();
  _ccol_mutex_lock(cli->lock);
  _async_idle_count_dec_locked(cli);
  _ccol_mutex_unlock(cli->lock);
}

/*
 * Ends a CONNECTING ctx whose non-blocking connect completed with an
 * error, seen as a failed getsockopt(SO_ERROR) on a writable dispatch or
 * as an error dispatch. There's only one connect for such a ctx, since a
 * target with several addresses goes through the race of the "ASYNC HAPPY
 * EYEBALLS" section, and that only hands the ctx a connected socket.
 *
 * ctx->timed_out is checked first when we report the failure, because the
 * shutdown(fd, SHUT_RDWR) the deadline sweep does on a stuck connect shows
 * up here as a failed connect too. Without the check an expired
 * connect_timeout_us would come out as ccol_http_connection_failed
 * instead of the ccol_timed_out that chttpclient.h documents.
 */
static void _async_connect_failed(chttp_async_ctx_t *ctx) {
  _async_fulfill(
      ctx, ctx->timed_out ? ccol_timed_out : ccol_http_connection_failed, NULL);
  _async_ctx_finish(ctx);
}

static void _async_finish_connection(chttp_async_ctx_t *ctx, bool reusable) {
  if (reusable && !ctx->request_close && ctx->origin_key &&
      _async_idle_pool_offer(ctx))
    return;
  _async_ctx_finish(ctx);
}

/* Forward declaration. _async_on_readable below calls it for a reused
 * connection that turns out to be dead before we read any response byte.
 * The definition is further down next to _async_submit_hop, after
 * _async_connect_task, which it needs for queueing the replacement
 * attempt. */
static void _async_retry_hop(chttp_async_ctx_t *ctx);

/* Forward declaration, _async_tls_advance below calls it once the handshake
 * completes. Defined later in this file. */
static void _async_tls_try_write(chttp_async_ctx_t *ctx);

/* Forward declaration. _async_replay_continue_carry below feeds the
 * "Expect: 100-continue" trailing bytes through it (see the field comment
 * of ctx->continue_carry). It's defined later in this file and the
 * ordinary on_readable dispatch path uses the same function. */
static void _async_process_reading_data(chttp_async_ctx_t *ctx,
                                        const char *data, size_t data_len);

/*
 * Drives the client TLS handshake one step at a time from
 * _async_on_readable or _async_on_writable, whichever fires next. The
 * ctx's single registration is flipped between the read and write
 * direction with ccol_event_loop_modify, following what the handshake asks
 * for with WANT_READ and WANT_WRITE.
 *
 * ctx->tls needs no lock around it. The per-registration dispatch_lock of
 * ccol_event_loop already keeps the read and write directions of this ctx
 * from ever running concurrently (the comment at the top of this section
 * explains why that leaves this module no need for a tls_lock at all).
 */
static void _async_tls_advance(chttp_async_ctx_t *ctx) {
  ctls_handshake_result_t r = ctls_conn_handshake_step(ctx->tls);
  if (r == CTLS_HANDSHAKE_DONE) {
    ctx->state = CHTTP_ASYNC_WRITING;
    if (_async_reg_modify(ctx, ccol_select_write) != ccol_success) {
      _async_ctx_finish(ctx);
      return;
    }
    _async_tls_try_write(ctx);
    return;
  }
  if (r == CTLS_HANDSHAKE_ERROR) {
    /* In OpenSSL 0 is X509_V_OK. ctls.h doesn't expose any OpenSSL header
     * to its callers on purpose, so like _tls_handshake of Tier 1 we
     * compare the raw value and not the X509_V_OK symbol.
     *
     * A handshake failure never gets a retry, whatever ctx->reused says.
     * It's a problem at the TLS level, not a sign that the pooled
     * connection simply went stale.
     *
     * ctx->timed_out is checked FIRST. The field is _Atomic, so a plain
     * read is race-free, same as the read of it in _async_ctx_finish.
     *
     * When the deadline sweep does shutdown(fd, SHUT_RDWR) on a stuck
     * handshake we see it here as CTLS_HANDSHAKE_ERROR. Without this check
     * an expired connect_timeout_us or request_timeout_us would be reported
     * as a TLS handshake or certificate failure instead of ccol_timed_out,
     * against the documented contract of chttpclient.h. */
    long vr = ctls_conn_verify_result(ctx->tls);
    _async_fulfill(ctx,
                   ctx->timed_out
                       ? ccol_timed_out
                       : (vr != 0 ? ccol_http_tls_cert_verification_failed
                                  : ccol_http_tls_handshake_failed),
                   NULL);
    _async_ctx_finish(ctx);
    return;
  }
  ccol_select_dir want =
      (r == CTLS_HANDSHAKE_WANT_WRITE) ? ccol_select_write : ccol_select_read;
  if (_async_reg_modify(ctx, want) != ccol_success) {
    _async_ctx_finish(ctx);
  }
}

/*
 * A response that starts arriving while Tier 2 is sending the request.
 * chttp_early_resp_t describes the case and the rules, which are the same
 * in every tier. A ctx has one registration that watches one direction at
 * a time, so Tier 2 applies those rules at the points where the send
 * can't go on:
 *   - A write that would block while the socket holds response bytes PAUSES
 *     the upload. The ctx goes to CHTTP_ASYNC_READING with
 *     ctx->upload_paused set and the read path parses what came in.
 *   - A write that fails because the peer is gone STOPS the upload. The ctx
 *     goes to CHTTP_ASYNC_READING without the flag and the read path takes
 *     whatever answer the peer left, or finds none.
 *   - A paused upload resumes (_async_upload_resume) on a discarded interim
 *     response, on a read that finds nothing more before any message
 *     started, and on a final response of 200 to 299 once its header block
 *     is in. A final response of 300 or above ends the pause for good and
 *     the rest of the request is never sent.
 * A ctx whose request didn't go out in full is never offered to the idle
 * pool (see _async_request_sent).
 *
 * A blocked write needs the write direction while the response may come on
 * the other one, so the deadline sweep looks after that direction on every
 * tick of CHTTP_DEADLINE_SWEEP_INTERVAL_MS (ctx->flip_watch). When input
 * arrives for a send waiting for room (CHTTP_FLIP_ON_INPUT) it turns the
 * registration to the read direction and the send pauses. The one case
 * where both directions carry the exchange is a 200 to 299 response that
 * streams in while the request goes out (ctx->duplex), e.g. a server that
 * echoes the body. There a blocked write waits on the read direction
 * first, because such a server answers what it read, and for a server
 * that keeps reading without writing the sweep turns the registration
 * back to the write direction once there's room (CHTTP_FLIP_ON_ROOM).
 */

static void _async_plain_try_write(chttp_async_ctx_t *ctx);

/* True once every byte of the request went out before a response started.
 * We never reuse a connection where it's false. */
static inline bool _async_request_sent(const chttp_async_ctx_t *ctx) {
  return ctx->wire_sent >= ctx->wire_len && !ctx->answered_early;
}

/* True when the socket, or the TLS layer on top of it, holds input that a
 * read would take without waiting. Only a write that would block calls it, so a
 * write path that makes progress doesn't pay for it. */
static bool _async_input_pending(chttp_async_ctx_t *ctx) {
  /* ctls_conn_has_pending_input also says yes for the start of a record
   * that isn't complete. Once a read blocked on such a start only the
   * socket can bring the rest, so then we ask the socket alone (see the
   * field comment of ctx->tls_read_blocked). */
  if (ctx->tls && !ctx->tls_read_blocked &&
      ctls_conn_has_pending_input(ctx->tls))
    return true;
  struct pollfd p = {.fd = ctx->fd, .events = POLLIN, .revents = 0};
  int rc;
  do {
    rc = poll(&p, 1, 0);
  } while (rc < 0 && errno == EINTR);
  return rc > 0 && (p.revents & POLLIN) != 0;
}

/* Stops the room watch of ctx, if it has one; see ctx->room_reg. */
static void _async_room_watch_stop(chttp_async_ctx_t *ctx) {
  ccol_event_reg room = atomic_exchange(&ctx->room_reg, CCOL_EVENT_REG_INVALID);
  if (room) ccol_event_loop_remove(cli_engine_bundler.reactor, room);
}

/* Changes the direction of ctx->reg. While a room watch exists it holds the
 * fd's write direction, and the event loop keeps one registration per
 * direction of an fd. Turning reg to the write direction therefore ends
 * the room watch first, and the write dispatch that follows resumes a
 * paused send instead of it. */
#ifdef RUNNING_UNIT_TESTS
/* Arming g_stall_next_read_for_tests makes the next hop that turns to
 * reading its response get no dispatch at all. Its registration is paused
 * right after it turns to the read direction, as if the reactor's dispatch
 * pool could never allocate a task node for it. The test resumes it with
 * _chttp_stalled_read_resume_for_tests after it has seen what it waits
 * for. */
static _Atomic bool g_stall_next_read_for_tests = false;
static _Atomic ccol_event_reg g_stalled_reg_for_tests = CCOL_EVENT_REG_INVALID;

void _chttp_stall_next_read_for_tests(void) {
  atomic_store(&g_stalled_reg_for_tests, CCOL_EVENT_REG_INVALID);
  atomic_store(&g_stall_next_read_for_tests, true);
}

/* Returns true when there was a paused registration and it got resumed. */
bool _chttp_stalled_read_resume_for_tests(void) {
  ccol_event_reg reg =
      atomic_exchange(&g_stalled_reg_for_tests, CCOL_EVENT_REG_INVALID);
  if (!reg) return false;
  return ccol_event_loop_resume(cli_engine_bundler.reactor, reg) ==
         ccol_success;
}
#endif

static ccol_retval_t _async_reg_modify(chttp_async_ctx_t *ctx,
                                       ccol_select_dir dir) {
  if (dir == ccol_select_write) _async_room_watch_stop(ctx);
#ifdef RUNNING_UNIT_TESTS
  if (dir == ccol_select_read && ctx->state == CHTTP_ASYNC_READING &&
      atomic_exchange(&g_stall_next_read_for_tests, false)) {
    ccol_retval_t mrv =
        ccol_event_loop_modify(cli_engine_bundler.reactor, ctx->reg, dir);
    if (mrv == ccol_success && ccol_event_loop_pause(cli_engine_bundler.reactor,
                                                     ctx->reg) == ccol_success)
      atomic_store(&g_stalled_reg_for_tests, ctx->reg);
    return mrv;
  }
#endif
  return ccol_event_loop_modify(cli_engine_bundler.reactor, ctx->reg, dir);
}

static void _async_on_room(ccol_event_loop loop, ccol_event_reg reg,
                           ccol_selectable *sel, void *arg);

/* Starts the room watch for a duplex send that a full send buffer paused
 * (see ctx->room_reg), and returns false if the registration can't be
 * added. The caller either holds the fd's dispatch lock or is the only
 * owner of the ctx. */
static bool _async_room_watch_start(chttp_async_ctx_t *ctx) {
  if (atomic_load(&ctx->room_reg)) return true;
  char *err = NULL;
  ccol_event_handlers_t handlers = {.on_writable = _async_on_room};
  ccol_event_reg room = ccol_event_loop_add(
      cli_engine_bundler.reactor,
      ccol_selectable_from_fd(ctx->fd, ccol_select_write), handlers, ctx, &err);
  if (!room) return false;
  atomic_store(&ctx->room_reg, room);
  return true;
}

/* The room watch fired, so the paused duplex send can continue. The fd's
 * dispatch lock serialises this with every dispatch of ctx->reg, and the
 * ctx teardown removes this registration before it frees ctx. An error on
 * the fd lands here too, since the event loop routes it to the one handler
 * this registration has, and the write that follows reports it. */
static void _async_on_room(ccol_event_loop loop, ccol_event_reg reg,
                           ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)sel;
  chttp_async_ctx_t *ctx = (chttp_async_ctx_t *)arg;
  ccol_event_reg expected = reg;
  if (!atomic_compare_exchange_strong(&ctx->room_reg, &expected,
                                      CCOL_EVENT_REG_INVALID))
    return;
  ccol_event_loop_remove(cli_engine_bundler.reactor, reg);
  if (ctx->hop_completed) {
    _async_ctx_handle_if_abandoned(ctx);
    return;
  }
  if (ctx->state == CHTTP_ASYNC_READING && ctx->upload_paused &&
      (ctx->duplex || _async_upload_may_resume(ctx)))
    _async_upload_resume(ctx);
}

/* See the field comment of ctx->continue_carry. Those bytes are the start
 * of the final response and need to reach the parser before anything a
 * later read takes. The replay can end the hop and free ctx, so every
 * caller makes this its last action.
 *
 * mp goes into a local first because _async_process_reading_data can free
 * ctx completely during this call (a completed final response with no
 * redirect, for example), and after that nothing may touch ctx.
 *
 * We also retain the chain for the whole replay and only drop that
 * reference after carry is freed. The replay may fulfil the chain and drop
 * its last reference, and that's what lets a chttpclient_destroy go on. It
 * blocks until async_in_flight_count reaches zero and then frees the
 * client and, for a client from chttpclient_create_mp, the caller's own
 * ccol_memmgmt_procs_t as well. Without the retain, freeing carry on this
 * thread would read mp->free out of freed memory and call through it. */
static void _async_replay_continue_carry(chttp_async_ctx_t *ctx) {
  if (!ctx->continue_carry) return;
  ccol_memmgmt_procs_t *mp = ctx->mp;
  char *carry = ctx->continue_carry;
  size_t carry_len = ctx->continue_carry_len;
  ctx->continue_carry = NULL;
  ctx->continue_carry_len = 0;
  chttp_async_chain_t *carry_chain = ctx->chain;
  _async_chain_retain(carry_chain);
#ifdef RUNNING_UNIT_TESTS
  _chttp_test_continue_carry_replay_delay();
#endif
  _async_process_reading_data(ctx, carry, carry_len);
#ifdef RUNNING_UNIT_TESTS
  _chttp_test_continue_carry_replay_delay();
#endif
  _ccol_mem_free(mp, carry);
  _async_chain_release(carry_chain);
}

/* Stops sending the request, for a while (paused == true) or for good, and
 * hands the ctx to the read path. It can end the hop and free ctx, so the
 * caller must do nothing after it. */
static void _async_upload_pause(chttp_async_ctx_t *ctx, bool paused) {
  ctx->upload_paused = paused;
  atomic_store(&ctx->flip_watch, CHTTP_FLIP_NONE);
  ctx->state = CHTTP_ASYNC_READING;
  if (_async_reg_modify(ctx, ccol_select_read) != ccol_success) {
    _async_ctx_finish(ctx);
    return;
  }
  /* A duplex send waits for room and for input at the same time, with reg
   * watching input and the room watch the write direction. If the room
   * watch couldn't be added, the deadline sweep turns reg to the write
   * direction once there's room instead, and that costs up to one sweep
   * tick per send buffer of the request. */
  if (paused && ctx->duplex && !_async_room_watch_start(ctx))
    atomic_store(&ctx->flip_watch, CHTTP_FLIP_ON_ROOM);
  _async_replay_continue_carry(ctx);
}

/* Continues a paused send. The write can end the hop and free ctx, so the
 * caller must do nothing after it. */
static void _async_upload_resume(chttp_async_ctx_t *ctx) {
  _async_room_watch_stop(ctx);
  if (ctx->resp_msg_started && ctx->pctx.status_code >= 200 &&
      ctx->pctx.status_code < 300)
    ctx->duplex = true;
  ctx->upload_paused = false;
  ctx->state = CHTTP_ASYNC_WRITING;
  if (ctx->tls) {
    _async_tls_try_write(ctx);
  } else {
    _async_plain_try_write(ctx);
  }
}

/* After this many request bytes since the last check, a successful write
 * looks whether a response arrived in the meantime. A server that answers
 * early and closes with the rest of the request unread resets the
 * connection, and on Linux that reset throws away an answer the client
 * hasn't read yet (a server with a lingering close, like nginx or
 * chttpsvr, keeps reading for a while first). With the check the send
 * finds the answer soon after it arrives even while writes keep
 * succeeding, for one poll(2) per this many bytes. A request that goes out
 * in one write is never checked, so small requests pay nothing. */
#define CHTTP_UPLOAD_INPUT_CHECK_BYTES (32u * 1024u)

/* True when a successful write leaves request bytes to send and a response
 * has started to arrive, which pauses the send (see above). It only polls
 * once CHTTP_UPLOAD_INPUT_CHECK_BYTES or more went out since the last
 * check. */
static bool _async_upload_should_pause(chttp_async_ctx_t *ctx,
                                       bool awaiting_continue,
                                       size_t target_len) {
  if (awaiting_continue || ctx->wire_sent >= target_len ||
      ctx->wire_sent < ctx->upload_check_at)
    return false;
  ctx->upload_check_at = ctx->wire_sent + CHTTP_UPLOAD_INPUT_CHECK_BYTES;
  return _async_input_pending(ctx);
}

/* True when a paused send may continue, either because no message started
 * since the last one the read path discarded, or because the one that did
 * is a final response of 200 to 299 with its header block in. */
static inline bool _async_upload_may_resume(const chttp_async_ctx_t *ctx) {
  return ctx->upload_paused &&
         (!ctx->resp_msg_started ||
          (ctx->pctx.status_code >= 200 && ctx->pctx.status_code < 300));
}

/*
 * Writes as much of ctx->wire[ctx->wire_sent..] as ctls_conn_write takes
 * at this moment and keeps track of partial progress, since a short write
 * is normal and not an error. It runs once right after the handshake
 * completes and then from _async_on_writable every time the write interest
 * fires.
 */
static void _async_tls_try_write(chttp_async_ctx_t *ctx) {
  if (atomic_load_explicit(&ctx->flip_watch, memory_order_relaxed))
    atomic_store_explicit(&ctx->flip_watch, CHTTP_FLIP_NONE,
                          memory_order_relaxed);
  /* With "Expect: 100-continue" we stop where the header ends and the body
   * starts, not at the end of wire. That happens exactly once per hop
   * attempt, because continue_decided turns true as soon as a real
   * "100 Continue" arrives or the wait runs out. The doc comment of
   * CHTTP_ASYNC_AWAITING_CONTINUE has the whole three-outcome contract,
   * which follows _chttp_send_and_read of Tier 1. */
  bool awaiting_continue =
      ctx->want_100_continue && !atomic_load(&ctx->continue_decided);
  size_t target_len = awaiting_continue ? ctx->header_len : ctx->wire_len;
  while (ctx->wire_sent < target_len) {
    ssize_t n;
#ifdef RUNNING_UNIT_TESTS
    if (_tls_dir_inject_blocks(true)) {
      errno = EWOULDBLOCK;
      n = -1;
    } else
#endif
    {
      n = ctls_conn_write(ctx->tls, ctx->wire + ctx->wire_sent,
                          target_len - ctx->wire_sent);
    }
    if (n > 0) {
      ctx->wire_sent += (size_t)n;
      if (_async_upload_should_pause(ctx, awaiting_continue, target_len)) {
        _async_upload_pause(ctx, true);
        return;
      }
      continue;
    }
    if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
      /* A response came in while the request is going out (see
       * _async_upload_pause). The wait for a "100 Continue" reads its own
       * answer, so we leave the header block send alone in that case. */
      if (!awaiting_continue && (ctx->duplex || _async_input_pending(ctx))) {
        _async_upload_pause(ctx, true);
        return;
      }
      /* EWOULDBLOCK from ctls_conn_write() doesn't always mean "wait for
       * writable". OpenSSL may need to READ before this call can make
       * progress, for a TLS 1.2 renegotiation or a TLS 1.3 KeyUpdate that
       * shows up while the send buffer is briefly full (see the doc
       * comment of ctls_conn_wants_write).
       *
       * The reactor is level-triggered. Leaving the registration pinned to
       * the write direction would hand the SHARED process-wide reactor
       * thread an event that is ready at once and on which it can make no
       * progress, and the thread would spin at 100 percent CPU. The
       * read-direction dispatch we flip to comes back into this function
       * through the CHTTP_ASYNC_WRITING branch of _async_on_readable_impl.
       *
       * The direction is set unconditionally, not only when it changes.
       * The read-direction dispatch that consumes a "100 Continue"
       * (_async_awaiting_continue_on_data) enters this function too, with
       * the registration on read, and a write that blocks there needs the
       * flip the other way. ccol_event_loop_modify is idempotent for a
       * direction that's already in effect. */
      ccol_select_dir want = _conn_tls_wants_write(ctx->tls) ? ccol_select_write
                                                             : ccol_select_read;
      if (want == ccol_select_write && !awaiting_continue)
        atomic_store(&ctx->flip_watch, CHTTP_FLIP_ON_INPUT);
#ifdef RUNNING_UNIT_TESTS
      if (want == ccol_select_read) {
        atomic_fetch_add(&g_tls_dir_read_interest_while_writing, 1u);
        _tls_dir_note_wait(false);
      }
#endif
      if (_async_reg_modify(ctx, want) != ccol_success) {
        _async_ctx_finish(ctx);
      }
      return;
    }
    /* n is 0 or a hard error, so the connection is dead, but the peer may
     * have answered before it went away. After a close_notify (n == 0) or
     * a reset (ECONNRESET or EPIPE) that answer is readable, the read path
     * takes it or finds none, and then _async_ctx_finish decides whether a
     * retry is possible (see _async_upload_pause). Any other error ends
     * the hop right here, and _async_ctx_finish makes the same check. */
    if (n == 0 || errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN) {
      _async_upload_pause(ctx, false);
      return;
    }
    _async_ctx_finish(ctx);
    return;
  }
  if (awaiting_continue) {
    /* The whole header block is out and we hold back the body,
     * wire[header_len..wire_len), while waiting for one of three answers
     * from the server: a "100 Continue", a direct final response or a
     * timeout.
     *
     * Unlike the ordinary full-send case below, wire must stay alive
     * here whatever ctx->reused says. The body bytes we have yet to send
     * live in it, and freeing it at this point is a use-after-free on a
     * fresh ctx the next time this hop tries to send the body. */
    /* continue_deadline is computed and stored BEFORE the store to
     * ctx->state below, never after it.
     *
     * The deadline sweep polls ctx->state on its own timer and coordinates
     * through nothing but that one atomic load. A visible
     * CHTTP_ASYNC_AWAITING_CONTINUE is its signal that continue_deadline
     * means something (see the comment of that field).
     *
     * So ctx->state is the real publish point. C11 atomics are
     * sequentially consistent by default, and a concurrent atomic_load of
     * ctx->state that sees this store sees every plain write this thread
     * sequenced before it too.
     *
     * If continue_deadline were written afterwards, the sweep could see the
     * new state with continue_deadline holding a stale or zeroed value
     * (from calloc, or from an earlier hop on a reused ctx). It would take
     * that as expired and fire the wait-timeout path at once, and we would
     * never wait the full CHTTP_100_CONTINUE_WAIT_US. */
    _ccol_mutex_lock(ctx->deadline_lock);
    ctx->continue_deadline = _deadline_earlier(
        _deadline_make(CHTTP_100_CONTINUE_WAIT_US), ctx->overall_deadline);
    chttp_deadline_t continue_dl = ctx->continue_deadline;
    _ccol_mutex_unlock(ctx->deadline_lock);
    _client_deadline_note(&continue_dl);
#ifdef RUNNING_UNIT_TESTS
    _continue_deadline_stored_for_tests();
#endif
    ctx->state = CHTTP_ASYNC_AWAITING_CONTINUE;
    if (_async_reg_modify(ctx, ccol_select_read) != ccol_success) {
      _async_ctx_finish(ctx);
    }
    return;
  }
  /* For a REUSED ctx the wire buffer has to survive a write that looks
   * successful. The local socket buffer took the bytes, but that does NOT
   * mean the peer is alive to answer. _async_retry_hop may run on THIS ctx
   * later, when the read side finds the connection was dead all along, and
   * it needs the ORIGINAL request bytes to send again on a fresh
   * connection.
   *
   * Freeing wire here unconditionally breaks that retry in two ways. Free
   * it and leave wire_len set, and we crash on the stale pointer. Free it
   * and zero wire_len and wire_sent too, and _async_retry_hop moves a NULL
   * wire with a wire_len of 0 into the retry ctx. That one then quietly
   * sends ZERO bytes, since its `wire_sent < wire_len` check is false
   * at once, and its connection sits open until the peer times out the
   * read and closes it.
   *
   * The second one is the harder to notice. You need enough concurrent
   * connections that a write to a reused socket the peer already closed
   * succeeds locally before the read side notices, and there's no data
   * race in it, so ThreadSanitizer stays quiet.
   *
   * A fresh ctx that wasn't reused never gets a retry (see the reused-only
   * check of _async_ctx_finish), so its wire is freed eagerly here and only
   * the reused case keeps it alive.
   *
   * The buffer is then claimed in one of three places: the transfer in
   * _async_retry_hop, a successful _async_idle_pool_offer (which always
   * resets wire for the next hop), or the unconditional free in
   * _async_ctx_free at teardown. */
  if (!ctx->reused) {
    _ccol_mem_free(ctx->mp, ctx->wire);
    ctx->wire = NULL;
    ctx->wire_len = ctx->wire_sent = 0;
  }
  ctx->state = CHTTP_ASYNC_READING;
  if (_async_reg_modify(ctx, ccol_select_read) != ccol_success) {
    _async_ctx_finish(ctx);
  }
}

/*
 * The plain-HTTP counterpart of _async_tls_try_write: write as much of
 * ctx->wire[ctx->wire_sent..] as a raw non-blocking send() will take at
 * the moment.
 *
 * It's called once right after the connect of a fresh connection
 * completes, then from the WRITING branch of _async_on_writable_impl every
 * time the write interest fires. _async_submit_hop on purpose does NOT
 * make the first write attempt of a reused connection synchronously (the
 * reused-connection comment of that function says why).
 *
 * That way the handling of a partial write, an EWOULDBLOCK and a hard
 * failure lives in this one function and no call site repeats it.
 */
static void _async_plain_try_write(chttp_async_ctx_t *ctx) {
  if (atomic_load_explicit(&ctx->flip_watch, memory_order_relaxed))
    atomic_store_explicit(&ctx->flip_watch, CHTTP_FLIP_NONE,
                          memory_order_relaxed);
  /* The same comment in _async_tls_try_write explains why we stop at
   * header_len instead of wire_len while waiting for a "100 Continue". */
  bool awaiting_continue =
      ctx->want_100_continue && !atomic_load(&ctx->continue_decided);
  size_t target_len = awaiting_continue ? ctx->header_len : ctx->wire_len;
  while (ctx->wire_sent < target_len) {
    ssize_t n = send(ctx->fd, ctx->wire + ctx->wire_sent,
                     target_len - ctx->wire_sent, CCOL_MSG_NOSIGNAL);
    if (n > 0) {
      ctx->wire_sent += (size_t)n;
      if (_async_upload_should_pause(ctx, awaiting_continue, target_len)) {
        _async_upload_pause(ctx, true);
        return;
      }
      continue;
    }
    if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
      /* Same check as in _async_tls_try_write. */
      if (!awaiting_continue && (ctx->duplex || _async_input_pending(ctx))) {
        _async_upload_pause(ctx, true);
        return;
      }
      /* With a plain send(2) EWOULDBLOCK always means "wait for writable",
       * but the registration isn't necessarily on the write direction yet,
       * because the read-direction dispatch that consumes a "100 Continue"
       * (_async_awaiting_continue_on_data) enters this function as well.
       *
       * Left on read there, the registration waits for readability the
       * peer has no reason to give us, and the hop stalls until its own
       * deadline. With the default request_timeout_us of 0 that deadline
       * never comes.
       *
       * ccol_event_loop_modify is idempotent for a direction that's
       * already in effect, so the ordinary write dispatch path pays only
       * for the call itself. */
      if (!awaiting_continue)
        atomic_store(&ctx->flip_watch, CHTTP_FLIP_ON_INPUT);
      if (_async_reg_modify(ctx, ccol_select_write) != ccol_success) {
        _async_ctx_finish(ctx);
      }
      return;
    }
    /* Same as the matching branch of _async_tls_try_write. */
    if (n < 0 && (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN)) {
      _async_upload_pause(ctx, false);
      return;
    }
    _async_ctx_finish(ctx);
    return;
  }
  if (awaiting_continue) {
    /* The matching branch of _async_tls_try_write gives the reasoning,
     * both about the lifetime of the wire buffer and about why
     * continue_deadline must be computed and stored BEFORE ctx->state is
     * published as CHTTP_ASYNC_AWAITING_CONTINUE, never after. */
    _ccol_mutex_lock(ctx->deadline_lock);
    ctx->continue_deadline = _deadline_earlier(
        _deadline_make(CHTTP_100_CONTINUE_WAIT_US), ctx->overall_deadline);
    chttp_deadline_t continue_dl = ctx->continue_deadline;
    _ccol_mutex_unlock(ctx->deadline_lock);
    _client_deadline_note(&continue_dl);
#ifdef RUNNING_UNIT_TESTS
    _continue_deadline_stored_for_tests();
#endif
    ctx->state = CHTTP_ASYNC_AWAITING_CONTINUE;
    if (_async_reg_modify(ctx, ccol_select_read) != ccol_success) {
      _async_ctx_finish(ctx);
    }
    return;
  }
  /* Full reasoning in the same comment of _async_tls_try_write. A REUSED
   * ctx keeps its wire buffer past a write that looks successful, since
   * _async_retry_hop needs the original request bytes to resend if the
   * read side then finds the peer was dead already. Only a fresh ctx,
   * which never gets a retry, frees the buffer eagerly here. */
  if (!ctx->reused) {
    _ccol_mem_free(ctx->mp, ctx->wire);
    ctx->wire = NULL;
    ctx->wire_len = ctx->wire_sent = 0;
  }
  ctx->state = CHTTP_ASYNC_READING;
  if (_async_reg_modify(ctx, ccol_select_read) != ccol_success) {
    _async_ctx_finish(ctx);
    return;
  }
#ifdef RUNNING_UNIT_TESTS
  /* See g_force_stray_write_arm_for_tests. We fake a continue-timeout flip
   * of the deadline sweep that lands after this hop has armed its own read
   * direction, at exactly the instruction where the real race would
   * produce it. Each arming consumes the flag once. */
  if (atomic_exchange(&g_force_stray_write_arm_for_tests, false)) {
    _async_reg_modify(ctx, ccol_select_write);
  }
#endif
}

/* Forward declaration for the redirect-detection branches of
 * _async_on_readable below. It's defined after _async_connect_task, which
 * it needs for queueing the next hop. */
static void _async_handle_redirect(chttp_async_ctx_t *ctx, bool reusable);

/*
 * Reads ctx->state and checks ctx->hop_completed once more, both under
 * ctx->idle_lock (the comment of that field explains why an unlocked read
 * of state isn't safe here).
 *
 * The result is CHTTP_ASYNC_DISPATCH_ABANDONED, rather than "not idle",
 * when hop_completed turned true inside a particular window. The caller's
 * entry check (at the very top of _async_on_readable, _async_on_writable
 * or _async_on_error) opens that window and the lock taken here closes it.
 * An application-thread failure path of _async_submit_hop or
 * _async_submit_hop_fail can race this very dispatch (see the field
 * comment of pending_app_teardown), and it sets hop_completed under the
 * same idle_lock.
 *
 * In that case the caller must return at once through
 * _async_ctx_handle_if_abandoned below and never touch ctx itself. Falling
 * through to the active-hop processing would treat the stale state, chain
 * and pctx of an abandoned ctx as a live hop and could hand data to the
 * wrong chain.
 *
 * The first mechanism, the staleness eviction of _async_idle_pool_take,
 * never gets here at all. It touches neither state nor hop_completed of a
 * stale candidate and only calls shutdown() on its fd, so for this
 * function such a candidate is a legitimately IDLE one, and its real
 * teardown goes through the separate _async_idle_ctx_finish path below,
 * not through this ABANDONED case. */
typedef enum {
  CHTTP_ASYNC_DISPATCH_ACTIVE,
  CHTTP_ASYNC_DISPATCH_IDLE,
  CHTTP_ASYNC_DISPATCH_ABANDONED,
} chttp_async_dispatch_kind_t;

static chttp_async_dispatch_kind_t _async_dispatch_kind(
    chttp_async_ctx_t *ctx) {
  _ccol_mutex_lock(ctx->idle_lock);
  bool completed = ctx->hop_completed;
  bool idle = (ctx->state == CHTTP_ASYNC_IDLE);
  _ccol_mutex_unlock(ctx->idle_lock);
  if (completed) return CHTTP_ASYNC_DISPATCH_ABANDONED;
  return idle ? CHTTP_ASYNC_DISPATCH_IDLE : CHTTP_ASYNC_DISPATCH_ACTIVE;
}

/*
 * Every dispatch callback path that finds ctx->hop_completed already true
 * calls this. It finds it in one of two ways: the quick check at the top
 * of _async_on_writable_impl, _async_on_readable_impl and
 * _async_on_error_impl, BEFORE they even call _async_dispatch_kind, or the
 * ABANDONED result of _async_dispatch_kind itself, which covers the
 * narrower race window between that top check and the re-check under
 * idle_lock.
 *
 * hop_completed can be true for two reasons. Either an earlier dispatch of
 * this same registration already ran the real teardown before setting it,
 * and there's nothing left to do. Or an application-thread failure path of
 * _async_submit_hop or _async_submit_hop_fail set hop_completed AND
 * pending_app_teardown, shut ctx->fd down and returned without touching
 * ctx again, leaving the real destructive teardown on purpose to whichever
 * dispatch notices (see the field comment of pending_app_teardown for the
 * reasoning).
 *
 * pending_app_teardown tells the two apart, and only the second case calls
 * _async_ctx_teardown. We don't call _async_ctx_finish: hop_completed is
 * already true and any legitimate retry already went to _async_retry_hop
 * before pending_app_teardown was set, so running _async_ctx_finish's
 * retry check a second time here would be wrong.
 *
 * In both cases every caller must return right after, since ctx may be
 * completely freed by the time this returns.
 *
 * The top checks of _async_on_readable_impl and _async_on_error_impl are
 * really reachable in the second case. The shutdown(fd, SHUT_RDWR) of the
 * second mechanism relies on the kernel delivering EPOLLIN or EPOLLERR on
 * this fd, which is registered for read, and that goes to exactly
 * one of those two functions, whose top check runs before anything gets to
 * _async_dispatch_kind.
 *
 * If either of those two checks didn't call this, the second mechanism
 * would quietly stop working: ctx would leak for good along with its chain
 * reference, and chttpclient_destroy would hang forever waiting for
 * async_in_flight_count to reach zero.
 *
 * The top check of _async_on_writable_impl can't be reached in the second
 * case, because ccol_event_loop_modify never changes a registration's
 * direction on any failure path it can return through, so a reused ctx
 * whose reactivation failed stays registered for read and never for write.
 * It gets the same treatment anyway so correctness doesn't depend on that
 * staying true forever, just like the hop_completed check of this function
 * that the file documents as impossible for the read-only case and keeps
 * anyway.
 *
 * It can never fire twice, neither inside one dispatch nor across separate
 * ones. Inside one call, whichever of a function's two checks sees
 * hop_completed true first calls this and returns, so the other check of
 * that function can't be reached after it.
 *
 * Across calls, _ccol_event_loop_poller_collect in cthreadcomm.c refuses to
 * submit a second job for an entry while entry->refcount is above 0, and
 * _async_ctx_teardown gets to _async_ctx_destroy_now, which calls
 * ccol_event_loop_remove before returning. That sets reg->removed, so no
 * later poller pass builds another job item for this reg.
 * entry->dispatch_lock also serialises one entry's callbacks, so a dispatch
 * reaches _async_ctx_teardown at most once per ctx.
 */
static void _async_ctx_handle_if_abandoned(chttp_async_ctx_t *ctx) {
  if (ctx->pending_app_teardown) _async_ctx_teardown(ctx);
}

/* Publishes reg, the handle ccol_event_loop passes to every dispatch of this
 * ctx, into ctx->reg while ctx->reg is CCOL_EVENT_REG_INVALID. A dispatch can
 * run before the `ctx->reg = ccol_event_loop_add(...)` in
 * _async_connect_task has finished, and every path below reads ctx->reg to
 * modify or remove the registration. A ctx has exactly one registration in
 * its whole life, so what we adopt here and what ccol_event_loop_add returns
 * are the same handle. The compare-and-exchange leaves an already set field
 * alone. */
static inline void _async_adopt_reg(chttp_async_ctx_t *ctx,
                                    ccol_event_reg reg) {
  ccol_event_reg expected = CCOL_EVENT_REG_INVALID;
  atomic_compare_exchange_strong(&ctx->reg, &expected, reg);
}

/* Forward declaration. _async_on_writable_impl below hands work to this for
 * a TLS ctx whose pending READ asked for the write direction, so the
 * write-direction dispatch caused by the flip drives the read it was
 * flipped for. The definition comes later, next to the rest of the read
 * path. */
static void _async_on_readable_impl(ccol_event_loop loop, ccol_event_reg reg,
                                    ccol_selectable *sel,
                                    chttp_async_ctx_t *ctx);

/*
 * Once per hop on a connection from the idle pool, before the first request
 * byte goes out, this tells whether the connection is really empty. A
 * pooled connection only counts as idle while nothing arrives on it. Input
 * after a complete response answers no request, and end of stream means the
 * peer is gone. Either one evicts the ctx through the IDLE dispatch of
 * _async_on_readable_impl, but _async_idle_pool_take can beat that dispatch,
 * and then the new hop would read those bytes as the start of its own
 * response.
 *
 * The check runs on a dispatch of this registration, which ccol_event_loop
 * serialises against all the others, so it's allowed to read the TLS layer.
 * ctls_conn_read lets OpenSSL swallow a record with no application data,
 * such as a TLS 1.3 NewSessionTicket, which a raw peek would take for
 * input. A plain socket gets a MSG_PEEK. We also look at ctx->idle_tainted,
 * set by the IDLE dispatch when its own read took the input. Tier 1's
 * _idle_pool_take makes the same check at take time.
 *
 * Nothing of the request has gone out at this point, so a connection that
 * fails the check is retried on a fresh one through the retry-once path of
 * _async_ctx_finish, just like a reused connection the peer has closed.
 */
static bool _async_reused_conn_is_clean(chttp_async_ctx_t *ctx) {
  ctx->reuse_checked = true;
  if (ctx->idle_tainted) return false;
  char probe;
  ssize_t pn;
  if (ctx->tls) {
    pn = ctls_conn_read(ctx->tls, &probe, 1);
  } else {
    do {
      pn = recv(ctx->fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
    } while (pn < 0 && errno == EINTR);
  }
  return pn < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

/* Starts or continues the send of a ctx in CHTTP_ASYNC_WRITING. It must be
 * the caller's last action, because it can end the hop and free ctx. */
static void _async_drive_write(chttp_async_ctx_t *ctx) {
  if (ctx->reused && !ctx->reuse_checked && ctx->wire_sent == 0 &&
      !_async_reused_conn_is_clean(ctx)) {
    _async_ctx_finish(ctx);
    return;
  }
  if (ctx->tls) {
    _async_tls_try_write(ctx);
  } else {
    _async_plain_try_write(ctx);
  }
}

/*
 * The tail of a write-direction dispatch for a ctx that has nothing of ours
 * to flush in that direction.
 */
static void _async_writable_rest(chttp_async_ctx_t *ctx) {
  /*
   * Every state that gets here has nothing of ours left to flush in the
   * write direction: CHTTP_ASYNC_READING on a plaintext connection, and
   * CHTTP_ASYNC_IDLE, whose home direction is read. The TLS version of the
   * first one is handled by the code above.
   *
   * A stray write-direction dispatch can reach both states. The deadline
   * sweep's continue-timeout flip samples ctx->state and then issues its
   * ccol_event_loop_modify, and in between a dispatch on another thread can
   * move the same ctx out of CHTTP_ASYNC_AWAITING_CONTINUE, so the sweep's
   * flip can land after that dispatch's modify.
   *
   * Putting the read direction back here limits that to one wasted
   * dispatch. Otherwise the registration stays armed for write with nothing
   * to write, and because a connected socket is nearly always writable,
   * the shared process-wide client reactor keeps dispatching this no-op,
   * burning one of its threads and slowing down every other connection on
   * the engine.
   *
   * The hop itself could never make progress either, since its read
   * interest is gone and nothing else issues another modify. With the
   * default request_timeout_us of 0 there's no deadline to end it, so
   * chttpclient_async_result_get and chttpclient_destroy would block
   * forever.
   *
   * Reading the state and the modify both happen under ctx->idle_lock,
   * which _async_idle_pool_take holds across the whole reactivation of a
   * pooled ctx (see its doc comment). A reactivation may arm the write
   * direction for a hop that really has bytes to send, and holding the
   * lock is what keeps this restore from landing on top of it.
   */
  _ccol_mutex_lock(ctx->idle_lock);
  chttp_async_state_t resting_state = ctx->state;
  bool restore = (resting_state == CHTTP_ASYNC_READING ||
                  resting_state == CHTTP_ASYNC_IDLE);
  ccol_retval_t mrv = restore
                          ? ccol_event_loop_modify(cli_engine_bundler.reactor,
                                                   ctx->reg, ccol_select_read)
                          : ccol_success;
  _ccol_mutex_unlock(ctx->idle_lock);
#ifdef RUNNING_UNIT_TESTS
  if (restore)
    atomic_fetch_add_explicit(&g_stray_write_arm_restores_for_tests, 1,
                              memory_order_relaxed);
#endif
  /* An idle ctx whose registration refuses a modify is already
   * unregistered and can't spin. The idle pool's staleness handling will
   * pick it up; this stray dispatch doesn't tear it down. */
  if (mrv != ccol_success && resting_state == CHTTP_ASYNC_READING)
    _async_ctx_finish(ctx);
}

static void _async_on_writable_impl(ccol_event_loop loop, ccol_event_reg reg,
                                    ccol_selectable *sel,
                                    chttp_async_ctx_t *ctx) {
  (void)loop;
  (void)sel;
  /* See the field comment of ctx->hop_completed. A stray dispatch can show
   * up after this ctx has reached a terminal outcome, and it must not run
   * any of the state transition logic below a second time.
   *
   * An idle pooled ctx is registered for read (see the "ASYNC IDLE POOL"
   * section), so a write-direction dispatch for one can only be the stray
   * left by the deadline sweep's continue-timeout flip, and
   * _async_writable_rest deals with it after the classification below.
   *
   * The comment of _async_ctx_handle_if_abandoned explains why we make
   * this call here even though nothing reaches this path. */
  if (ctx->hop_completed) {
    _async_ctx_handle_if_abandoned(ctx);
    return;
  }
  /* See the field comment of ctx->reg and _async_adopt_reg. A
   * write-direction dispatch for this ctx's very first registration can
   * arrive before the `ctx->reg = ccol_event_loop_add(...)` in
   * _async_connect_task finishes, since a loopback connect is often
   * writable the instant it's registered. */
  _async_adopt_reg(ctx, reg);

  /* The classification takes idle_lock, like the read and error dispatches.
   * Taking a pooled ctx sets its state to WRITING under that lock and then
   * builds the new hop (wire, parser, reuse flag) before arming the write
   * direction. Without the lock, a stray write dispatch from an earlier hop
   * running in that window would drive the write of a half-built hop, send
   * nothing and leave the request unsent. Waiting on the lock lets the take
   * finish first. */
  chttp_async_dispatch_kind_t kind = _async_dispatch_kind(ctx);
  if (kind == CHTTP_ASYNC_DISPATCH_ABANDONED) {
    _async_ctx_handle_if_abandoned(ctx);
    return;
  }
  /* An IDLE ctx goes straight to the tail that reads the state again under
   * idle_lock. A take can move it to WRITING right after the
   * classification, and reading the state here without the lock would then
   * drive the write of a hop the take is building. An ACTIVE state
   * can't change under this dispatch, because only a take leaves IDLE and
   * the dispatches of one fd run one at a time. */
#ifdef RUNNING_UNIT_TESTS
  if (kind == CHTTP_ASYNC_DISPATCH_IDLE) {
    int stray_stage = 2;
    if (atomic_compare_exchange_strong(&g_stray_before_take_stage_for_tests,
                                       &stray_stage, 3))
      _stray_before_take_wait_for_tests(4);
  }
#endif
  if (kind == CHTTP_ASYNC_DISPATCH_IDLE) {
    _async_writable_rest(ctx);
    return;
  }

  if (ctx->state == CHTTP_ASYNC_CONNECTING) {
    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    if (getsockopt(ctx->fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0 ||
        soerr != 0) {
      /* ctx->timed_out is checked first (it's _Atomic, so a plain read is
       * race-free). The shutdown(fd, SHUT_RDWR) the deadline sweep does on a
       * stuck connect shows up here as a getsockopt or SO_ERROR failure,
       * and without this check an expired connect_timeout_us would be
       * reported as ccol_http_connection_failed instead of the
       * ccol_timed_out that chttpclient.h documents. */
      _async_connect_failed(ctx);
      return;
    }
    /* The connect is up and we need no further candidate. Otherwise the list
     * would hang around on a pooled connection for its whole idle life. */
    _ccol_mem_free(ctx->mp, ctx->addrs);
    ctx->addrs = NULL;
    if (!ctx->is_unix) _apply_tcp_nodelay(ctx->fd);

    if (ctx->is_https) {
      /* A configured cert, key or CA path can be unreadable, and that
       * failure is deferred instead of failing chttpclient_set_tls, as the
       * comment of _rebuild_tls_ctx_locked in Tier 1 describes.
       *
       * _chttp_do_async_internal already checks it synchronously, through
       * tls_ctx_usable, before opening any connection, so by the time we
       * get here TLS really is usable. */
#ifdef RUNNING_UNIT_TESTS
      /* Every connection that does its handshake here is fresh, since a
       * reused ctx skips CONNECTING and TLS_HANDSHAKING. It's set up under
       * chain->tls_ctx, so it must already carry that chain's generation.
       *
       * A ctx arriving here with any other value would go back to the idle
       * pool labelled with a configuration its handshake never used, and
       * _async_idle_pool_take would then give it to a later request because
       * of that label.
       *
       * The check reads two fields this dispatch already owns, and
       * chain->tls_generation is written once when the chain is created and
       * never again, so the check keeps no state and can't race. */
      if (ctx->tls_generation != ctx->chain->tls_generation) {
        _ccol_fatal_err(
            "chttpclient: a fresh TLS connection carries a tls_generation "
            "other than its own chain's");
      }
#endif
      ccol_retval_t crv;
      ctx->tls = _tls_client_create(ctx->chain->tls_ctx, ctx->fd, ctx->host,
                                    ctx->verify_host, &crv);
      if (!ctx->tls) {
        _async_fulfill(ctx, crv, NULL);
        _async_ctx_finish(ctx);
        return;
      }
      ctx->state = CHTTP_ASYNC_TLS_HANDSHAKING;
      _async_tls_advance(ctx);
      return;
    }

    ctx->state = CHTTP_ASYNC_WRITING;
    /* Fall through to the WRITING branch below, which tries the first write
     * immediately since the fd is writable now, rather than waiting for
     * another on_writable dispatch. */
  }

  if (ctx->state == CHTTP_ASYNC_TLS_HANDSHAKING) {
    _async_tls_advance(ctx);
    return;
  }

  if (ctx->state == CHTTP_ASYNC_WRITING) {
    _async_drive_write(ctx);
    return;
  }
  if (ctx->state == CHTTP_ASYNC_AWAITING_CONTINUE) {
    /* Only one thing gets here: the deadline sweep's continue_deadline
     * expiry check, which flips this registration to the write direction to
     * deliver this dispatch (see the "ASYNC DEADLINE SWEEP" section). While
     * a hop waits for a continue we never have anything else to flush in
     * this direction, unlike the ordinary WRITING state above.
     *
     * Claim the decision first, like every exit of
     * _async_awaiting_continue_on_data. A real "100 Continue" can arrive and
     * a racing on_readable dispatch can claim it just before this one runs.
     * ccol_event_loop's per-entry dispatch_lock orders the two but doesn't
     * pick the winner, and if the other one won it has already taken over
     * sending the body and there's nothing left for us. */
    if (atomic_load(&ctx->continue_msg_started)) {
      /* A message started inside the window and the parser holds its
       * beginning. The window only bounds the wait for a first byte, so the
       * decision belongs to that message, whose rest is bounded by
       * overall_deadline, which the sweep enforces. Hand the registration
       * back to the read direction and leave the decision unclaimed. */
      if (_async_reg_modify(ctx, ccol_select_read) != ccol_success)
        _async_ctx_finish(ctx);
      return;
    }
    /* Only this hop's own window decides. The sweep judges a snapshot it
     * took earlier, and on a pooled ctx that snapshot may belong to an
     * earlier hop whose window had ended while this hop has only just
     * started its own. This dispatch runs on the thread that owns the hop,
     * so it reads the hop's own deadline. If the window is open the
     * registration goes back to the read direction, and a flip that came
     * too early costs just this one dispatch. */
    {
      _ccol_mutex_lock(ctx->deadline_lock);
      chttp_deadline_t own_dl = ctx->continue_deadline;
      _ccol_mutex_unlock(ctx->deadline_lock);
      int own_ms;
      if (_deadline_remaining_ms(&own_dl, &own_ms)) {
        if (_async_reg_modify(ctx, ccol_select_read) != ccol_success)
          _async_ctx_finish(ctx);
        return;
      }
    }
    if (atomic_exchange(&ctx->continue_decided, true)) return;
    /* The wait timed out with no answer either way, so we send the body
     * anyway, like curl's CURLOPT_EXPECT_100_TIMEOUT_MS and the same timeout
     * branch of _chttp_send_and_read in Tier 1.
     *
     * ctx->retry_unsafe stays false here on purpose (see its field
     * comment). Unlike a confirmed "100 Continue", a plain timeout proves
     * nothing about the peer being alive, so the usual retry-once safety
     * net for a reused connection applies if this write, or the read after
     * it, finds the connection was already dead. */
    /* See the field comment of ctx->interim_responses_seen. The same
     * timeout branch in Tier 1 reads the final response with a fresh
     * _chttp_read_message_loop call that has its own counter, whatever
     * ended the continue wait.
     *
     * So we reset the counter here too, not only on a confirmed "100
     * Continue". Otherwise a hop that dropped interim responses during the
     * wait would start reading its final response with less than the
     * documented budget of 64 responses. */
    ctx->interim_responses_seen = 0;
    ctx->state = CHTTP_ASYNC_WRITING;
    if (ctx->tls) {
      _async_tls_try_write(ctx);
    } else {
      _async_plain_try_write(ctx);
    }
    return;
  }
  if (ctx->state == CHTTP_ASYNC_READING && ctx->upload_paused &&
      (ctx->duplex || _async_upload_may_resume(ctx))) {
    /* A paused send whose registration the deadline sweep turned to the
     * write direction because there's room to write (see
     * _async_upload_pause). */
    _async_upload_resume(ctx);
    return;
  }
  if (ctx->state == CHTTP_ASYNC_READING && ctx->tls) {
    /* Except for a paused send, a TLS ctx waiting for its response only has
     * write interest because the EWOULDBLOCK branch of
     * _async_on_readable_impl put it there, when ctls_conn_read had to write
     * before it could decrypt another record.
     *
     * Put the read direction back first, so a read that succeeds now leaves
     * the registration in its home direction, then drive the read this
     * dispatch was flipped for. If that read blocks on the write direction
     * again it flips it back itself. */
    if (_async_reg_modify(ctx, ccol_select_read) != ccol_success) {
      _async_ctx_finish(ctx);
      return;
    }
    _async_on_readable_impl(loop, reg, sel, ctx);
    return;
  }

  _async_writable_rest(ctx);
}

/*
 * ccol_event_loop has its own public callback shape: ccol_event_writable_fn,
 * ccol_event_readable_fn and ccol_event_error_fn all take a `void *arg`, the
 * value the caller gave to ccol_event_loop_add. These thin wrappers only
 * cast `arg` back to a chttp_async_ctx_t* and pass the work on to the real
 * logic in _async_on_writable_impl, _async_on_readable_impl and
 * _async_on_error_impl below.
 *
 * They do no pinning or refcounting of any kind on purpose. A per-ctx
 * atomic refcount pinned here can't close the use-after-free underneath:
 * the pin's very first read of that refcount races a concurrent free in
 * exactly the same way (the field comment of pending_app_teardown explains
 * why).
 *
 * Safety rests on a single rule: no application thread ever frees a
 * registered ctx. So only a dispatch callback can ever run the real
 * destructive teardown of a ctx these wrappers dispatch for, and
 * ccol_event_loop makes sure that happens at most once per ctx with no
 * other thread touching it at the same time, through its
 * entry->dispatch_lock and its invariant of one job in flight per entry
 * (entry->refcount, see cthreadcomm.c).
 */
static void _async_on_writable(ccol_event_loop loop, ccol_event_reg reg,
                               ccol_selectable *sel, void *arg) {
  _async_on_writable_impl(loop, reg, sel, (chttp_async_ctx_t *)arg);
}

/*
 * Parses data[0..data_len) with ctx->parser as one or more messages in the
 * CHTTP_ASYNC_READING state, the same as an ordinary on_readable dispatch.
 *
 * It has two callers. One is the READING-state read of
 * _async_on_readable_impl, the normal case with data straight off the
 * socket. The other is the continue_carry replay of _async_upload_pause,
 * with bytes that came off the socket in the same read as a "100 Continue"
 * during the AWAITING_CONTINUE wait, while the body send waits for this
 * function to decide about them.
 *
 * Both hand us a message boundary ctx->parser has never seen, so the loop
 * and reset logic doesn't need to care which of the two it came from.
 *
 * Looping instead of making a single chttp1_parser_execute call is what
 * lets us drop an interim 1xx response and keep parsing the next message,
 * which may sit in THIS SAME chunk when a fast server wrote both into one
 * buffer. Without the loop the first 1xx would be taken as the final
 * response.
 *
 * By the time this runs every "Expect: 100-continue" decision has been
 * made: _async_awaiting_continue_on_data owns it and nothing reaches it from
 * here. So every 1xx is dropped unconditionally, and only a final response
 * of 200 or above (which a redirect always is) ends the loop, like the
 * general path of _chttp_read_message_loop in Tier 1.
 *
 * `data` and `data_len` move forward from one iteration to the next, and
 * the bytes after a dropped message go to a freshly initialized
 * ctx->parser, because chttp1_parser_execute's contract forbids feeding
 * more bytes to an instance already in CHTTP1_PAUSED, so every later
 * message needs a new one. That's the same way _async_submit_hop and
 * _async_retry_hop set up ctx->parser for a brand-new hop, and Tier 1's
 * "one fresh parser per message" convention.
 */
static void _async_process_reading_data(chttp_async_ctx_t *ctx,
                                        const char *data, size_t data_len) {
  for (;;) {
    if (data_len > 0) {
      ctx->resp_msg_started = true;
      if (ctx->upload_paused) ctx->answered_early = true;
    }
    chttp1_errno_t err = chttp1_parser_execute(&ctx->parser, data, data_len);
    if (err == CHTTP1_PAUSED) {
      /* The message is complete and the parser deliberately pauses right
       * after it, as in Tier 1's CHTTP1_PAUSED handling. */
      size_t consumed = chttp1_parser_consumed(&ctx->parser);
      bool trailing_this_msg = consumed < data_len;

      if (ctx->pctx.status_code >= 100 && ctx->pctx.status_code < 200) {
        /* An interim informational response ahead of the real one, e.g. a
         * "103 Early Hints" from RFC 8297. Drop it and keep reading on the
         * same connection, like the general path of
         * _chttp_read_message_loop in Tier 1.
         *
         * It's never terminal: ctx->hop_completed stays false, and no
         * _async_handle_redirect, fulfil or teardown happens here.
         *
         * It's capped, the same as Tier 1's loop (see the comment of
         * CHTTP_MAX_INTERIM_RESPONSES). Without the cap a server that never
         * stops sending interim responses would keep this ctx alive forever,
         * along with the chain and future waiting on it. */
        if (++ctx->interim_responses_seen > CHTTP_MAX_INTERIM_RESPONSES) {
          ctx->hop_completed = true;
          _async_fulfill(ctx, ccol_http_transfer_aborted, NULL);
          _async_ctx_finish(ctx);
          return;
        }
        ccol_retval_t rrv = _parse_ctx_reset_for_continue(&ctx->pctx);
        if (rrv != ccol_success) {
          ctx->hop_completed = true;
          _async_fulfill(ctx, ccol_not_enough_memory, NULL);
          _async_ctx_finish(ctx);
          return;
        }
        _async_parser_start(ctx);
        ctx->resp_msg_started = false;
        ctx->answered_early = false;
        if (!trailing_this_msg) {
          /* An interim response doesn't answer the request. A send it
           * paused carries on; otherwise we wait for more bytes through
           * on_readable. */
          if (ctx->upload_paused) _async_upload_resume(ctx);
          return;
        }
        data += consumed;
        data_len -= consumed;
        continue; /* Parse the bytes that are left as the next message. */
      }

      /* A final response of 200 or above, or a redirect (a 3xx, so also 200
       * or above). */
      ctx->hop_completed = true;
      if (trailing_this_msg) ctx->pctx.trailing_garbage = true;
      /* A connection whose request didn't fully go out is never reused (see
       * _async_upload_pause). */
      bool keep_alive = chttp1_should_keep_alive(&ctx->parser) &&
                        !ctx->pctx.trailing_garbage && _async_request_sent(ctx);

      if (ctx->pctx.will_redirect) {
        _async_handle_redirect(ctx, keep_alive); /* Closes the connection
                                                  * or pools it. */
      } else {
        /* Copy chain out and build the response BEFORE calling
         * _async_finish_connection.
         *
         * The copy takes a temporary extra retain (the same retain in
         * _async_handle_redirect says why), because _async_finish_
         * connection below can start a concurrent teardown of THIS ctx's
         * chain reference on another reactor thread, and if nothing else
         * holds chain that teardown can free it before _async_fulfill_chain
         * below runs.
         *
         * With keep_alive true that call may also offer ctx to the idle pool
         * successfully, and then _async_idle_pool_offer sets ctx->chain to
         * NULL and resets ctx->pctx and ctx->bb for reuse, which is a normal
         * successful outcome. So ctx->chain must be copied out and
         * ctx->pctx and ctx->bb extracted first, or a NULL ctx->chain
         * crashes the fulfil below, or the response is built from fields
         * the offer has already cleared.
         *
         * Finishing the connection before the fulfil matters for its own
         * sake too. A fulfil can unblock the caller immediately (a
         * ctpool_future_get on another thread, say), which may then destroy
         * cli, while the idle pool offer path of _async_finish_connection
         * touches cli->lock afterwards: a race ending in a use-after-free. */
        chttp_async_chain_t *chain = ctx->chain;
        _async_chain_retain(chain);
        chttpcli_response *resp = _async_build_response(ctx);
        _async_finish_connection(ctx, keep_alive);
        _async_fulfill_chain(
            chain, resp ? ccol_success : ccol_not_enough_memory, resp);
        _async_chain_release(chain);
      }
      return;
    }
    if (err == CHTTP1_USER) {
      ctx->hop_completed = true;
      _async_fulfill(ctx,
                     ctx->pctx.too_large
                         ? ccol_msg_too_large
                         : (ctx->pctx.error ? ccol_not_enough_memory
                                            : ccol_http_transfer_aborted),
                     NULL);
      _async_ctx_finish(ctx);
      return;
    }
    if (err != CHTTP1_OK) {
      ctx->hop_completed = true;
      _async_fulfill(ctx, ccol_http_transfer_aborted, NULL);
      _async_ctx_finish(ctx);
      return;
    }
    /* The message isn't complete yet, so wait for another on_readable. A
     * send paused by the start of this message carries on for a final
     * response of 200 to 299, whose server may stream it while reading the
     * request, and stops for good for 300 or above (see
     * _async_upload_pause). */
    if (ctx->upload_paused && ctx->pctx.status_code >= 200) {
      if (ctx->pctx.status_code < 300) {
        _async_upload_resume(ctx);
        return;
      }
      ctx->upload_paused = false;
    }
    return;
  }
}

/*
 * Handles data[0..data_len) arriving while ctx->state is
 * CHTTP_ASYNC_AWAITING_CONTINUE: the header block is out and we're holding
 * the body back on purpose until the server answers.
 *
 * It's exactly the post-header wait of _chttp_send_and_read in Tier 1 (that
 * function's doc comment gives the contract with its three outcomes),
 * only event-driven.
 *
 * It only ever runs from _async_on_readable_impl after a real byte has
 * arrived, never from the timeout path the deadline sweep triggers (that's
 * the CHTTP_ASYNC_AWAITING_CONTINUE branch of _async_on_writable_impl).
 *
 * ctx->continue_decided, an _Atomic bool, is the one guard that decides
 * whether this function or the timeout path gets to act (its own comment
 * analyses the race in full).
 *
 * Every exit here that would act on the parsed message claims the guard
 * first with atomic_exchange, and quietly backs off if the decision was
 * already made, which happens when the timeout path gets ahead of a
 * response that really is late.
 */
static void _async_awaiting_continue_on_data(chttp_async_ctx_t *ctx,
                                             const char *data,
                                             size_t data_len) {
  for (;;) {
    /* Every byte here is part of a message (see the field comment of
     * continue_msg_started). */
    if (data_len > 0) atomic_store(&ctx->continue_msg_started, true);
    chttp1_errno_t err = chttp1_parser_execute(&ctx->parser, data, data_len);
    if (err == CHTTP1_PAUSED) {
      size_t consumed = chttp1_parser_consumed(&ctx->parser);
      bool trailing_this_msg = consumed < data_len;

      if (ctx->pctx.status_code == 100) {
        /* The expected outcome: the server confirms it wants the body.
         * Claim the decision first. If the timeout path got it already
         * there's nothing left to do, since that path has taken over the
         * body send and this "100 Continue" came too late to matter. */
        if (atomic_exchange(&ctx->continue_decided, true)) return;

        ccol_retval_t rrv = _parse_ctx_reset_for_continue(&ctx->pctx);
        if (rrv != ccol_success) {
          ctx->hop_completed = true;
          _async_fulfill(ctx, ccol_not_enough_memory, NULL);
          _async_ctx_finish(ctx);
          return;
        }
        _async_parser_start(ctx);
        /* We're about to give the body to a connection the server itself
         * just confirmed as alive and reading (see the field comment of
         * ctx->retry_unsafe), and from then on, whatever happens, that rules
         * out the usual retry-once safety net for a reused connection. */
        ctx->retry_unsafe = true;
        /* A real "100 Continue" is a message we don't drop, so the run of
         * CONSECUTIVE dropped interim responses this field counts starts
         * over here.
         *
         * See the field comment of ctx->interim_responses_seen. Without the
         * reset, interim responses dropped during THIS wait would quietly
         * eat into the budget for reading the final response, while Tier 1's
         * _chttp_send_and_read gives the two phases to separate
         * _chttp_read_message_loop calls with their own counters. */
        ctx->interim_responses_seen = 0;

        if (trailing_this_msg) {
          /* The same read also holds the start of the next message after
           * the "100 Continue", so the body send starts out paused, just as
           * when a message starts arriving while the body is going out. The
           * read path parses those bytes first and only lets the send go on
           * for an interim response or a final one of 200 to 299
           * (_async_upload_may_resume). A final response of 300 or above, or
           * a complete one, ends the send before a single body byte goes out,
           * and the connection is never reused (_async_request_sent).
           * Without the pause the whole body would go out after a final
           * answer, and a server that answered and stopped reading would
           * leave the send waiting for room until the deadline.
           *
           * See the field comment of ctx->continue_carry, which is how
           * _async_upload_pause hands the bytes to the read path. */
          ctx->continue_carry =
              (char *)_ccol_mem_alloc(ctx->mp, data_len - consumed);
          if (!ctx->continue_carry) {
            ctx->hop_completed = true;
            _async_fulfill(ctx, ccol_not_enough_memory, NULL);
            _async_ctx_finish(ctx);
            return;
          }
          memcpy(ctx->continue_carry, data + consumed, data_len - consumed);
          ctx->continue_carry_len = data_len - consumed;
          _async_upload_pause(ctx, true);
          return;
        }

        ctx->state = CHTTP_ASYNC_WRITING;
        if (ctx->tls) {
          _async_tls_try_write(ctx);
        } else {
          _async_plain_try_write(ctx);
        }
        return;
      }

      if (ctx->pctx.status_code >= 100 && ctx->pctx.status_code < 200) {
        /* Some OTHER interim response (a "103 Early Hints", say) came
         * before a "100 Continue" or a direct final answer. Drop it and keep
         * waiting inside the same continue_deadline window, like Tier 1's
         * _chttp_read_message_loop(stop_at_status=100), which also drops
         * everything that isn't the status it waits for. */
        if (++ctx->interim_responses_seen > CHTTP_MAX_INTERIM_RESPONSES) {
          if (atomic_exchange(&ctx->continue_decided, true)) return;
          ctx->hop_completed = true;
          _async_fulfill(ctx, ccol_http_transfer_aborted, NULL);
          _async_ctx_finish(ctx);
          return;
        }
        ccol_retval_t rrv = _parse_ctx_reset_for_continue(&ctx->pctx);
        if (rrv != ccol_success) {
          if (atomic_exchange(&ctx->continue_decided, true)) return;
          ctx->hop_completed = true;
          _async_fulfill(ctx, ccol_not_enough_memory, NULL);
          _async_ctx_finish(ctx);
          return;
        }
        _async_parser_start(ctx);
        if (!trailing_this_msg) {
          /* The interim message ended on a boundary, so the window keeps
           * running for the first byte of the next message. */
          atomic_store(&ctx->continue_msg_started, false);
          return; /* Wait for more bytes through
                   * on_readable. */
        }
        data += consumed;
        data_len -= consumed;
        continue;
      }

      /* A final response of 200 or above came directly, with no
       * "100 Continue" first. RFC 7231 SS5.1.1 fully allows that, as a way
       * for a server to reject a request without ever wanting the body. It
       * IS the final answer and the body must never be sent.
       *
       * Such a connection never qualifies for keep-alive, whatever
       * chttp1_should_keep_alive and the response's Connection header say
       * (see the same comment in _chttp_send_and_read, this branch's Tier 1
       * counterpart). The peer may be expecting the body we held
       * back, so pooling the connection would desynchronize the next
       * unrelated request that reuses it. */
      if (atomic_exchange(&ctx->continue_decided, true)) return;
      ctx->hop_completed = true;
      if (trailing_this_msg) ctx->pctx.trailing_garbage = true;
      if (ctx->pctx.will_redirect) {
        _async_handle_redirect(ctx, false);
      } else {
        chttp_async_chain_t *chain = ctx->chain;
        _async_chain_retain(chain);
        chttpcli_response *resp = _async_build_response(ctx);
        _async_finish_connection(ctx, false);
        _async_fulfill_chain(
            chain, resp ? ccol_success : ccol_not_enough_memory, resp);
        _async_chain_release(chain);
      }
      return;
    }
    if (err == CHTTP1_USER) {
      if (atomic_exchange(&ctx->continue_decided, true)) return;
      ctx->hop_completed = true;
      _async_fulfill(ctx,
                     ctx->pctx.too_large
                         ? ccol_msg_too_large
                         : (ctx->pctx.error ? ccol_not_enough_memory
                                            : ccol_http_transfer_aborted),
                     NULL);
      _async_ctx_finish(ctx);
      return;
    }
    if (err != CHTTP1_OK) {
      if (atomic_exchange(&ctx->continue_decided, true)) return;
      ctx->hop_completed = true;
      _async_fulfill(ctx, ccol_http_transfer_aborted, NULL);
      _async_ctx_finish(ctx);
      return;
    }
    /* The message isn't complete yet, it may stop in the middle of a status
     * line or a header line. Wait for more bytes through on_readable, within
     * continue_deadline. */
    return;
  }
}

static void _async_on_readable_impl(ccol_event_loop loop, ccol_event_reg reg,
                                    ccol_selectable *sel,
                                    chttp_async_ctx_t *ctx) {
  (void)loop;
  (void)sel;

  /* An earlier dispatch of this ctx has already brought the hop to a
   * terminal outcome: a fulfilled future (a TLS handshake failure found
   * mid-handshake included), or a handoff to a redirect hop.
   *
   * An extra spurious on_readable can come after that. Every route of
   * the tests' mock server sends "Connection: close" and closes its end
   * right after writing the response, so the peer's EOF can show up in a
   * SEPARATE dispatch from the one that already consumed the response bytes
   * and reached CHTTP1_PAUSED.
   *
   * Running the completion logic twice is harmless for a plain fulfil,
   * which chain->fulfilled guards, and for repeating
   * ctls_conn_handshake_step on a handshake that already failed, which just
   * returns another error we ignore.
   *
   * _async_handle_redirect is NOT idempotent though: it queues another hop
   * each time it runs, so without this guard one hop could fan out into
   * several redirect chains sharing the same chain state and wreck that
   * chain's refcount bookkeeping.
   *
   * We check the flag before the TLS_HANDSHAKING branch too, so a handshake
   * failure found by one dispatch keeps a second racing dispatch out of
   * _async_tls_advance.
   *
   * ccol_event_loop's per-registration dispatch_lock already serialises all
   * dispatches of this ctx's single registration, so a plain bool will do
   * and no other lock is needed. */
  if (ctx->hop_completed) {
    /* See the comment of _async_ctx_handle_if_abandoned. This is the MAIN
     * path that reaps a deferred teardown of the second mechanism (see the
     * field comment of pending_app_teardown): the shutdown(fd, SHUT_RDWR)
     * dispatches to exactly this function, or to _async_on_error_impl,
     * before anything reads _async_dispatch_kind below. */
    _async_ctx_handle_if_abandoned(ctx);
    return;
  }
  /* See the field comment of ctx->reg and _async_adopt_reg. */
  _async_adopt_reg(ctx, reg);

  chttp_async_dispatch_kind_t kind = _async_dispatch_kind(ctx);
  if (kind == CHTTP_ASYNC_DISPATCH_ABANDONED) {
    /* A concurrent staleness eviction, or some other change of
     * hop_completed, raced this dispatch between the hop_completed check
     * above and the lock _async_dispatch_kind takes (see that function's
     * comment).
     *
     * That's the narrower race window, besides the check at the top of this
     * function, where the second mechanism can also be reaped.
     * _async_ctx_handle_if_abandoned does nothing if an earlier dispatch
     * already took care of it. */
    _async_ctx_handle_if_abandoned(ctx);
    return;
  }
  if (kind == CHTTP_ASYNC_DISPATCH_IDLE) {
    /* N reactor threads all call epoll_wait on one shared epoll instance
     * without any deduplication like EPOLLEXCLUSIVE, so one readiness event
     * can legitimately produce more than one sequential dispatch for the
     * same fd.
     *
     * Take a response that arrives in two TCP segments. Two reactor threads
     * can each see one segment's readiness in their own epoll_wait before
     * either of them drains the socket. The first dispatch reads and
     * handles the whole response, moves the ctx to IDLE and pools it, and a
     * second dispatch for that same original event comes afterwards.
     *
     * So a bare dispatch here doesn't prove the connection is dead or has
     * really unexpected data on it. A real non-blocking read is what tells a
     * stale readiness that another thread already handled apart from real
     * activity.
     *
     * Without this read a pooled connection gets evicted moments after it
     * was offered, and async_idle_pool.sequential_requests_reuse_connection
     * fails. */
#ifdef RUNNING_UNIT_TESTS
    _chttp_test_idle_probe_hold(ctx->fd);
#endif
    char peek_buf[1];
    ssize_t pn;
    if (ctx->tls) {
      pn = ctls_conn_read(ctx->tls, peek_buf, sizeof(peek_buf));
    } else {
      do {
        pn = recv(ctx->fd, peek_buf, sizeof(peek_buf), 0);
      } while (pn < 0 && errno == EINTR);
    }
    if (pn < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
      /* Nothing there, so this was a stale or duplicate dispatch for an
       * event another thread already handled completely. The connection
       * stays idle and pooled, and there's nothing to do.
       *
       * Unlike the read of an active hop we deliberately don't look at
       * ctls_conn_wants_write, even though ctls_conn_read can return
       * EWOULDBLOCK here while wanting the write direction.
       *
       * An idle pooled registration is only ever registered for read, and
       * _async_on_writable_impl counts on that to know an on_writable
       * dispatch is never for an idle ctx, so flipping to write here would
       * break it.
       *
       * And there's nothing to drive on an idle connection anyway; this
       * probe only exists to tell a stale dispatch from a really dead peer.
       * A connection whose TLS layer is in the middle of a renegotiation
       * just stays pooled until the next request drives it or
       * CHTTP_IDLE_MAX_AGE_MS retires it. */
      return;
    }
    /* The connection really isn't safe to reuse: pn of 0 means the peer
     * closed it, pn above 0 means unexpected data, and so does a hard
     * error. A take for a new hop may have removed the ctx from the pool
     * already, and then the eviction below finds nothing to do; the mark
     * tells that hop what this read saw. */
    ctx->idle_tainted = true;
    _async_idle_ctx_finish(ctx);
    return;
  }

  if (ctx->state == CHTTP_ASYNC_TLS_HANDSHAKING) {
    _async_tls_advance(ctx);
    return;
  }

  if (ctx->state == CHTTP_ASYNC_WRITING) {
    /* A read-direction dispatch while the hop has request bytes to
     * send, so it's the pending write that needs driving, not a read.
     *
     * Two things lead here: ctls_conn_write can ask for the read direction
     * (see the EWOULDBLOCK branch of _async_tls_try_write), or the read
     * dispatch that consumed a "100 Continue" has just moved the ctx back to
     * CHTTP_ASYNC_WRITING while the registration is on read.
     *
     * Reading here would consume response bytes for a request that isn't
     * fully sent yet, and leave the send stalled with nothing to drive
     * it. */
    _async_drive_write(ctx);
    return;
  }

  /* The _read_deadline_passed rule that Tier 1 applies before each read.
   * ctx->timed_out means the sweep has already found the deadline passed,
   * and our own check covers the time until the sweep's next tick.
   * chain->overall_deadline never changes during the chain's life and an
   * active dispatch always has its chain, so reading it needs no lock. */
  if (ctx->timed_out ||
      _read_deadline_passed(&ctx->chain->overall_deadline, ctx->fd)) {
    if (ctx->state == CHTTP_ASYNC_AWAITING_CONTINUE &&
        atomic_exchange(&ctx->continue_decided, true))
      return;
    ctx->timed_out = true;
    ctx->hop_completed = true;
    _async_fulfill(ctx, ccol_timed_out, NULL);
    _async_ctx_finish(ctx);
    return;
  }

  /* A TLS record carries at most 16 KB of application data, so a buffer of
   * that size takes a whole record in one read (the drain below handles
   * input that doesn't fit). */
  char buf[16384];
  char *data = buf;
  char *heap_data = NULL;
  bool drain_oom = false;
  bool drain_timed_out = false;
  ssize_t n = 0;
  bool eof = false;
  bool hard_error = false;
  bool injected = false;

#ifdef RUNNING_UNIT_TESTS
  /* The comment of g_force_async_hard_read_error_for_tests, above the
   * "ASYNC IDLE POOL" section, says why this exists and why it only kicks in
   * once ctx->any_bytes_read is true. */
  if (ctx->any_bytes_read &&
      atomic_exchange(&g_force_async_hard_read_error_for_tests, false)) {
    hard_error = true;
    injected = true;
  }
#endif

  if (!injected) {
    if (ctx->tls) {
#ifdef RUNNING_UNIT_TESTS
      if (_tls_dir_inject_blocks(false)) {
        errno = EWOULDBLOCK;
        n = -1;
      } else
#endif
      {
        size_t first_len = sizeof(buf);
#ifdef RUNNING_UNIT_TESTS
        size_t cap = atomic_load(&g_async_tls_first_read_cap_for_tests);
        if (cap > 0 && cap < first_len) first_len = cap;
#endif
        n = ctls_conn_read(ctx->tls, buf, first_len);
      }
      /* See the field comment of ctx->tls_read_blocked. */
      ctx->tls_read_blocked =
          n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN);
      if (n < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
          /* EWOULDBLOCK from ctls_conn_read() doesn't always mean "wait
           * until readable". OpenSSL may have to WRITE before this call can
           * make progress, e.g. to flush a TLS 1.2 renegotiation or a TLS
           * 1.3 KeyUpdate response (see the doc comment of
           * ctls_conn_wants_write).
           *
           * The reactor is level-triggered, so returning with the
           * registration stuck on the read direction gives the SHARED
           * process-wide reactor thread an event it can't make progress
           * on. Where the peer keeps the socket readable that spins the
           * thread at 100 percent CPU, and everywhere else the hop stalls
           * until its deadline, which with the default request_timeout_us
           * of 0 never comes.
           *
           * The write-direction dispatch we flip to puts the read interest
           * back and then comes back in here through the
           * CHTTP_ASYNC_READING branch of _async_on_writable_impl.
           *
           * We only flip in CHTTP_ASYNC_READING, on purpose. In
           * CHTTP_ASYNC_AWAITING_CONTINUE a write-direction dispatch is the
           * deadline sweep's signal that the "100 Continue" wait is over
           * and the body should go out (see the "ASYNC DEADLINE SWEEP"
           * section), so a flip there couldn't be told apart from that
           * signal and would end the wait early.
           *
           * continue_deadline bounds that wait anyway, so leaving read
           * interest in place costs at most the rest of
           * CHTTP_100_CONTINUE_WAIT_US and never spins, and when the sweep's
           * expiry fires the body goes out, which is exactly the documented
           * timeout outcome. */
          /* A paused send carries on when nothing holds it back, like in
           * the plaintext branch below. Its write also flushes whatever
           * OpenSSL wants to write. */
          if (ctx->state == CHTTP_ASYNC_READING &&
              _async_upload_may_resume(ctx)) {
            _async_upload_resume(ctx);
            return;
          }
          if (ctx->state == CHTTP_ASYNC_READING &&
              _conn_tls_wants_write(ctx->tls)) {
#ifdef RUNNING_UNIT_TESTS
            atomic_fetch_add(&g_tls_dir_write_interest_while_reading, 1u);
            _tls_dir_note_wait(true);
#endif
            if (_async_reg_modify(ctx, ccol_select_write) != ccol_success) {
              _async_ctx_finish(ctx);
            }
          }
          return;
        }
        /* A real TLS or transport error: SSL_ERROR_SYSCALL or SSL_ERROR_SSL,
         * which _ctls_classify_io_result in ctls.c maps to errno
         * ECONNRESET. A graceful close looks different, ctls_conn_read
         * reports a close_notify, and a close without one on a record
         * boundary, as n == 0, just like a plain socket EOF (the `else`
         * branch below). This error must never be folded into `eof` (the
         * hard_error handling a few lines down says why). */
        hard_error = true;
      } else {
        eof = (n == 0);
      }
      /* Drain what the TLS layer already holds. epoll(7) only sees the
       * socket, so input a read leaves inside the TLS layer triggers no
       * further dispatch, and the hop would wait for bytes that are already
       * here until its deadline, or forever with the default
       * request_timeout_us of 0. It must happen here, before processing
       * the bytes, because processing can end the hop and free ctx. Any read
       * that returns something other than data ends the drain; EWOULDBLOCK
       * then means the rest is the start of an incomplete record, and the
       * socket will signal when the remainder comes. */
      if (n > 0) {
        size_t total = (size_t)n;
        size_t cap = sizeof(buf);
        while (ctls_conn_has_pending_input(ctx->tls)) {
          if (total == cap) {
            size_t ncap = cap * 2;
            char *nb =
                (ncap > cap) ? (char *)_ccol_mem_alloc(ctx->mp, ncap) : NULL;
            if (!nb) {
              drain_oom = true;
              break;
            }
            memcpy(nb, data, total);
            _ccol_mem_free(ctx->mp, heap_data);
            heap_data = nb;
            data = nb;
            cap = ncap;
          }
          if (_read_deadline_passed(&ctx->chain->overall_deadline, ctx->fd)) {
            drain_timed_out = true;
            break;
          }
          ssize_t m = ctls_conn_read(ctx->tls, data + total, cap - total);
          if (m <= 0) {
            ctx->tls_read_blocked =
                m < 0 && (errno == EWOULDBLOCK || errno == EAGAIN);
            break;
          }
          total += (size_t)m;
        }
        n = (ssize_t)total;
      }
    } else {
      do {
        n = recv(ctx->fd, buf, sizeof(buf), 0);
      } while (n < 0 && errno == EINTR);
      if (n < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
          /* Nothing more to read. A paused send carries on if nothing holds
           * it back (see _async_upload_may_resume). */
          if (ctx->state == CHTTP_ASYNC_READING &&
              _async_upload_may_resume(ctx))
            _async_upload_resume(ctx);
          return;
        }
        /* Same distinction as in the TLS branch above. recv() returning -1
         * with a real errno like ECONNRESET or ENOTCONN is a hard transport
         * error, never the orderly close from a FIN, which recv() reports
         * as n == 0. */
        hard_error = true;
      } else {
        eof = (n == 0);
      }
    }
  }
  if (n > 0) ctx->any_bytes_read = true;

  if (hard_error) {
    /* This branch never gets to chttp1_parser_finish below, on purpose: a
     * hard error, unlike a real EOF, says nothing about whether the peer
     * sent everything it meant to.
     *
     * chttp1_parser_finish reports a complete, successful EOF-delimited body
     * whenever the parser is already in CHTTP1_ST_BODY_EOF, which is where a
     * response with neither Content-Length nor chunked Transfer-Encoding
     * leaves it, so we'd quietly truncate the response instead of reporting
     * the failure.
     *
     * A connection RESET in the middle of a transfer must never look like a
     * peer that finished normally, and neither may a fatal TLS alert.
     *
     * Tier 1's _chttp_read_message does the same, returning
     * ccol_http_transfer_aborted immediately for every read error but
     * EWOULDBLOCK and never calling chttp1_parser_finish, and so does
     * _async_on_error_impl next to this function for a real EPOLLERR
     * dispatch. The way this function classifies a read result must keep
     * EOF and error just as clearly apart.
     *
     * _async_ctx_finish checks reused and any_bytes_read itself and retries
     * when it can, so we fulfil nothing here (see that function's
     * comment). */
    _async_ctx_finish(ctx);
    return;
  }

  if (eof) {
    /* Same handling of n == 0 / EOF as in Tier 1's
     * _chttp_read_message_with: a clean chttp1_parser_finish with a complete
     * message is valid, since some responses mark their end by closing the
     * connection instead of with Content-Length or chunked framing.
     *
     * A reused connection that gives us this before any response byte is
     * Tier 1's retry-once case, and it's checked BEFORE marking
     * hop_completed or fulfilling the future, because the whole point is
     * that nothing has failed for the caller yet.
     *
     * fe being CHTTP1_PAUSED, and not only CHTTP1_OK, is the EXPECTED result
     * for a valid EOF-delimited body (see the doc comment of
     * chttp1_parser_finish and the same comment in Tier 1's
     * _chttp_read_message_with). */
    chttp1_errno_t fe = chttp1_parser_finish(&ctx->parser);
    bool ok = ((fe == CHTTP1_OK || fe == CHTTP1_PAUSED) &&
               ctx->pctx.message_complete);
    if (!ok) {
      /* _async_ctx_finish checks reused and any_bytes_read itself and
       * retries when it can, so nothing is fulfilled here (see that
       * function's comment). */
      _async_ctx_finish(ctx);
      return;
    }
    if (ctx->state == CHTTP_ASYNC_AWAITING_CONTINUE) {
      /* An EOF-delimited final response (no Content-Length, no chunked
       * framing) arriving directly, with no "100 Continue" first.
       *
       * It's the same "server answered directly, body never sent" outcome
       * as in Tier 1 (see the same comment in _chttp_send_and_read), only
       * with different framing: closing the connection marks the end
       * instead of a length header.
       *
       * Claim the decision first like every other exit of
       * _async_awaiting_continue_on_data, and back off quietly if the
       * deadline sweep's timeout path got it already.
       *
       * The connection can't be reused either way, because this EOF path
       * never pools its connection (see the comment of
       * _async_fulfill_success), so keep_alive needs no extra override. */
      if (atomic_exchange(&ctx->continue_decided, true)) return;
    }
    ctx->hop_completed = true;
    if (ctx->pctx.will_redirect) {
      /* Framing by the peer's close, unlike Content-Length or chunked, never
       * allows keep-alive, the same as Tier 1's _chttp_read_message_with,
       * which forces *keep_alive_out to false for exactly this case. */
      _async_handle_redirect(ctx, false);
    } else {
      _async_fulfill_success(ctx);
      _async_ctx_finish(ctx);
    }
    return;
  }

  if (drain_timed_out) {
    /* The deadline passed between two reads of the drain (see
     * _read_deadline_passed). */
    _ccol_mem_free(ctx->mp, heap_data);
    if (ctx->state == CHTTP_ASYNC_AWAITING_CONTINUE &&
        atomic_exchange(&ctx->continue_decided, true))
      return;
    ctx->timed_out = true;
    ctx->hop_completed = true;
    _async_fulfill(ctx, ccol_timed_out, NULL);
    _async_ctx_finish(ctx);
    return;
  }

  if (drain_oom) {
    /* We couldn't take the input the TLS layer holds, and nothing would
     * signal it again, so the hop can't go on. */
    _ccol_mem_free(ctx->mp, heap_data);
    if (ctx->state == CHTTP_ASYNC_AWAITING_CONTINUE &&
        atomic_exchange(&ctx->continue_decided, true))
      return;
    ctx->hop_completed = true;
    _async_fulfill(ctx, ccol_not_enough_memory, NULL);
    _async_ctx_finish(ctx);
    return;
  }

  /* The processing below can end the hop and wake a caller who then
   * destroys its client, and ctx->mp points into that client, so for a heap
   * buffer we copy the procs by value first. */
  ccol_memmgmt_procs_t data_procs;
  ccol_memmgmt_procs_t *data_mp = NULL;
  if (heap_data && ctx->mp) {
    data_procs = *ctx->mp;
    data_mp = &data_procs;
  }
  if (ctx->state == CHTTP_ASYNC_AWAITING_CONTINUE) {
    _async_awaiting_continue_on_data(ctx, data, (size_t)n);
  } else {
    _async_process_reading_data(ctx, data, (size_t)n);
  }
  _ccol_mem_free(data_mp, heap_data);
}

static void _async_on_readable(ccol_event_loop loop, ccol_event_reg reg,
                               ccol_selectable *sel, void *arg) {
  /* Same as the comment on _async_on_writable above. */
  _async_on_readable_impl(loop, reg, sel, (chttp_async_ctx_t *)arg);
}

static void _async_on_error_impl(ccol_event_loop loop, ccol_event_reg reg,
                                 ccol_selectable *sel, chttp_async_ctx_t *ctx) {
  (void)loop;
  (void)sel;
  if (ctx->hop_completed) {
    /* See the comment of _async_ctx_handle_if_abandoned. Like the one in
     * _async_on_readable_impl, this is a MAIN path that reaps a deferred
     * teardown of the second mechanism (see the field comment of
     * pending_app_teardown): the shutdown(fd, SHUT_RDWR) dispatches to
     * exactly this function or to _async_on_readable_impl, before anything
     * reads _async_dispatch_kind below. */
    _async_ctx_handle_if_abandoned(ctx);
    return;
  }
  /* See the field comment of ctx->reg and _async_adopt_reg; the fd can
   * fail almost immediately after it's registered. */
  _async_adopt_reg(ctx, reg);
  chttp_async_dispatch_kind_t kind = _async_dispatch_kind(ctx);
  if (kind == CHTTP_ASYNC_DISPATCH_ABANDONED) {
    /* As in _async_on_readable_impl, this is the narrower race window where
     * the second mechanism can also be reaped. */
    _async_ctx_handle_if_abandoned(ctx);
    return;
  }
  if (kind == CHTTP_ASYNC_DISPATCH_IDLE) {
    _async_idle_ctx_finish(ctx);
    return;
  }
  /* cthreadcomm.c's write-direction dispatch policy always ranks EPOLLERR
   * or EPOLLHUP above EPOLLOUT (is_error is is_err and is_writable is
   * !is_err), while the read direction has an exception around has_writer.
   *
   * So a failed non-blocking connect(), the normal case for any host off
   * loopback, dispatches HERE, because once the RST or the timeout arrives
   * the kernel reports EPOLLOUT, EPOLLERR and EPOLLHUP together. It never
   * goes to the CHTTP_ASYNC_CONNECTING branch of _async_on_writable_impl,
   * which diagnoses the failure with getsockopt(SO_ERROR).
   *
   * Same for a TLS handshake killed by an RST halfway: it never gets to the
   * CTLS_HANDSHAKE_ERROR branch of _async_tls_advance, which diagnoses with
   * ctls_conn_verify_result.
   *
   * Without the two checks below both cases would fall through to the
   * generic ccol_http_transfer_aborted backstop, while chttpclient.h
   * promises a specific code (ccol_http_connection_failed,
   * ccol_http_tls_handshake_failed or
   * ccol_http_tls_cert_verification_failed) once the request is really in
   * flight.
   *
   * A reused ctx never gets into either state, because a pooled connection
   * comes back in at CHTTP_ASYNC_WRITING and never at CONNECTING or
   * TLS_HANDSHAKING, so fulfilling right here, like the other diagnosis
   * sites do, can never suppress a legitimate retry of a reused
   * connection. */
  /* Both branches below look at ctx->timed_out first (it's _Atomic, so a
   * plain read is race-free).
   *
   * The deadline sweep's shutdown(fd, SHUT_RDWR) on a stuck connect or
   * handshake dispatches here exactly like a real failure on the peer's
   * side. Without this check an expired connect_timeout_us or
   * request_timeout_us would be reported as ccol_http_connection_failed,
   * ccol_http_tls_handshake_failed or ccol_http_tls_cert_verification_failed
   * instead of the ccol_timed_out that chttpclient.h documents. */
  if (ctx->state == CHTTP_ASYNC_CONNECTING) {
    _async_connect_failed(ctx);
    return;
  }
  if (ctx->state == CHTTP_ASYNC_TLS_HANDSHAKING) {
    long vr = ctx->tls ? ctls_conn_verify_result(ctx->tls) : 0;
    _async_fulfill(ctx,
                   ctx->timed_out
                       ? ccol_timed_out
                       : (vr != 0 ? ccol_http_tls_cert_verification_failed
                                  : ccol_http_tls_handshake_failed),
                   NULL);
    _async_ctx_finish(ctx);
    return;
  }
  /* An fd-level error with no more specific diagnosis available, so leave
   * it to _async_ctx_finish, which checks for a retry first and then falls
   * back to its backstop, as the cases above do. Fulfilling here instead
   * would rule out a legitimate retry of a reused connection. */
  _async_ctx_finish(ctx);
}

static void _async_on_error(ccol_event_loop loop, ccol_event_reg reg,
                            ccol_selectable *sel, void *arg) {
  /* Same as the comment on _async_on_writable above. */
  _async_on_error_impl(loop, reg, sel, (chttp_async_ctx_t *)arg);
}

/*
 * ASYNC HAPPY EYEBALLS
 *
 * A TCP target with more than one address connects through a race of
 * connect attempts (chttp_addr_list_t describes the policy, which Tier 1
 * applies in _tcp_connect). In Tier 2 each attempt is a socket with its own
 * registration on the reactor, whose arg is a chttp_connect_attempt_t and
 * not the ctx, and the hop's ctx isn't registered while the race runs. The
 * first attempt to connect hands its fd to the ctx, which registers it
 * through _async_connect_register just like a single connect, and at that
 * moment the race closes all the other attempts. If every candidate fails,
 * the attempt that failed last reports the failure for the hop.
 *
 * The deadline sweep drives the two timed parts of the race through
 * _async_race_tick: starting the next candidate once the attempts in flight
 * have had CHTTP_CONNECT_ATTEMPT_DELAY_MS without a result, and ending
 * every attempt once the connect deadline expires. The sweep wakes up at
 * the planned start of the next candidate (see g_sweep_next_ns), so a
 * candidate starts that delay after the previous one, or immediately after a
 * failure.
 *
 * race->lock guards every field of the race. Lock order is a deadline
 * stripe's mutex, then race->lock, then the reactor's internal locks. No
 * path holds race->lock while taking a deadline stripe or a ctx lock; the
 * attempt that decides the race drops the lock before handing the result
 * to the ctx.
 *
 * The race is reference counted. The ctx holds one reference from the start
 * of the race until the ctx is freed, which is after it has left the
 * deadline registry, so the sweep never reads a race that's gone. Each
 * attempt registration holds one more, released by its on_removed, because
 * a dispatch of a removed registration may be in flight. The race
 * keeps its own copy of the allocator procs, since an on_removed can run
 * after the client that started the race has been destroyed.
 */
typedef struct chttp_connect_race_s chttp_connect_race_t;

typedef struct {
  chttp_connect_race_t *race;
  int fd;             /* -1 when the attempt is not running. */
  ccol_event_reg reg; /* CCOL_EVENT_REG_INVALID when the attempt is not
                       * registered. */
} chttp_connect_attempt_t;

struct chttp_connect_race_s {
  _ccol_mutex_t lock;
  ccol_memmgmt_procs_t procs;
  bool has_procs;
  int refs;
  chttp_async_ctx_t *ctx; /* The hop. NULL once the race is decided. */
  bool decided;
  size_t pending; /* Attempts that run. */
  size_t next;    /* The next candidate of addrs. */
  struct timespec next_start;
  chttp_addr_list_t *addrs;
  chttp_connect_attempt_t attempts[];
};

static void _async_connect_register(chttp_async_ctx_t *ctx, int fd);

static void _async_race_release(chttp_connect_race_t *r) {
  _ccol_mutex_lock(r->lock);
  bool last = (--r->refs == 0);
  _ccol_mutex_unlock(r->lock);
  if (!last) return;
  ccol_memmgmt_procs_t procs = r->procs;
  ccol_memmgmt_procs_t *mp = r->has_procs ? &procs : NULL;
  _ccol_mutex_destroy(r->lock);
  _ccol_mem_free(mp, r->addrs);
  _ccol_mem_free(mp, r);
}

static void _async_race_on_removed(void *arg) {
  _async_race_release(((chttp_connect_attempt_t *)arg)->race);
}

/* The caller holds r->lock. We remove the registration before closing its
 * fd, so the fd number can't end up on another socket while the reactor is
 * watching it. */
static void _async_race_close_attempt_locked(chttp_connect_race_t *r,
                                             chttp_connect_attempt_t *a) {
  if (a->reg) {
    ccol_event_loop_remove(cli_engine_bundler.reactor, a->reg);
    a->reg = CCOL_EVENT_REG_INVALID;
  }
  if (a->fd >= 0) {
    close(a->fd);
    a->fd = -1;
    r->pending--;
  }
}

static void _async_race_on_event(ccol_event_loop loop, ccol_event_reg reg,
                                 ccol_selectable *sel, void *arg);

/* Starts the next candidate that accepts a connect; the caller holds
 * r->lock. A connect that completes immediately is registered like a pending
 * one, since its writable dispatch follows at once and takes the normal
 * winner path. A candidate whose socket, connect or registration fails on
 * the spot makes way for the next one. */
static void _async_race_launch_locked(chttp_connect_race_t *r) {
  while (r->next < r->addrs->count) {
    size_t idx = r->next++;
    bool in_progress = false;
    int fd = _addr_connect_start(&r->addrs->addrs[idx], &in_progress);
    if (fd < 0) continue;
    chttp_connect_attempt_t *a = &r->attempts[idx];
    ccol_event_handlers_t handlers = {
        .on_writable = _async_race_on_event,
        .on_error = _async_race_on_event,
        .on_removed = _async_race_on_removed,
    };
    char *err = NULL;
    ccol_event_reg reg = ccol_event_loop_add(
        cli_engine_bundler.reactor,
        ccol_selectable_from_fd(fd, ccol_select_write), handlers, a, &err);
    if (!reg) {
      close(fd);
      continue;
    }
    /* Any dispatch of this registration waits for r->lock, which we hold, so
     * it always finds these fields set. */
    a->fd = fd;
    a->reg = reg;
    r->refs++;
    r->pending++;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    r->next_start = _timespec_add_ms(now, _connect_attempt_delay_ms());
    if (r->next < r->addrs->count)
      _client_deadline_note_ns(_timespec_to_ns(&r->next_start));
    return;
  }
}

/* Decides the race in favour of `winner`, or of nobody when winner is NULL.
 * The caller holds r->lock, releases it afterwards and then acts on the ctx
 * this returns. */
static chttp_async_ctx_t *_async_race_decide_locked(
    chttp_connect_race_t *r, chttp_connect_attempt_t *winner) {
  for (size_t i = 0; i < r->addrs->count; i++) {
    chttp_connect_attempt_t *a = &r->attempts[i];
    if (a != winner) _async_race_close_attempt_locked(r, a);
  }
  r->decided = true;
  chttp_async_ctx_t *ctx = r->ctx;
  r->ctx = NULL;
  return ctx;
}

static void _async_race_on_event(ccol_event_loop loop, ccol_event_reg reg,
                                 ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  (void)sel;
  chttp_connect_attempt_t *a = (chttp_connect_attempt_t *)arg;
  chttp_connect_race_t *r = a->race;
  _ccol_mutex_lock(r->lock);
  if (r->decided || a->fd < 0) {
    /* A dispatch for an attempt the race has already closed. */
    _ccol_mutex_unlock(r->lock);
    return;
  }
  int soerr = 0;
  socklen_t slen = sizeof(soerr);
  if (getsockopt(a->fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) == 0 &&
      soerr == 0) {
    int fd = a->fd;
    /* The fd goes over to the ctx, so only the attempt's registration goes. */
    ccol_event_loop_remove(cli_engine_bundler.reactor, a->reg);
    a->reg = CCOL_EVENT_REG_INVALID;
    a->fd = -1;
    r->pending--;
    chttp_async_ctx_t *ctx = _async_race_decide_locked(r, a);
    _ccol_mutex_unlock(r->lock);
    _async_connect_register(ctx, fd);
    return;
  }
  _async_race_close_attempt_locked(r, a);
  chttp_async_ctx_t *ctx = r->ctx;
  /* A failure hands over to the next candidate right away, unless the
   * connect deadline has already ended the race. */
  if (!ctx->timed_out) _async_race_launch_locked(r);
  if (r->pending > 0) {
    _ccol_mutex_unlock(r->lock);
    return;
  }
  ctx = _async_race_decide_locked(r, NULL);
  _ccol_mutex_unlock(r->lock);
  _async_fulfill(
      ctx, ctx->timed_out ? ccol_timed_out : ccol_http_connection_failed, NULL);
  _async_ctx_teardown(ctx);
}

/* The deadline sweep calls this for a ctx with a running race, under the
 * mutex of that ctx's deadline stripe. An expired connect deadline shuts
 * every attempt down and each one then fails through its own dispatch; the
 * last one reports ccol_timed_out, since the sweep marked the ctx timed out
 * first. Otherwise the next candidate starts once its delay is over.
 * Starting it never decides the race here, it only registers an attempt, so
 * nothing on this path takes a ctx or stripe lock. */
/* Returns when the next attempt of the race starts, in CLOCK_MONOTONIC
 * nanoseconds, or UINT64_MAX when no attempt is waiting. */
static uint64_t _async_race_tick(chttp_connect_race_t *r, bool expired) {
  uint64_t next_ns = UINT64_MAX;
  _ccol_mutex_lock(r->lock);
  if (!r->decided) {
    /* A ctx an earlier tick marked as timed out starts no more attempts. An
     * attempt started after the deadline isn't shut down by any later
     * expiry and would hold the future until the kernel gives up on its
     * connect, so such a tick shuts down whatever is running instead. */
    if (expired || (r->ctx && r->ctx->timed_out)) {
      for (size_t i = 0; i < r->addrs->count; i++)
        if (r->attempts[i].fd >= 0) shutdown(r->attempts[i].fd, SHUT_RDWR);
    } else if (r->next < r->addrs->count) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (_ms_until(&r->next_start, &now) == 0) {
#ifdef RUNNING_UNIT_TESTS
        _sweep_note_due_for_tests(1, _timespec_to_ns(&r->next_start));
#endif
        _async_race_launch_locked(r);
      }
      if (r->next < r->addrs->count) next_ns = _timespec_to_ns(&r->next_start);
    }
  }
  _ccol_mutex_unlock(r->lock);
  return next_ns;
}

/* Starts the race for ctx over ctx->addrs, which the race takes ownership
 * of. It runs on the connect task's worker, before anything else can reach
 * the race. Returns false if the race couldn't be built, and the caller
 * reports the failure then. */
static bool _async_race_start(chttp_async_ctx_t *ctx) {
  size_t n = ctx->addrs->count;
  chttp_connect_race_t *r = (chttp_connect_race_t *)_ccol_mem_calloc(
      ctx->mp, 1, sizeof(*r) + n * sizeof(r->attempts[0]));
  if (!r) return false;
  if (_ccol_mutex_init(r->lock) != 0) {
    _ccol_mem_free(ctx->mp, r);
    return false;
  }
  if (ctx->mp) {
    r->procs = *ctx->mp;
    r->has_procs = true;
  }
  r->refs = 1; /* The ctx's reference. */
  r->ctx = ctx;
  r->addrs = ctx->addrs;
  ctx->addrs = NULL;
  for (size_t i = 0; i < n; i++) {
    r->attempts[i].race = r;
    r->attempts[i].fd = -1;
  }
  ctx->state = CHTTP_ASYNC_CONNECTING;
  atomic_store(&ctx->race, r);

  _ccol_mutex_lock(r->lock);
  _async_race_launch_locked(r);
  bool failed = (r->pending == 0);
  if (failed) (void)_async_race_decide_locked(r, NULL);
  _ccol_mutex_unlock(r->lock);
  if (failed) {
    _async_fulfill(ctx, ccol_http_connection_failed, NULL);
    _async_ctx_teardown(ctx);
  }
  return true;
}

/* Lets go of ctx's race when ctx is freed. On every path the race is decided
 * by then, and the check here just keeps it that way, so an attempt never
 * outlives the ctx it connects for. */
static void _async_race_detach(chttp_async_ctx_t *ctx) {
  chttp_connect_race_t *r = atomic_load(&ctx->race);
  if (!r) return;
  atomic_store(&ctx->race, NULL);
  _ccol_mutex_lock(r->lock);
  if (!r->decided) (void)_async_race_decide_locked(r, NULL);
  _ccol_mutex_unlock(r->lock);
  _async_race_release(r);
}

/*
 * Runs on a cli_engine_bundler.dns_pool worker for a named target, and on
 * the thread that starts the hop for an address literal or a unix target
 * (see _async_connect_begin). For TCP it resolves the target, for a unix
 * target it builds the sockaddr_un. Then it issues ONE non-blocking
 * connect() and finally registers the fd with the reactor for write
 * readiness, so it never blocks waiting for the connect to complete.
 *
 * That split (a synchronous DNS lookup plus one non-blocking connect on an
 * offload thread) is on purpose. A worker that blocks through the whole
 * connect ties up a fixed-size pool for as long as the connect takes, which
 * hurts throughput under high concurrency, while registering for write
 * readiness lets one epoll instance track thousands of pending connects
 * cheaply.
 *
 * For a TCP target with one address we start the connect on that address
 * and register its fd. With more than one address we start the Happy
 * Eyeballs race of the "ASYNC HAPPY EYEBALLS" section instead, which
 * registers one fd per attempt and hands the winner to the ctx, following
 * the chttp_addr_list_t policy that Tier 1 shares. Apart from the name
 * lookup this never blocks the thread it runs on.
 */
static void _async_connect_task(void *arg) {
  chttp_async_ctx_t *ctx = (chttp_async_ctx_t *)arg;
  int fd = -1;

  if (ctx->is_unix) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t path_len = strlen(ctx->unix_socket_path);
    if (path_len >= sizeof(addr.sun_path)) {
      _async_fulfill(ctx, ccol_http_invalid_url, NULL);
      _async_ctx_teardown(ctx);
      return;
    }
    memcpy(addr.sun_path, ctx->unix_socket_path, path_len + 1);
    fd = ccol_socket_nb(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      _async_fulfill(ctx, ccol_http_connection_failed, NULL);
      _async_ctx_teardown(ctx);
      return;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 &&
        errno != EINPROGRESS) {
      close(fd);
      _async_fulfill(ctx, ccol_http_connection_failed, NULL);
      _async_ctx_teardown(ctx);
      return;
    }
  } else {
    /* If the deadline expired while this task was queued in the DNS pool,
     * or while its lookup ran, the hop ends here before the lookup and
     * before any connect. The sweep has already answered its future with
     * ccol_timed_out, and a lookup or a SYN for it would only tie up a pool
     * thread and a peer. */
    if (ctx->timed_out) {
      _async_fulfill(ctx, ccol_timed_out, NULL);
      _async_ctx_teardown(ctx);
      return;
    }
    ccol_retval_t rrv = _addr_list_resolve_for_target(ctx->mp, ctx->host,
                                                      ctx->port, &ctx->addrs);
    if (rrv != ccol_success) {
      _async_fulfill(ctx, rrv, NULL);
      _async_ctx_teardown(ctx);
      return;
    }
    if (ctx->timed_out) {
      _async_fulfill(ctx, ccol_timed_out, NULL);
      _async_ctx_teardown(ctx);
      return;
    }
    _addr_list_interleave(ctx->addrs);
    if (ctx->addrs->count > 1) {
      /* See the "ASYNC HAPPY EYEBALLS" section. */
      if (!_async_race_start(ctx)) {
        _async_fulfill(ctx, ccol_not_enough_memory, NULL);
        _async_ctx_teardown(ctx);
      }
      return;
    }
    bool in_progress = false;
    fd = _addr_connect_start(&ctx->addrs->addrs[0], &in_progress);
    if (fd < 0) {
      _async_fulfill(ctx, ccol_http_connection_failed, NULL);
      _async_ctx_teardown(ctx);
      return;
    }
    /* Like Tier 1's _tcp_connect, which stops on a pending attempt once the
     * connect deadline has passed. */
    int remaining_ms;
    if (in_progress &&
        !_deadline_remaining_ms(&ctx->connect_deadline, &remaining_ms))
      ctx->timed_out = true;
  }
  _async_connect_register(ctx, fd);
}

/* Starts the connect of a fresh hop. A target that needs a name lookup goes
 * to cli_engine_bundler.dns_pool, since getaddrinfo() blocks. An address
 * literal or a unix socket needs no lookup, so its connect task runs right
 * here, doing one non-blocking connect() and a registration without ever
 * waiting. That way blocking lookups (say during a resolver outage) can
 * never hold up a request to an address literal, or the connect of any
 * request that already has its address.
 *
 * The caller has registered ctx with the deadline sweep and must not touch
 * it after a success, because the task can finish the hop and free ctx
 * before we return. On failure nothing ran and freeing ctx is up to the
 * caller. */
static ccol_retval_t _async_connect_begin(chttp_async_ctx_t *ctx) {
  if (ctx->is_unix || _host_is_ip_literal(ctx->host)) {
    _async_connect_task(ctx);
    return ccol_success;
  }
  return ctpool_submit(cli_engine_bundler.dns_pool, _async_connect_task, ctx,
                       NULL);
}

/* Hands the socket fd (connect pending or complete) to ctx and registers it
 * with the reactor for write readiness. The connect task calls it for a
 * single connect, and the winner of a Happy Eyeballs race calls it from its
 * own dispatch. */
static void _async_connect_register(chttp_async_ctx_t *ctx, int fd) {
  /* ctx->fd and ctx->state are _Atomic (see their field comments), so these
   * plain assignments don't race the deadline sweep. The sweep is a
   * separate thread that has had this ctx registered since before it was
   * submitted here (see _async_submit_hop), and it can run at any moment,
   * this very assignment included.
   *
   * It also covers a connect_timeout_us that ran out while the task was
   * just sitting in the cli_engine_bundler.dns_pool queue, which a
   * saturated pool causes. The sweep can't shut down an fd that doesn't
   * exist yet, so on that path all it can do is mark ctx->timed_out and
   * wait, and the check below sees that mark as soon as a real fd exists. */
  ctx->fd = fd;
  ctx->state = CHTTP_ASYNC_CONNECTING;

  if (ctx->timed_out) {
    /* The fd isn't registered with the reactor, so nothing else can act on
     * it yet. Report the timeout here and let _async_ctx_teardown close the
     * fd through _async_ctx_destroy_now.
     *
     * Closing it by hand here, before _client_deadline_unregister runs,
     * would break this module's "unregister before close" invariant (see
     * the doc comment of _async_ctx_destroy_now), because the deadline
     * registry already holds this ctx: it was registered before this task
     * was submitted.
     *
     * _async_ctx_destroy_now always closes ctx->fd itself AFTER the
     * unregister, which is the only order that keeps the invariant. */
    _async_fulfill(ctx, ccol_timed_out, NULL);
    _async_ctx_teardown(ctx);
    return;
  }

  char *err = NULL;
  ccol_event_handlers_t handlers = {
      .on_readable = _async_on_readable,
      .on_writable = _async_on_writable,
      .on_error = _async_on_error,
  };
  /* We hold idle_lock across ccol_event_loop_add and across reading
   * ctx->reg back right after it.
   *
   * As soon as ccol_event_loop_add makes the registration live, a reactor
   * thread may dispatch it, and a loopback connect is very often writable
   * immediately. That thread can then run the whole hop to completion and
   * reach _async_ctx_free while our `ctx->reg = ...` write and the
   * `if (!ctx->reg)` read after it are in flight. That's a real
   * write/read race on ctx->reg, which ThreadSanitizer reports even though
   * reading the code doesn't show it.
   *
   * _async_ctx_free briefly takes the same lock as its very first action,
   * and that's enough to make sure this window is over before the free goes
   * on: ctx->reg is published by then and we've stopped touching ctx.
   *
   * No dispatch path needs idle_lock to read ctx->reg, because every
   * dispatch first adopts the handle that ccol_event_loop hands it (see
   * _async_adopt_reg). A dispatch that gets to a path taking idle_lock only
   * waits here until ccol_event_loop_add returns, and ccol_event_loop_add
   * never waits for a dispatch. */
  _ccol_mutex_lock(ctx->idle_lock);
  ctx->reg = ccol_event_loop_add(cli_engine_bundler.reactor,
                                 ccol_selectable_from_fd(fd, ccol_select_write),
                                 handlers, ctx, &err);
  bool reg_failed = !ctx->reg;
  _ccol_mutex_unlock(ctx->idle_lock);
  if (reg_failed) {
    _async_fulfill(ctx, ccol_not_enough_memory, NULL);
    _async_ctx_teardown(ctx);
  }
}

/*
 * Called when a REUSED connection turns out to be dead before any response
 * byte was read: a write failure, an immediate EOF, or an error the reactor
 * found. Same recovery as Tier 1 in chttp_do_internal, one retry on a
 * brand-new connection after the liveness probe failed.
 *
 * We allocate a fresh ctx rather than reusing old_ctx's struct for the new
 * attempt. Reusing it would mean neutralising old_ctx's dispatch callbacks,
 * because a dispatch already in flight for the OLD registration would
 * otherwise show up and read fields that now serve the NEW attempt, and
 * that neutralising is itself unsafe if some OTHER independent teardown was
 * in flight at the same moment. One more allocation on a path this rare is
 * a small price for that.
 *
 * So we move out of old_ctx only what the retry needs: the serialized wire
 * bytes, host and port, origin_key, the empty response header map and the
 * hop's metadata. Those fields are set to NULL in old_ctx, so its ordinary
 * teardown (which the caller runs exactly as for any other failure) won't
 * free them a second time.
 *
 * Apart from that old_ctx is left alone and goes down its NORMAL teardown
 * path, and we neither free it nor touch its chain reference. We only set
 * ctx->hop_completed, which keeps old_ctx's on_data and on_close from
 * triggering this again, and which is also true from old_ctx's point of
 * view since it really did reach a terminal outcome.
 *
 * The new ctx gets a SECOND chain reference, while old_ctx keeps its own
 * until its teardown drops it. The retain comes before old_ctx can release
 * anything, so the chain's refcount never hits zero too early during the
 * handoff, the same retain-before-release order a redirect hop uses.
 */
static void _async_retry_hop(chttp_async_ctx_t *old_ctx) {
  old_ctx->hop_completed = true;
  chttp_async_chain_t *chain = old_ctx->chain;

  _async_chain_retain(chain);

  chttp_async_ctx_t *ctx = _async_ctx_create(chain->mp);
  if (!ctx) {
    _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
    _async_chain_release(chain);
    return;
  }

  /* ctx is brand new and private here. Neither the deadline registry nor
   * ccol_event_loop has it yet, so no concurrent reader can race this
   * write, but we take deadline_lock anyway so every write site looks the
   * same (see that lock's field comment). */
  _ccol_mutex_lock(ctx->deadline_lock);
  ctx->overall_deadline = chain->overall_deadline;
  _ccol_mutex_unlock(ctx->deadline_lock);
  ctx->chain = chain;
  ctx->cli = old_ctx->cli;
  ctx->hop = old_ctx->hop;
  ctx->cur_method = old_ctx->cur_method;
  ctx->is_https = old_ctx->is_https;
  ctx->verify_host = old_ctx->verify_host;
  /* The retry makes a fresh connection (ctx->reused is false below), so its
   * handshake runs under chain->tls_ctx, the configuration this chain pinned
   * when it was created. That's why it carries the chain's generation and
   * not the one the dead connection had.
   *
   * Copying old_ctx's value could label a connection with the current
   * configuration although it was set up under an older one, and the
   * eligibility check in _async_idle_pool_take would then give it to a
   * later request that thinks it runs on the new policy. */
  ctx->tls_generation = chain->tls_generation;

  ctx->is_unix = old_ctx->is_unix;
  ctx->unix_socket_path = old_ctx->unix_socket_path;
  old_ctx->unix_socket_path = NULL;
  ctx->host = old_ctx->host;
  old_ctx->host = NULL;
  ctx->port = old_ctx->port;
  ctx->is_ipv6 = old_ctx->is_ipv6;
  ctx->path_and_query = old_ctx->path_and_query;
  old_ctx->path_and_query = NULL;
  ctx->wire = old_ctx->wire;
  old_ctx->wire = NULL;
  ctx->wire_len = old_ctx->wire_len;
  ctx->origin_key = old_ctx->origin_key;
  old_ctx->origin_key = NULL;
  /* The retry sends exactly the same request again, so it keeps the
   * want_100_continue and header_len decisions old_ctx already made, the
   * way the redirect code in this file carries cur_method and cur_body
   * forward.
   *
   * We only get here through the retry-eligibility check of
   * _async_ctx_finish: old_ctx never sent the body after a confirmed
   * "100 Continue" (retry_unsafe is false), and either it didn't send the
   * whole request or the request is replayable (see _request_is_replayable).
   * So old_ctx may have sent part of the wire buffer or all of it. Only a
   * reused ctx is ever retried, and a reused ctx keeps its wire buffer after
   * a finished write (see _async_tls_try_write), so ctx->wire holds the
   * whole request and the new attempt, with wire_sent starting at 0, sends
   * all of it again.
   *
   * continue_decided starts out false again for this attempt, just like a
   * brand-new hop. */
  ctx->want_100_continue = old_ctx->want_100_continue;
  ctx->header_len = old_ctx->header_len;
  ctx->replayable = old_ctx->replayable;
  ctx->request_close = old_ctx->request_close;
  atomic_store(&ctx->continue_decided, false);
  atomic_store(&ctx->continue_msg_started, false);

  ctx->pctx.mp = chain->mp;
  ctx->pctx.is_head_request = old_ctx->pctx.is_head_request;
  /* A streaming request sets chain->write_fn, which hands body bytes
   * straight to the caller's callback, and a buffered request uses ctx->bb.
   * The comment of _async_build_response explains why this choice must
   * match what that function does later. */
  ctx->pctx.requested_sink_fn =
      chain->write_fn ? _sink_async_user : _sink_buffered;
  ctx->pctx.requested_sink_ctx = chain->write_fn ? (void *)chain : &ctx->bb;
  ctx->pctx.headers = old_ctx->pctx.headers; /* The map is empty, since a
                                              * retry needs !any_bytes_read
                                              * and so nothing was ever
                                              * parsed into it. */
  old_ctx->pctx.headers = NULL;
  ctx->bb.mp = chain->mp;
  ctx->bb.max_size = chain->max_response_body_size;

  ctx->reused = false; /* The retry runs on a fresh connection. */
  ctx->any_bytes_read = false;
  /* Since the retry connects fresh, it needs a connect_deadline of its own,
   * just like the fresh path of _async_submit_hop. The chain's
   * overall_deadline stays as it is, because it was never per hop. */
  ctx->connect_deadline = _deadline_make(chain->connect_timeout_us);

  _async_parser_start(ctx);

  /* Register the ctx BEFORE submitting the task (the same comment in
   * _async_submit_hop says why). In the other order a worker can run the
   * hop to completion and free ctx before this thread has registered it. */
  _client_deadline_register(ctx);
  _client_deadline_note(&ctx->connect_deadline);
  _client_deadline_note(&chain->overall_deadline);

  ccol_retval_t sr = _async_connect_begin(ctx);
  if (sr != ccol_success) {
    _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
    _async_ctx_free(ctx); /* Also unregisters ctx. */
    _async_chain_release(chain);
  }
}

/*
 * Prepares one hop of a redirect chain and submits it. It's used for the
 * very first hop (from _chttp_do_async_internal) and for every redirect hop
 * after it (from _async_handle_redirect).
 *
 * On success it holds one chain reference, dropped when this hop's ctx is
 * torn down. On failure it drops that reference again itself, so the
 * refcount ends up unchanged, and it fulfils the chain's future with a
 * specific error code before returning, so callers don't need their own
 * fallback that fulfils on failure.
 *
 * The generic backstop in _async_chain_release is only a last resort for
 * paths no more specific error can reach, like a connection that dies while
 * already in flight.
 *
 * body_data, body_len and body_content_type are the body of THIS hop.
 * Usually they are chain->body_data, chain->body_len and
 * chain->body_content_type as they are, and all empty after a redirect that
 * doesn't preserve the method has rewritten it to GET.
 *
 * The caller passes them explicitly instead of us always reading them from
 * the chain, because the caller (the counterpart of the Tier 1 loop) is the
 * one who knows from the previous hop's status code whether to keep them or
 * drop them.
 *
 * Returns true when the hop was queued.
 */

/*
 * Handles a setup failure that happens AFTER we've already popped a reused
 * connection from the idle pool: a failed allocation of the headers map, a
 * failed serialization of the request, or a failed allocation of the
 * origin_key.
 *
 * At that point ctx->fd is a live, registered connection (not a fresh ctx
 * that never connected), and ctx->idle_lock is held, as the return
 * contract of _async_idle_pool_take says.
 *
 * We report the error and mark the ctx terminal, which it is anyway. The
 * mark also keeps the retry check of _async_ctx_finish from queuing a
 * pointless retry, since this is a failed allocation and not a dead
 * connection.
 *
 * This runs on an ordinary application thread (whoever called
 * chttpclient_do_async, or a caller driven by a redirect) while the ctx's
 * ccol_event_loop registration is fully live, so a reactor dispatch for it
 * can legitimately be in flight, or about to run, on another thread right
 * now.
 *
 * Calling _async_ctx_teardown directly here would be a use-after-free (the
 * field comment of pending_app_teardown has the whole reasoning), and this
 * function is one of the two places that comment describes.
 *
 * Instead we set pending_app_teardown and hop_completed, in that order (as
 * the note in that field comment says, the order doesn't matter here but
 * we keep it consistent). Then we shutdown() the fd, which forces a real
 * EPOLLIN or EPOLLERR dispatch on it since it's registered for read,
 * and only after that unlock idle_lock. Whichever dispatch sees the mark
 * runs the real destructive teardown safely from a dispatch context (see
 * _async_ctx_handle_if_abandoned).
 *
 * The shutdown comes before the unlock. Once idle_lock is free the
 * parked dispatch runs the teardown and close()s ctx->fd, and a shutdown
 * after that would name an fd number the kernel may already have given to
 * an unrelated socket in this process. Nothing may touch ctx after
 * idle_lock is unlocked.
 *
 * A fresh ctx that never connected is another story: nobody ever locked its
 * idle_lock, it isn't attached to anything and ctx->reg is NULL, so
 * no dispatch can be in flight for it. We just free it directly, like every
 * other failure path before a connect.
 */
static void _async_submit_hop_fail(chttp_async_ctx_t *ctx, ccol_retval_t rv) {
  chttp_async_chain_t *chain = ctx->chain;
  _async_fulfill_chain(chain, rv, NULL);
  if (ctx->reused) {
    ctx->pending_app_teardown = true;
    ctx->hop_completed = true;
    /* This runs with idle_lock held on purpose. A reactor dispatch for this
     * fd, which is registered, may already be parked in
     * _async_dispatch_kind waiting for exactly this lock, and the moment we
     * unlock it that dispatch sees hop_completed and runs the real teardown,
     * which close()s ctx->fd.
     *
     * A shutdown after that point would act on a bare int, a number that
     * the kernel may have given in the meantime to a completely unrelated
     * socket opened by any thread of the process.
     *
     * Holding the lock across the shutdown costs nothing and has no
     * reentrancy risk, because shutdown() is a plain kernel operation with
     * no synchronous application callback. The deadline sweep relies on the
     * same thing when it calls shutdown under a stripe mutex. */
#ifdef RUNNING_UNIT_TESTS
    atomic_store(&g_abandon_shutdown_under_lock_for_tests,
                 ctx->idle_lock_held_for_tests ? 1 : 0);
#endif
    if (ctx->fd >= 0) shutdown(ctx->fd, SHUT_RDWR);
#ifdef RUNNING_UNIT_TESTS
    ctx->idle_lock_held_for_tests = false;
#endif
    _ccol_mutex_unlock(ctx->idle_lock);
    /* _client_engine_release() below drops the SEPARATE engine reference
     * that _async_idle_pool_offer took for this ctx while it was in the idle
     * pool. _async_ctx_teardown doesn't know about that reference, it only
     * drops the ctx's chain reference once the deferred dispatch runs it.
     *
     * This is the fourth way out of the idle pool that has to drop the
     * reference. The other three are a successful reuse (below in
     * _async_submit_hop), an idle connection dying naturally (in
     * _async_idle_ctx_finish) and a staleness eviction (in
     * _async_idle_pool_take).
     *
     * Without it, every setup failure on a reused connection (a failed
     * allocation or serialization after a pooled connection was popped)
     * leaks one engine reference.
     *
     * It's safe to call straight from this thread, since it only touches a
     * global counter and never ctx. */
    _client_engine_release();
  } else {
    _async_ctx_free(ctx);
    _async_chain_release(chain);
  }
}

static bool _async_submit_hop(chttp_async_chain_t *chain, const char *url_str,
                              chttp_method_t method, const void *body_data,
                              size_t body_len, const char *body_content_type,
                              int hop) {
  _async_chain_retain(chain);

  chttp_url_t url;
  ccol_retval_t prv = _parse_chttp_url(chain->mp, url_str, &url);
  if (prv != ccol_success) {
    _async_fulfill_chain(chain, prv, NULL);
    _async_chain_release(chain);
    return false;
  }
  if (url.is_https && !chain->tls_ctx_usable) {
    /* Same per-hop check as in Tier 1: the configured cert, key or CA path
     * couldn't be read at the set_tls call, or it failed to load or parse
     * there. The same check in chttp_do_internal explains why
     * ccol_http_tls_cert_load_failed is the right code. */
    _url_free(chain->mp, &url);
    _async_fulfill_chain(chain, ccol_http_tls_cert_load_failed, NULL);
    _async_chain_release(chain);
    return false;
  }

  /* The credential headers and the Host header that the caller set with
   * chttp_request_set_header are left off every hop whose origin or host
   * differs from chain->initial_origin_key, the origin of hop 0 (see
   * CHTTP_REDIRECT_DROP_CREDENTIALS). It works exactly like the
   * initial_origin_key and redirect_drops locals of chttp_do_internal in
   * Tier 1.
   *
   * Changing chain->redirect_drops here needs no lock, for the same reason
   * as the chain->carried_auth and chain->carried_auth_origin changes below
   * (see the next comment in this function). */
  chain->redirect_drops =
      _redirect_header_drops(chain->initial_origin_key, url.origin_key);

  /* Carries forward an Authorization value we injected from a URL's
   * userinfo, exactly like the carried_auth and carried_auth_origin locals
   * of chttp_do_internal in Tier 1: kept on the same origin, dropped for
   * good once the origin changes.
   *
   * chain->carried_auth and chain->carried_auth_origin can be changed here
   * without a lock, because _async_submit_hop runs strictly one hop at a
   * time per chain, and one hop's setup (this change included) always
   * finishes before anything submits the next hop. Every other unguarded
   * change to a chain field in this file's redirect code, such as the
   * method and body downgrade decision in _async_handle_redirect, relies on
   * the same invariant. */
  const char *effective_auth = NULL;
  if (url.userinfo_authorization) {
    effective_auth = url.userinfo_authorization;
  } else if (chain->carried_auth &&
             _origin_keys_same(chain->carried_auth_origin, url.origin_key)) {
    effective_auth = chain->carried_auth;
  }
  if (url.userinfo_authorization) {
    char *na = ccol_strdup(chain->mp, url.userinfo_authorization);
    char *no = ccol_strdup(chain->mp, url.origin_key);
    if (!na || !no) {
      _ccol_mem_free(chain->mp, na);
      _ccol_mem_free(chain->mp, no);
      _url_free(chain->mp, &url);
      _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
      _async_chain_release(chain);
      return false;
    }
    _ccol_mem_free(chain->mp, chain->carried_auth);
    _ccol_mem_free(chain->mp, chain->carried_auth_origin);
    chain->carried_auth = na;
    chain->carried_auth_origin = no;
  } else if (chain->carried_auth &&
             !_origin_keys_same(chain->carried_auth_origin, url.origin_key)) {
    _ccol_mem_free(chain->mp, chain->carried_auth);
    _ccol_mem_free(chain->mp, chain->carried_auth_origin);
    chain->carried_auth = NULL;
    chain->carried_auth_origin = NULL;
  }

  chttp_async_ctx_t *ctx = NULL;
  bool reused = _async_idle_pool_take(chain->cli, url.origin_key, chain, &ctx);
#ifdef RUNNING_UNIT_TESTS
  if (reused && atomic_exchange(&g_stray_write_at_take_for_tests, false)) {
    (void)ccol_event_loop_modify(cli_engine_bundler.reactor, ctx->reg,
                                 ccol_select_write);
    struct timespec pause_ts = {0, 200000000L};
    nanosleep(&pause_ts, NULL);
  }
#endif
  if (!reused) {
    ctx = _async_ctx_create(chain->mp);
    if (!ctx) {
      _url_free(chain->mp, &url);
      _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
      _async_chain_release(chain);
      return false;
    }
    ctx->cli = chain->cli;
    /* Only a fresh connection goes through CHTTP_ASYNC_CONNECTING and
     * CHTTP_ASYNC_TLS_HANDSHAKING. A reused one jumps straight to WRITING
     * below, so nobody ever reads its connect_deadline (see the "ASYNC
     * DEADLINE SWEEP" section). */
    ctx->connect_deadline = _deadline_make(chain->connect_timeout_us);
  }
  /* For a reused connection, _async_idle_pool_take has already set
   * ctx->chain, ctx->overall_deadline and ctx->state (to
   * CHTTP_ASYNC_WRITING), all under ctx->idle_lock, and it returned with
   * that lock HELD (its doc comment says why).
   *
   * We keep the lock for everything below and only unlock it once the write
   * attempt at the bottom has really happened, successful or not, because
   * no concurrent dispatch may touch ctx before that.
   *
   * In the reused case the assignment below is a harmless no-op, since the
   * fields already hold these values, and in the fresh case it's the only
   * assignment. Both cases go through deadline_lock (see that lock's field
   * comment), because for a reused ctx the deadline sweep may already hold
   * it from an earlier fresh connect. */
  _ccol_mutex_lock(ctx->deadline_lock);
  ctx->overall_deadline = chain->overall_deadline;
  _ccol_mutex_unlock(ctx->deadline_lock);
  ctx->chain = chain;
  ctx->hop = hop;
  ctx->cur_method = method;
  ctx->is_https = url.is_https;
  ctx->is_ipv6 = url.is_ipv6;
  ctx->verify_host = chain->verify_host;
  /* A reused ctx keeps the generation its own handshake ran under. Only a
   * fresh connection is about to do a handshake under the chain's pinned
   * configuration, and overwriting it on the reused path would wipe out
   * exactly what _async_idle_pool_take just checked. */
  if (!reused) ctx->tls_generation = chain->tls_generation;
  ctx->reused = reused;
  ctx->reuse_checked = false;
  ctx->any_bytes_read = false;
  ctx->hop_completed = false;
  ctx->interim_responses_seen = 0;

  /* Captured again on EVERY hop, reused connections included, because the
   * request path can differ from the previous hop on this same pooled
   * connection even when the origin is the same. host, port and origin_key
   * don't have that problem.
   *
   * _async_handle_redirect needs it to resolve a relative Location against
   * the URL of THIS hop, like the per-hop chttp_url_t url local of Tier 1
   * (the struct comment of this field describes the crash it prevents).
   *
   * We free the old value first, since a reused ctx may carry its
   * previous owner's copy: the idle pool offer clears it when it can, but
   * correctness doesn't depend on that. */
  _ccol_mem_free(chain->mp, ctx->path_and_query);
  ctx->path_and_query = ccol_strdup(chain->mp, url.path_and_query);
  if (!ctx->path_and_query) {
    _url_free(chain->mp, &url);
    _async_submit_hop_fail(ctx, ccol_not_enough_memory);
    return false;
  }

  ctx->pctx.mp = chain->mp;
  ctx->pctx.is_head_request = (method == CHTTP_HEAD);
  /* A streaming request sets chain->write_fn, which hands body bytes
   * straight to the caller's callback, and a buffered request uses ctx->bb.
   * The comment of _async_build_response explains why this choice must
   * match what that function does later. */
  ctx->pctx.requested_sink_fn =
      chain->write_fn ? _sink_async_user : _sink_buffered;
  ctx->pctx.requested_sink_ctx = chain->write_fn ? (void *)chain : &ctx->bb;
  ctx->bb.mp = chain->mp;
  ctx->bb.max_size = chain->max_response_body_size;

  char *herr = NULL;
  ctx->pctx.headers =
      chmap_create_full(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                        ccol_string, chain->mp, NULL, NULL, &herr);
  if (!ctx->pctx.headers) {
    _url_free(chain->mp, &url);
    _async_submit_hop_fail(ctx, ccol_not_enough_memory);
    return false;
  }

  chttp_request_t hop_req;
  memset(&hop_req, 0, sizeof(hop_req));
  hop_req.method = method;
  hop_req.headers = chain->req_headers;
  hop_req.body.data = body_data;
  hop_req.body.len = body_len;
  hop_req.body.content_type = body_content_type;
  /* Without this line _serialize_request never puts
   * "expect: 100-continue" on the wire, because it only looks at
   * req->expect_continue and nothing on the chain.
   *
   * ctx->want_100_continue below is taken from chain->expect_continue
   * directly, so the split-write-and-wait machinery would kick in anyway:
   * we'd really wait, and the header would never reach the peer.
   *
   * async_step_a.expect_continue_field_reaches_the_wire catches it: without
   * this line it fails, counting zero such headers on the wire. */
  hop_req.expect_continue = chain->expect_continue;

  chttp_header_presence_t hop_hp;
  prv = _serialize_request(chain->mp, &hop_req, &url, effective_auth,
                           chain->redirect_drops, &ctx->wire, &ctx->wire_len,
                           &hop_hp);
  if (prv != ccol_success) {
    _url_free(chain->mp, &url);
    _async_submit_hop_fail(ctx, prv);
    return false;
  }
  /* Exactly the same as the use_100_continue local of chttp_do_internal in
   * Tier 1. That function's comment explains why has_explicit_expect has to
   * come from the case-insensitive scan in _serialize_request and not from
   * a second naive lookup.
   *
   * We compute body_carrying_method again here rather than pass it along
   * from hop_req, because chttp_method_t has no public accessor for it
   * outside what _serialize_request does internally. */
  bool body_carrying_method_ac =
      (method == CHTTP_POST || method == CHTTP_PUT || method == CHTTP_PATCH);
  ctx->want_100_continue = chain->expect_continue && !hop_hp.has_expect &&
                           body_carrying_method_ac && body_data && body_len > 0;
  ctx->replayable = _request_is_replayable(method, &hop_hp);
  ctx->request_close = hop_hp.request_close;
  ctx->header_len = ctx->want_100_continue ? ctx->wire_len - body_len : 0;
  atomic_store(&ctx->continue_decided, false);
  atomic_store(&ctx->continue_msg_started, false);

  if (!ctx->origin_key) {
    /* Always set, except on the reused path where the field already holds
     * the origin this connection was pooled under. That value equals
     * url.origin_key by construction, because _async_idle_pool_take only
     * returns a connection filed under the exact origin_key it looked up. */
    ctx->origin_key = ccol_strdup(chain->mp, url.origin_key);
    if (!ctx->origin_key) {
      _url_free(chain->mp, &url);
      _async_submit_hop_fail(ctx, ccol_not_enough_memory);
      return false;
    }
  }

  if (!reused) {
    ctx->is_unix = url.is_unix;
    if (url.is_unix) {
      ctx->unix_socket_path = ccol_strdup(chain->mp, url.unix_socket_path);
      if (!ctx->unix_socket_path) {
        _url_free(chain->mp, &url);
        _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
        _async_ctx_teardown(ctx);
        return false;
      }
    } else {
      ctx->host = ccol_strdup(chain->mp, url.host);
      ctx->port = url.port;
      if (!ctx->host) {
        _url_free(chain->mp, &url);
        _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
        _async_ctx_teardown(ctx);
        return false;
      }
    }
  }
  _url_free(chain->mp, &url);

  _async_parser_start(ctx);

  if (reused) {
    /* The connection is already connected, and for HTTPS the handshake is
     * done too, so CONNECTING and TLS_HANDSHAKING are skipped completely.
     * ctx->state was moved to CHTTP_ASYNC_WRITING above, under idle_lock
     * and together with ctx->chain, before any other per-hop field below.
     *
     * The registration needs to flip from read (its steady state in the idle
     * pool) to write. We do NOT attempt the write ourselves on this calling
     * thread, which isn't a reactor thread, and leave it all to the
     * _async_on_writable dispatch, the same way the first write of a fresh
     * connection works.
     *
     * That's simpler, but mostly correctness needs it. Once
     * ccol_event_loop_modify flips the registration to write interest, an
     * already running reactor thread may dispatch _async_on_writable for it
     * at the same time as this thread. ccol_event_loop only promises that two
     * dispatches of one registration never run concurrently with EACH
     * OTHER; it says nothing about a non-callback caller like us racing a
     * dispatch we just made possible.
     *
     * A write attempt here would race that dispatch directly, and it would
     * also fail to move ctx->state and the direction to READING after a
     * complete write, which hangs a sequential reuse of the same pooled
     * connection. */
    ccol_retval_t reactivate_rv;
#ifdef RUNNING_UNIT_TESTS
    if (atomic_exchange(&g_force_reactivate_fail_for_tests, false)) {
      /* Simulates a failed ccol_event_loop_modify. We skip the real call on
       * purpose, so ctx->reg stays exactly as a real failure would leave it,
       * registered for read. The comment of this branch below says why
       * ccol_event_loop_modify never changes the direction on any failure
       * path it can return through, and the comment of
       * g_force_reactivate_fail_for_tests says why the real call can never
       * fail here. */
      reactivate_rv = ccol_unexpected_failure;
    } else
#endif
    {
      reactivate_rv = ccol_event_loop_modify(cli_engine_bundler.reactor,
                                             ctx->reg, ccol_select_write);
    }
    if (reactivate_rv != ccol_success) {
      /* See the field comment of pending_app_teardown. The ctx's
       * ccol_event_loop registration is fully live here, since we just
       * popped it from the idle pool for reuse, so a reactor dispatch for it
       * can legitimately be in flight, or about to run, on another thread
       * right now.
       *
       * Calling _async_ctx_teardown directly from this thread, which isn't a
       * dispatch thread, would be a use-after-free, so we only mark ctx as
       * terminal.
       *
       * pending_app_teardown is set BEFORE _async_retry_hop, whose very
       * first statement sets ctx->hop_completed. The order of the two
       * writes doesn't matter for correctness, because the top check of
       * _async_on_writable_impl can't be dispatched during this window
       * anyway (see the comment of that check), but we set
       * pending_app_teardown first so correctness never ends up relying on
       * that.
       *
       * Then the fd is shut down below, which forces a real EPOLLIN or
       * EPOLLERR dispatch, and whichever dispatch notices it runs the real
       * destructive teardown safely from a dispatch context (see
       * _async_ctx_handle_if_abandoned). Nothing may touch ctx after
       * idle_lock is unlocked. */
      ctx->pending_app_teardown = true;
      _async_retry_hop(ctx); /* Sets ctx->hop_completed to true itself and
                              * queues a brand-new ctx to retry the
                              * request. From here on we abandon this
                              * ctx, the reused one whose reactivation
                              * just failed. */
      /* Done with idle_lock held, for the same reason as in the reused
       * branch of _async_submit_hop_fail. Unlocking first would let a
       * dispatch parked in _async_dispatch_kind run the real teardown and
       * close() ctx->fd, and a shutdown() after that would act on a bare
       * int that the kernel may already have given to an unrelated socket
       * in this process. */
#ifdef RUNNING_UNIT_TESTS
      atomic_store(&g_abandon_shutdown_under_lock_for_tests,
                   ctx->idle_lock_held_for_tests ? 1 : 0);
#endif
      if (ctx->fd >= 0) shutdown(ctx->fd, SHUT_RDWR);
#ifdef RUNNING_UNIT_TESTS
      ctx->idle_lock_held_for_tests = false;
#endif
      _ccol_mutex_unlock(ctx->idle_lock);
      /* Drops the SEPARATE engine reference that _async_idle_pool_offer
       * took for this ctx while it was in the idle pool. _async_ctx_teardown
       * doesn't know about it, it only drops the ctx's chain reference once
       * the deferred dispatch runs it.
       *
       * It's the same leak that the reused branch of _async_submit_hop_fail
       * guards against (see the comment there), only reached through a
       * failed ccol_event_loop_modify.
       *
       * Safe to call straight from this thread, since it only touches a
       * global counter and never ctx. */
      _client_engine_release();
      return true;
    }
    /* ctx is consistent again: every per-hop field above is set and the
     * registration reflects WRITING. So we can unlock idle_lock, and a
     * dispatch that was waiting for it carries on against a coherent ctx,
     * which can be almost immediate since a freshly reused fd is nearly
     * always writable already. */
#ifdef RUNNING_UNIT_TESTS
    ctx->idle_lock_held_for_tests = false;
#endif
    _ccol_mutex_unlock(ctx->idle_lock);
    /* From here on this ctx's connection belongs to the chain and not to the
     * idle pool, so we drop the engine reference it held while pooled. The
     * chain's single reference, held for its whole lifetime, covers it.
     *
     * No deadline registration here: the ctx was registered when it was
     * created on its first fresh hop, and that registration survives every
     * round trip through the idle pool (see the comment of
     * _client_deadline_register). */
    _client_engine_release();
    return true;
  }

  /* Register ctx BEFORE submitting the task. Once ctpool_submit hands ctx to
   * a worker, that worker can connect, finish the whole hop and free ctx
   * before this thread gets around to registering it. Registering first
   * means the registry holds ctx for the whole time any other thread can
   * touch it. */
  _client_deadline_register(ctx);
  _client_deadline_note(&ctx->connect_deadline);
  _client_deadline_note(&chain->overall_deadline);

  ccol_retval_t sr = _async_connect_begin(ctx);
  if (sr != ccol_success) {
    _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
    _async_ctx_teardown(ctx); /* Also unregisters ctx, through
                               * _async_ctx_free. */
    return false;
  }
  return true;
}

/*
 * Called once a hop's response has been fully parsed as a redirect (the
 * parse sets pctx.will_redirect). We resolve the Location header against
 * this hop's URL and apply the same method and body rules as Tier 1: a 307
 * or 308 keeps the method and body, every other redirect status turns the
 * method into GET and drops the body, unless the method is already HEAD.
 * Then we finish this hop's connection, offering it to the idle pool when
 * `reusable` is true and closing it otherwise (see
 * _async_finish_connection), and only THEN queue the next hop.
 *
 * That order matters for the close case. The close(fd) inside
 * _async_ctx_free runs synchronously on THIS thread before
 * _async_finish_connection returns, while the next hop's connect() runs on
 * a different ctpool worker. A loopback redirect chain opens and closes a
 * fresh fd on every hop in quick succession, so the kernel can give that
 * worker back the EXACT SAME fd number it just freed, though only once the
 * worker really runs, and it can't run before ctpool_submit queues the next
 * hop. By finishing the connection FIRST, unconditionally, the old fd is
 * fully closed and its ccol_event_loop registration fully removed before
 * _async_submit_hop ever calls ctpool_submit, so the new hop's socket()
 * can never get the number of an old fd the reactor holds. A pooled
 * connection frees no fd at all, so the order costs nothing there.
 *
 * This connection's job is over whether or not the handoff to the next hop
 * works. On failure _async_submit_hop has already fulfilled the future with
 * a specific error, and for a Location that doesn't resolve we create no
 * next hop and the chain's refcount stays the same. Either way the teardown
 * of this ctx inside _async_finish_connection is the one guaranteed to
 * notice, through the backstop in _async_chain_release, that nothing
 * fulfilled the future, and it then fulfils it with a generic error.
 */
static void _async_handle_redirect(chttp_async_ctx_t *ctx, bool reusable) {
  chttp_async_chain_t *chain = ctx->chain;
  /* A temporary extra reference. The _async_finish_connection call below
   * can start a full teardown of THIS ctx's chain reference (through the
   * teardown of _async_ctx_finish, or the explicit release after a
   * successful idle pool offer), and that teardown runs at the same time on
   * a DIFFERENT reactor thread. If it's the last live reference, chain gets
   * freed while we use our local `chain` below, in the arguments of
   * _async_submit_hop and in _ccol_mem_free(chain->mp, ...). It's the same
   * kind of race this whole section is about (a concurrent teardown freeing
   * something a synchronous caller needs), just one level above the
   * per-ctx ones. Retaining first and releasing only after we're completely
   * done with `chain` keeps the count from reaching zero too early, however
   * fast a concurrent teardown is. */
  _async_chain_retain(chain);

  if (ctx->hop >= CHTTP_MAX_REDIRECTS) {
    /* This response belongs to the 51st request of the chain and is ALSO a
     * redirect, so following it would go past the budget of 50 redirects.
     * Like the same cap check in Tier 1's chttp_do_internal, we return an
     * error instead of quietly delivering the response, because a caller
     * using ccol_http_too_many_redirects to spot a redirect loop needs a
     * real error and not a stale 3xx to notice and inspect. No URL
     * resolution and no next-hop work is attempted. */
    _async_finish_connection(ctx, reusable);
    _async_fulfill_chain(chain, ccol_http_too_many_redirects, NULL);
    _async_chain_release(chain);
    return;
  }

  chttp_url_t base;
  memset(&base, 0, sizeof(base));
  base.is_https = ctx->is_https;
  base.is_ipv6 = ctx->is_ipv6;
  base.is_unix = ctx->is_unix;
  base.unix_socket_path = ctx->unix_socket_path;
  base.host = ctx->host;
  base.port = ctx->port;
  /* Two fields, is_ipv6 above and path_and_query below, come from
   * ctx and can't stay at the zero memset leaves. A false is_ipv6 quietly
   * gives a redirect target like "scheme://<ipv6-literal>:port/path" with
   * no brackets, which the next hop then can't parse. A NULL path_and_query
   * crashes the process with a strchr() on NULL inside _merge_ref_path as
   * soon as a server sends a really relative Location (not an absolute
   * path, not a full URL, not protocol-relative). _async_submit_hop
   * captures both ctx fields again on every hop (see their struct
   * comments). */
  base.path_and_query = ctx->path_and_query;

  ccol_retval_t resolve_rv = ccol_success;
  char *next_url =
      _resolve_redirect_url(chain->mp, &base, ctx->pctx.location, &resolve_rv);
  /* See _redirect_transport_allowed. A refused target is reported as
   * ccol_http_invalid_url, like the same check in chttp_do_internal for
   * Tier 1 and Tier 3. We check it here while `base` describes the hop this
   * Location came in on, because the target must be allowed against that
   * hop. */
  bool transport_refused = false;
  ccol_retval_t transport_rv =
      next_url ? _redirect_transport_allowed(
                     chain->mp, &base, next_url,
                     chain->prevent_tls_downgrade_on_redirect)
               : ccol_success;
  if (transport_rv != ccol_success) {
    _ccol_mem_free(chain->mp, next_url);
    next_url = NULL;
    transport_refused = true;
  }
  bool preserve =
      (ctx->pctx.status_code == 307 || ctx->pctx.status_code == 308);
  chttp_method_t next_method = ctx->cur_method;
  /* We look at chain->body_dropped rather than taking body_data, body_len
   * and body_content_type unconditionally. An earlier hop of this chain may
   * already have dropped the body through a redirect that doesn't preserve
   * the method, and then it must STAY dropped on every later hop, even a
   * later 307 or 308, which preserves the CURRENT body and doesn't bring
   * back the chain's original hop-0 body. The field comment of
   * chain->body_dropped explains the reasoning, and also why Tier 1's
   * counterpart (the cur_body loop local, overwritten in place) needs no
   * such flag. */
  const void *next_body_data = chain->body_dropped ? NULL : chain->body_data;
  size_t next_body_len = chain->body_dropped ? 0 : chain->body_len;
  const char *next_body_ct =
      chain->body_dropped ? NULL : chain->body_content_type;
  if (!preserve && ctx->cur_method != CHTTP_HEAD) {
    next_method = CHTTP_GET;
    next_body_data = NULL;
    next_body_len = 0;
    next_body_ct = NULL;
    chain->body_dropped = true;
  }
  /* Read BEFORE _async_finish_connection and never after it. That call can
   * start the ordinary teardown of THIS ctx through a dispatch on another
   * reactor thread, the teardown frees ctx, and then nothing may read
   * ctx->hop. The temporary chain retain above only protects `chain`, not
   * ctx: once its connection has gone to _async_finish_connection, nothing
   * may touch ctx. */
  int next_hop = ctx->hop + 1;

  _async_finish_connection(ctx, reusable);

  if (next_url) {
    _async_submit_hop(chain, next_url, next_method, next_body_data,
                      next_body_len, next_body_ct, next_hop);
    _ccol_mem_free(chain->mp, next_url);
  } else if (transport_refused) {
    /* We fulfil the future ourselves instead of leaving it to the
     * ccol_http_transfer_aborted backstop of _async_chain_release below, so
     * every tier reports the same code for a refused redirect target:
     * ccol_http_invalid_url, or ccol_not_enough_memory when the check ran
     * out of memory parsing the target. */
    _async_fulfill_chain(chain, transport_rv, NULL);
  } else {
    /* The Location didn't resolve. resolve_rv separates a failed allocation
     * (ccol_not_enough_memory) from an empty Location, the same as in Tier 1
     * and Tier 3. */
    _async_fulfill_chain(chain, resolve_rv, NULL);
  }
  _async_chain_release(chain); /* Drops the temporary reference taken
                                * above. */
}

/*
 * Pre-flight validation for Tier 2 and Tier 3, which both queue through
 * _chttp_do_async_internal. It validates cli and req, parses req->url into
 * *url_out (after ccol_success the caller must _url_free it), and checks
 * that TLS is usable. Those are the same three checks chttp_do_internal
 * makes in Tier 1, with the same result codes: ccol_invalid_args, whatever
 * _parse_chttp_url returns, and ccol_http_tls_cert_load_failed.
 * _chttp_do_async_internal passes the code on through its rv_out, so Tier
 * 3 reports it exactly like Tier 1.
 *
 * Tier 1 does NOT use this, because its versions of these checks sit in
 * the per-hop loop of chttp_do_internal and run again on every hop. A
 * redirect can change the URL and the scheme between hops, so there's no
 * single up-front check to pull out of Tier 1. Tier 2 and Tier 3 need it
 * only once, before any chain or future exists.
 */
static ccol_retval_t _chttp_async_preflight_check(struct chttpclient *cli,
                                                  const chttp_request_t *req,
                                                  chttp_url_t *url_out) {
  if (!cli || !req) return ccol_invalid_args;

  ccol_retval_t prv = _parse_chttp_url(cli->m_procs, req->url, url_out);
  if (prv != ccol_success) return prv;

  _ccol_mutex_lock(cli->lock);
  bool tls_ctx_usable = cli->tls_ctx_usable;
  _ccol_mutex_unlock(cli->lock);

  if (url_out->is_https && !tls_ctx_usable) {
    /* At the set_tls call the configured cert, key or CA path couldn't be
     * read, or ctls failed to load or parse it, and instead of stopping the
     * process we report it here (see _rebuild_tls_ctx_locked). Same check
     * and same error code as in Tier 1; the comment on that check in
     * chttp_do_internal explains why it's ccol_http_tls_cert_load_failed and
     * not ccol_http_tls_handshake_failed. */
    _url_free(cli->m_procs, url_out);
    return ccol_http_tls_cert_load_failed;
  }
  return ccol_success;
}

/*
 * Internal entry point that submits req to run asynchronously against cli.
 * It returns a future, and the caller must pair it with exactly one
 * ctpool_future_free, optionally after a ctpool_future_get or
 * ctpool_future_done.
 *
 * It returns NULL when the request can't even be queued: bad arguments, an
 * invalid URL, unusable TLS, a failed allocation, or an engine that won't
 * start, the same "NULL on failure" convention as ctpool_submit_future.
 * *rv_out holds the specific cause then, and ccol_success when a future is
 * returned.
 *
 * Once a future exists it is returned for every later failure as well (a
 * failure on a later redirect hop, say), fulfilled with a specific error
 * instead of a NULL return, the way Tier 1 returns a ccol_retval_t and
 * never fails quietly.
 *
 * Every queued hop is freed unconditionally, together with the single
 * engine reference of the whole chain, once the redirect chain reaches a
 * terminal connection state (see the file-level comment of
 * chttp_async_chain_t).
 */
static ctpool_future *_chttp_do_async_internal(struct chttpclient *cli,
                                               const chttp_request_t *req,
                                               chttpcli_write_fn write_fn,
                                               void *write_ctx,
                                               ccol_retval_t *rv_out) {
  chttp_url_t url;
  ccol_retval_t prv = _chttp_async_preflight_check(cli, req, &url);
  if (prv != ccol_success) {
    *rv_out = prv;
    return NULL;
  }
  *rv_out = ccol_not_enough_memory;
  ccol_memmgmt_procs_t *mp = cli->m_procs;
  /* Copied out before url is freed. Nothing else needs url, since the
   * pre-check above was its only other user and _async_submit_hop parses
   * req->url again on every hop anyway. The chain keeps track of whether
   * the origin changed since hop 0 (see the comment of
   * chttp_async_chain_t.initial_origin_key), so hop 0's origin_key needs to
   * live as long as the chain, not just this stack frame. */
  char *initial_origin_key = ccol_strdup(mp, url.origin_key);
  _url_free(mp, &url);
  if (!initial_origin_key) return NULL;

  /* Read and pin the client's TLS context under its lock, as Tier 1's
   * chttp_do_internal does. ctls_ctx_retain protects it from a concurrent
   * chttpclient_set_tls, which could otherwise free or rebuild the context
   * while this request is using it.
   *
   * We pin it unconditionally, not only when the first hop is https,
   * because a redirect chain can go back and forth between http and https,
   * just like with Tier 1's tls_ctx local. _async_chain_release drops the
   * pinned reference once, with ctls_ctx_release, when the last hop of the
   * whole chain is torn down. */
  ctls_ctx_t *tls_ctx;
  bool tls_ctx_usable;
  bool verify_host;
  /* Snapshotted under the same lock as tls_ctx, so the chain carries one
   * coherent TLS identity (see the field comment of
   * chttpclient.tls_generation). */
  uint64_t tls_generation;
  uint64_t connect_timeout_us;
  uint64_t request_timeout_us;
  size_t max_response_body_size;
  _ccol_mutex_lock(cli->lock);
  tls_ctx = cli->tls_ctx;
  tls_ctx_usable = cli->tls_ctx_usable;
  /* The hostname is matched only when the chain is verified at all, since
   * matching against a certificate nobody validated proves nothing (see
   * the field comments of chttp_tls_config_t). */
  verify_host =
      !cli->tls.insecure_skip_verify && !cli->tls.insecure_skip_hostname_check;
  tls_generation = cli->tls_generation;
  connect_timeout_us = cli->connect_timeout_us;
  request_timeout_us = cli->request_timeout_us;
  max_response_body_size = cli->max_response_body_size;
  if (tls_ctx) ctls_ctx_retain(tls_ctx);
  _ccol_mutex_unlock(cli->lock);

  ccol_retval_t arv = _client_engine_acquire_chain();
  if (arv != ccol_success) {
    if (tls_ctx) ctls_ctx_release(tls_ctx);
    _ccol_mem_free(mp, initial_origin_key);
    *rv_out = arv;
    return NULL;
  }

  /* The result the future will carry is allocated here, before the future
   * exists, so no later step can end up unable to report its code (see
   * _async_fulfill_chain). ctpool_future_create_detached only fails when an
   * allocation fails. */
  chttpcli_async_result_t *result = _async_result_alloc(mp);
  char *ferr = NULL;
  ctpool_future *future = result ? ctpool_future_create_detached(&ferr) : NULL;
  if (!future) {
    if (result) chttpclient_async_result_free(result);
    if (tls_ctx) ctls_ctx_release(tls_ctx);
    _client_async_request_settled();
    _client_engine_release_chain();
    _ccol_mem_free(mp, initial_origin_key);
    return NULL;
  }
  *rv_out = ccol_success;
  /* From here on a future exists and we always return it to the caller, who
   * owns pairing it with exactly one ctpool_future_free. Every later failure
   * fulfils it with a specific error instead of returning NULL.
   *
   * We keep the future in a local now because once hop 0 is queued below, a
   * worker can run the request to completion and free the chain through
   * _async_chain_release before this thread executes one more instruction,
   * and reading chain->future after that is a use-after-free. */
  ctpool_future *f = future;

  chttp_async_chain_t *chain = _async_chain_create(
      mp, cli, future, (chmap)req->headers, req->body.data, req->body.len,
      req->body.content_type, initial_origin_key, tls_ctx, tls_ctx_usable,
      verify_host, tls_generation, connect_timeout_us, request_timeout_us,
      max_response_body_size, write_fn, write_ctx, req->expect_continue,
      req->prevent_tls_downgrade_on_redirect);
  /* _async_chain_create deep-copies initial_origin_key, like req_headers,
   * body_data and body_content_type, so our local copy never outlives this
   * call, whether the chain was created or not. */
  _ccol_mem_free(mp, initial_origin_key);
  if (!chain) {
    if (tls_ctx) ctls_ctx_release(tls_ctx);
    /* _async_chain_create only fails when an allocation fails (the calloc,
     * the _ccol_mutex_init, or one of its deep copies), so
     * ccol_not_enough_memory is always the right cause here.
     *
     * We report it through the chttpcli_async_result_t the request already
     * owns rather than fulfilling the future with a bare NULL. That way
     * chttpclient_async_result_get returns a real result carrying the error
     * code, and this specific, common failure doesn't collapse into the
     * same NULL a cancelled future gives, which Tier 3 couldn't tell apart
     * from any other cause. */
    _client_async_request_settled();
    result->rv = ccol_not_enough_memory;
    result->resp = NULL;
    ctpool_future_fulfill(future, result);
    _client_engine_release_chain();
    return f;
  }
  chain->result = result;
  _async_submit_hop(chain, req->url, req->method, req->body.data, req->body.len,
                    req->body.content_type, 0);
  return f;
}

/* White-box test helpers that expose the engine's internal state. They are
 * not public API, and a gate keeps these symbols out of a production build
 * of libccollections.so. The engine code around them has no such gate,
 * because chttpclient_do_async and chttpclient_do_async_streaming really
 * call it, while these functions only exist to instrument tests. */
#ifdef RUNNING_UNIT_TESTS
/* Exposes sizeof(chttp_async_chain_t), an internal type, so a test can
 * build a fault-injecting allocator that targets that size and fails
 * exactly the chain struct's calloc inside _async_chain_create, however
 * many allocations of other sizes (some from parsing the URL) come before
 * it. That's deterministic: a change that moves the number of earlier
 * allocations can't break it, while it would break a fault injector that
 * counts an index. */
size_t _chttp_async_chain_struct_size_for_tests(void) {
  return sizeof(chttp_async_chain_t);
}

int _chttpclient_engine_ref_count_for_tests(void) {
  _ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  _ccol_mutex_lock(cli_engine_bundler.mutex);
  int n = (int)cli_engine_bundler.reactor_refs;
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
  return n;
}

clog _chttpclient_engine_logger_for_tests(void) {
  _ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  _ccol_mutex_lock(cli_engine_bundler.mutex);
  clog l = cli_engine_bundler.logger;
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
  return l;
}

/* Removes the logger installed by chttpcli_set_engine_logger and closes it,
 * so the next engine run opens a fallback logger. A test that needs the
 * fallback calls this in a forked child, whatever tests ran before it. The
 * engine must not be running. */
void _chttpclient_clear_engine_user_logger_for_tests(void) {
  _ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  _ccol_mutex_lock(cli_engine_bundler.mutex);
  clog user = cli_engine_bundler.user_logger;
  cli_engine_bundler.user_logger = CLOG_INVALID;
  if (cli_engine_bundler.logger == user)
    cli_engine_bundler.logger = CLOG_INVALID;
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
  if (user) clog_close(user);
}

bool _chttpclient_engine_running_for_tests(void) {
  _ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  _ccol_mutex_lock(cli_engine_bundler.mutex);
  bool running = (cli_engine_bundler.reactor != CCOL_EVENT_LOOP_INVALID);
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
  return running;
}

ccol_retval_t _chttpclient_engine_acquire_for_tests(void) {
  return _client_engine_acquire();
}

void _chttpclient_engine_release_for_tests(void) { _client_engine_release(); }

void _chttpclient_engine_wait_for_quiescence_for_tests(void) {
  _client_engine_wait_for_quiescence();
}

/* Whether this process has started initializing the engine globals. */
bool _chttpclient_engine_globals_entered_for_tests(void) {
  return atomic_load(&g_engine_globals_entered);
}

/* The DNS pool of the running engine, or CTPOOL_INVALID when there's no
 * engine. */
ctpool _chttpclient_engine_dns_pool_for_tests(void) {
  _ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  _ccol_mutex_lock(cli_engine_bundler.mutex);
  ctpool h = cli_engine_bundler.dns_pool;
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
  return h;
}

/* See g_reaper_final_hold_ms_for_tests. */
void _chttpclient_hold_next_reaper_final_section_for_tests(int ms) {
  atomic_store(&g_reaper_final_hold_entered_for_tests, false);
  atomic_store(&g_reaper_final_hold_ms_for_tests, ms);
}

bool _chttpclient_reaper_final_section_was_entered_for_tests(void) {
  return atomic_load(&g_reaper_final_hold_entered_for_tests);
}

/* See g_reaper_tail_hold_ms_for_tests. */
void _chttpclient_hold_next_reaper_tail_for_tests(int ms) {
  atomic_store(&g_reaper_tail_hold_entered_for_tests, false);
  atomic_store(&g_reaper_tail_hold_left_for_tests, false);
  atomic_store(&g_reaper_tail_hold_ms_for_tests, ms);
}

bool _chttpclient_reaper_tail_was_entered_for_tests(void) {
  return atomic_load(&g_reaper_tail_hold_entered_for_tests);
}

bool _chttpclient_reaper_tail_was_left_for_tests(void) {
  return atomic_load(&g_reaper_tail_hold_left_for_tests);
}

/* Waits until no engine teardown is in flight and leaves the handle of the
 * reaper that finished it unjoined, which is the state a process is in
 * between its engine stopping and the next acquire. */
void _chttpclient_engine_wait_stopped_unjoined_for_tests(void) {
  _ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  _ccol_mutex_lock(cli_engine_bundler.mutex);
  while (cli_engine_bundler.stopping)
    _ccol_cond_var_wait(cli_engine_bundler.stopped_cv,
                        cli_engine_bundler.mutex);
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
}

/* Copies the handle of the not yet joined reaper into *out, and returns
 * false when there isn't one. */
bool _chttpclient_engine_unjoined_reaper_for_tests(_ccol_thread_id_t *out) {
  _ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  _ccol_mutex_lock(cli_engine_bundler.mutex);
  bool joinable = cli_engine_bundler.reaper_joinable;
  if (joinable) *out = cli_engine_bundler.reaper_thread;
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
  return joinable;
}

/* See the comment of g_abandon_shutdown_under_lock_for_tests. Returns -1 if
 * no shutdown of an abandoned ctx has run yet, 1 if the last one ran under
 * that ctx's idle_lock (the only safe order), and 0 if it ran after the
 * unlock. */
int _chttpclient_abandon_shutdown_under_lock_for_tests(void) {
  return atomic_load(&g_abandon_shutdown_under_lock_for_tests);
}

void _chttpclient_reset_abandon_shutdown_probe_for_tests(void) {
  atomic_store(&g_abandon_shutdown_under_lock_for_tests, -1);
}

/* See the TLS direction injection block near the top of this file. Mode 0
 * disarms the injection, mode 1 means "a TLS write needs the connection to
 * become readable first" and mode 2 means "a TLS read needs it to become
 * writable first". Arming also clears every counter so each test starts
 * from a known state. A test must disarm it before returning, because
 * this module's reactor threads keep running between tests. */
void _chttpclient_tls_dir_inject_for_tests(int mode) {
  atomic_store(&g_tls_dir_blocked, false);
  atomic_store(&g_tls_dir_released, false);
  atomic_store(&g_tls_dir_block_attempts, 0);
  atomic_store(&g_tls_dir_read_interest_while_writing, 0);
  atomic_store(&g_tls_dir_write_interest_while_reading, 0);
  atomic_store(&g_tls_dir_inject_mode, mode);
}

/* True once the module has really released the injected block, which
 * happens when it waits or registers in the direction the injection asked
 * for. */
bool _chttpclient_tls_dir_released_for_tests(void) {
  return atomic_load(&g_tls_dir_released);
}

unsigned _chttpclient_tls_dir_read_interest_while_writing_for_tests(void) {
  return atomic_load(&g_tls_dir_read_interest_while_writing);
}

unsigned _chttpclient_tls_dir_write_interest_while_reading_for_tests(void) {
  return atomic_load(&g_tls_dir_write_interest_while_reading);
}

/* How many TLS I/O attempts the armed injection above has refused so far. A
 * caller that follows the reported direction pays exactly one refusal,
 * since its wait or registration in that direction releases the injection
 * and the next attempt goes through. A caller that keeps retrying in its
 * own home direction on a connection that is already ready is busy
 * spinning, and this counter is what measures the spin. */
unsigned _chttpclient_tls_dir_block_attempts_for_tests(void) {
  return atomic_load(&g_tls_dir_block_attempts);
}

/* Holds the next reaper that _client_engine_spawn_reaper creates between its
 * creation and the publishing of its handle, until the test releases it (see
 * the comment of g_reaper_spawn_hold_for_tests). */
void _chttpclient_hold_next_reaper_spawn_for_tests(void) {
  atomic_store(&g_reaper_spawn_gate_for_tests, false);
  atomic_store(&g_reaper_spawn_held_for_tests, false);
  atomic_store(&g_reaper_spawn_hold_for_tests, true);
}

bool _chttpclient_reaper_spawn_is_held_for_tests(void) {
  return atomic_load(&g_reaper_spawn_held_for_tests);
}

void _chttpclient_release_reaper_spawn_for_tests(void) {
  atomic_store(&g_reaper_spawn_gate_for_tests, true);
}

unsigned _chttpclient_reaper_created_count_for_tests(void) {
  return atomic_load(&g_reaper_created_for_tests);
}

unsigned _chttpclient_reaper_joined_count_for_tests(void) {
  return atomic_load(&g_reaper_joined_for_tests);
}

/* See the comment of g_force_reaper_spawn_fail_for_tests. It makes every
 * attempt to create a reaper thread fail, so a test can exercise the give-up
 * path (the engine stays running for the next release to try again)
 * without a real thread limit. */
void _chttpclient_force_reaper_spawn_fail_for_tests(bool on) {
  atomic_store(&g_force_reaper_spawn_fail_for_tests, on);
}

unsigned _chttpclient_engine_reaper_abandoned_count_for_tests(void) {
  return atomic_load(&g_reaper_abandoned_count_for_tests);
}

size_t _chttpclient_engine_num_reactor_threads_for_tests(void) {
  _ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  _ccol_mutex_lock(cli_engine_bundler.mutex);
  size_t n = cli_engine_bundler.last_resolved_num_reactor_threads;
  _ccol_mutex_unlock(cli_engine_bundler.mutex);
  return n;
}

/* Moves the last_used timestamp of every idle connection in the Tier 2 and
 * Tier 3 pool far enough into the past that the CHTTP_IDLE_MAX_AGE_MS
 * staleness check of _async_idle_pool_take finds it too old on the very
 * next pop, so a test doesn't need to sit out the real 60 second window.
 * Test-only, to make the staleness eviction path of _async_idle_pool_take
 * reachable deterministically. */
void _chttpclient_force_async_idle_stale_for_tests(struct chttpclient *cli) {
  _ccol_mutex_lock(cli->lock);
  if (cli->idle_pools_async) {
    cmap_iterator *it = chashmap_begin_iter(cli->idle_pools_async, NULL);
    for (; it; it = it->_next_fn(it)) {
      cvec list = _read_cvec(it->val_pair->ptr);
      if (!list) continue;
      size_t n = cvector_elem_count(list);
      for (size_t i = 0; i < n; i++) {
        chttp_async_ctx_t *actx = *(chttp_async_ctx_t **)cvector_at(list, i);
        actx->last_used.tv_sec -= (CHTTP_IDLE_MAX_AGE_MS / 1000L) + 5;
      }
    }
  }
  _ccol_mutex_unlock(cli->lock);
}

/* Shuts down every fd currently in the Tier 2 and Tier 3 async idle pool
 * and leaves each connection where it is. Each connection's IDLE-state
 * dispatch (_async_idle_ctx_finish) then removes it from the pool and frees
 * it with its engine reference, from a reactor dispatch context and
 * asynchronously to this call. The teardown sweep of __chttpclient_destroy
 * uses the same mechanism, and shutdown() is just as safe under cli->lock
 * there since it runs no application callback synchronously.
 *
 * Test-only. It lets a test put the release of the LAST engine reference on
 * a reactor thread on purpose, instead of waiting for a server to close a
 * pooled connection at some moment nobody controls. */
void _chttpclient_shutdown_async_idle_connections_for_tests(
    struct chttpclient *cli) {
  _ccol_mutex_lock(cli->lock);
  if (cli->idle_pools_async) {
    cmap_iterator *it = chashmap_begin_iter(cli->idle_pools_async, NULL);
    for (; it; it = it->_next_fn(it)) {
      cvec list = _read_cvec(it->val_pair->ptr);
      if (!list) continue;
      size_t n = cvector_elem_count(list);
      for (size_t i = 0; i < n; i++) {
        chttp_async_ctx_t *actx = *(chttp_async_ctx_t **)cvector_at(list, i);
        int afd = actx->fd;
        if (afd >= 0) shutdown(afd, SHUT_RDWR);
      }
    }
  }
  _ccol_mutex_unlock(cli->lock);
}

/* Reads cli->idle_total_count_async, the number of connections in the Tier
 * 2 and Tier 3 async idle pool right now, over all origins. Test-only. It
 * lets a test check pool membership directly instead of working it out
 * indirectly, which it needs to state an invariant that doesn't depend on
 * an ordinal: "whenever this pool is empty, its contribution to the engine
 * refcount is zero too", true whichever internal allocation an injected
 * out-of-memory failure hits. */
size_t _chttpclient_async_idle_total_count_for_tests(struct chttpclient *cli) {
  _ccol_mutex_lock(cli->lock);
  size_t n = cli->idle_total_count_async;
  _ccol_mutex_unlock(cli->lock);
  return n;
}

/* Counts the connections in cli's async idle pool, over all origins, that
 * carry a timed_out mark. An idle connection should never have one, since
 * the mark belongs to the hop that ended when the connection went idle.
 * Test-only. */
size_t _chttpclient_async_idle_timed_out_count_for_tests(
    struct chttpclient *cli) {
  size_t n = 0;
  _ccol_mutex_lock(cli->lock);
  if (cli->idle_pools_async) {
    cmap_iterator *it = chashmap_begin_iter(cli->idle_pools_async, NULL);
    for (; it; it = it->_next_fn(it)) {
      cvec list = _read_cvec(it->val_pair->ptr);
      size_t count = list ? cvector_elem_count(list) : 0;
      for (size_t i = 0; i < count; i++) {
        chttp_async_ctx_t **slot = (chttp_async_ctx_t **)cvector_at(list, i);
        if ((*slot)->timed_out) n++;
      }
    }
  }
  _ccol_mutex_unlock(cli->lock);
  return n;
}

/* Number of distinct origin keys that Tier 1's cli->idle_pools has an entry
 * for right now. Test-only. It lets a test check that popping the last
 * connection of an origin really prunes that origin's (empty) chmap
 * entry; without the prune an empty cvec for it would stay there forever
 * (see the prune comment in _idle_pool_take). */
size_t _chttpclient_idle_pools_key_count_for_tests(struct chttpclient *cli) {
  _ccol_mutex_lock(cli->lock);
  size_t n = cli->idle_pools ? chmap_elem_count(cli->idle_pools) : 0;
  _ccol_mutex_unlock(cli->lock);
  return n;
}

/* Tier 2 and Tier 3 counterpart of
 * _chttpclient_idle_pools_key_count_for_tests above; it reads
 * cli->idle_pools_async. */
size_t _chttpclient_idle_pools_async_key_count_for_tests(
    struct chttpclient *cli) {
  _ccol_mutex_lock(cli->lock);
  size_t n =
      cli->idle_pools_async ? chmap_elem_count(cli->idle_pools_async) : 0;
  _ccol_mutex_unlock(cli->lock);
  return n;
}

/* Overrides the effective CHTTP_MAX_IDLE_ORIGINS, shared by _idle_pool_offer
 * and _async_idle_pool_offer, for the rest of the process (see the comment
 * of g_max_idle_origins_override_for_tests). Pass 0 to go back to the real
 * compile-time constant. Like every other *_for_tests fault-injection hook
 * in this file, it applies to the whole process and not to one client. */
void _chttpclient_set_max_idle_origins_for_tests(size_t n) {
  atomic_store(&g_max_idle_origins_override_for_tests, n);
}

/* Resolves h to its struct chttpclient* without pinning it. It doesn't
 * touch the pin index at all, it's a bare slot table lookup. That's fine
 * for tests, since the test code calling it runs synchronously on one
 * thread and there's no concurrent destroy to race.
 *
 * _chttpcli_resolve on the other hand needs a matching _unpin that a test
 * has to remember, which is easy to forget, and a forgotten unpin leaves a
 * pin on the client so every later chttpclient_destroy of it hangs without
 * a word.
 *
 * This returns NULL under exactly the same conditions as
 * _chttpcli_resolve, because the slot table record read here and the pin
 * index record read there are flipped under the same lock and can't
 * disagree. */
struct chttpclient *_chttpcli_resolve_for_tests(chttpcli h) {
  _ccol_call_once(chttpcli_slot_table.once, _chttpcli_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  _ccol_rw_lock_rdlock(chttpcli_slot_table.rwlock);
  struct chttpclient *raw = NULL;
  if (idx < cvector_elem_count(chttpcli_slot_table.slots)) {
    chttpcli_slot_t *slot =
        (chttpcli_slot_t *)cvector_at(chttpcli_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  _ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
  return raw;
}

/* Number of slots the chttpcli handle table holds right now, both the ones
 * it grew and the freed ones not reused yet. A test uses it to assert that
 * a loop creating and destroying clients reuses freed slots instead of
 * growing the table without bound. */
/* The indices currently on the free list; read it together with the
 * capacity above. A rollback that loses a slot shows up as table growth,
 * but only while the free list was empty, so a test that looks at the
 * growth alone passes or fails depending on how many handles earlier tests
 * happened to hold at once. Together the two figures describe the table
 * without that dependency. */
size_t _chttpcli_free_index_count_for_tests(void) {
  _ccol_call_once(chttpcli_slot_table.once, _chttpcli_slot_table_init_globals);
  _ccol_rw_lock_rdlock(chttpcli_slot_table.rwlock);
  size_t n = cvector_elem_count(chttpcli_slot_table.free_indices);
  _ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
  return n;
}

size_t _chttpcli_slot_table_capacity_for_tests(void) {
  _ccol_call_once(chttpcli_slot_table.once, _chttpcli_slot_table_init_globals);
  _ccol_rw_lock_rdlock(chttpcli_slot_table.rwlock);
  size_t n = cvector_elem_count(chttpcli_slot_table.slots);
  _ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
  return n;
}

/* Number of pins currently held on h's slot. A test can assert directly
 * that resolves and unpins stay balanced, instead of finding out from a
 * destroy that never returns. It takes the handle and not a resolved
 * pointer, so the caller needs no pin of its own to ask. */
size_t _chttpcli_pin_count_for_tests(chttpcli h) {
  return ccol_pintable_pins_for(&chttpcli_pintable, h);
}

/* Makes the very next cvector_push_back in _async_idle_pool_offer count as
 * a failure, for any client and any origin, without touching the real
 * allocator or the origin's real list. The flag is consumed the first time
 * that call site is reached, so call this again before every attempt that
 * needs to hit the path. The comment of g_force_offer_push_fail_for_tests,
 * above the "ASYNC IDLE POOL" section, says why ordinary allocator failure
 * injection can't get to this scenario. */
void _chttpclient_force_offer_push_fail_once_for_tests(void) {
  atomic_store(&g_force_offer_push_fail_for_tests, true);
}

/* Arms the next plain-HTTP hop that finishes writing its request to flip
 * its own registration straight back to the write direction. That
 * reproduces a continue-timeout flip by the deadline sweep that lands after
 * the hop has already moved on (see the comment of
 * g_force_stray_write_arm_for_tests, above the "ASYNC IDLE POOL" section).
 * The first such hop consumes the flag. */
void _chttpclient_force_stray_write_arm_once_for_tests(void) {
  atomic_store(&g_force_stray_write_arm_for_tests, true);
}

/* Arms the next connection that lands in the async idle pool to flip its
 * own registration straight back to the write direction. It's the pooled
 * connection version of the hook above (see the comment of
 * g_force_stray_idle_write_arm_for_tests). */
void _chttpclient_force_stray_idle_write_arm_once_for_tests(void) {
  atomic_store(&g_force_stray_idle_write_arm_for_tests, true);
}

/* Sets the delay in milliseconds on each side of the carry-over replay for
 * "Expect: 100-continue"; 0 turns it off. See the comment of
 * g_continue_carry_replay_delay_ms_for_tests. */
/* See g_idle_probe_hold_for_tests. */
void _chttpclient_arm_idle_probe_hold_for_tests(void) {
  atomic_store(&g_idle_probe_hold_for_tests, 1);
}

/* Waits up to timeout_ms for the IDLE branch to get to the hold and tells
 * whether it did. */
bool _chttpclient_wait_idle_probe_held_for_tests(long timeout_ms) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
  for (long i = 0; i < timeout_ms; i++) {
    if (atomic_load(&g_idle_probe_hold_for_tests) == 2) return true;
    nanosleep(&ts, NULL);
  }
  return atomic_load(&g_idle_probe_hold_for_tests) == 2;
}

/* Ends the hold, or disarms it if nothing got there. */
void _chttpclient_release_idle_probe_hold_for_tests(void) {
  int holding = 2;
  if (!atomic_compare_exchange_strong(&g_idle_probe_hold_for_tests, &holding,
                                      3))
    atomic_store(&g_idle_probe_hold_for_tests, 0);
}

void _chttpclient_set_continue_carry_replay_delay_ms_for_tests(long ms) {
  atomic_store(&g_continue_carry_replay_delay_ms_for_tests, ms);
}

/* Arms one stray write dispatch. The next take of a pooled connection arms
 * the write direction of its registration while holding idle_lock, before
 * building the new hop, and waits 200 ms, which is what the deadline
 * sweep's continue-timeout flip can do when it lands on a later hop. */
void _chttpclient_arm_stray_write_at_take_for_tests(void) {
  atomic_store(&g_stray_write_at_take_for_tests, true);
}

/* Arms one stray write dispatch that races a take. The next take of a
 * pooled connection arms the write direction of its registration before
 * taking idle_lock, waits for that dispatch to classify the ctx as IDLE,
 * then moves the ctx to WRITING and holds idle_lock for 200 ms while the
 * dispatch carries on. */
void _chttpclient_arm_stray_write_before_take_for_tests(void) {
  atomic_store(&g_stray_before_take_stage_for_tests, 1);
}

/* Tells whether the armed dispatch was held as described above, and
 * disarms the hook. */
bool _chttpclient_stray_write_before_take_held_for_tests(void) {
  bool held = atomic_load(&g_stray_before_take_stage_for_tests) >= 3;
  atomic_store(&g_stray_before_take_stage_for_tests, 0);
  return held;
}

/* How many times since the process started the write-direction
 * fall-through of _async_on_writable_impl has put a registration back to
 * the read direction. */
unsigned long long _chttpclient_stray_write_arm_restores_for_tests(void) {
  return atomic_load_explicit(&g_stray_write_arm_restores_for_tests,
                              memory_order_relaxed);
}

/* Makes the very next ccol_event_loop_modify of _async_submit_hop on a
 * reused connection count as a failure, for any client, without touching
 * the real registration. The flag is consumed the first time that call site
 * is reached. The comment of g_force_reactivate_fail_for_tests, above the
 * "ASYNC IDLE POOL" section, says why ordinary allocator failure injection
 * can't get to this scenario. */
void _chttpclient_force_reactivate_fail_once_for_tests(void) {
  atomic_store(&g_force_reactivate_fail_for_tests, true);
}

/* Makes the very next read dispatch of _async_on_readable_impl a hard
 * error, but only for a ctx that has already had at least one real
 * successful read, so never the very first read of a fresh or reused
 * connection. We behave exactly as if recv() or ctls_conn_read() returned
 * -1 with ECONNRESET, without touching the real socket, and the flag is
 * consumed the first time that condition is met. The comment of
 * g_force_async_hard_read_error_for_tests, above the "ASYNC IDLE POOL"
 * section, says why the timing of a real TCP RST over a real socket can't
 * be pinned down. */
void _chttpclient_force_async_hard_read_error_once_for_tests(void) {
  atomic_store(&g_force_async_hard_read_error_for_tests, true);
}
#endif /* RUNNING_UNIT_TESTS */

ctpool_future *chttpclient_do_async(chttpcli h, const chttp_request_t *req) {
  struct chttpclient *raw = _chttpcli_resolve(h);
  if (!raw) return NULL;
  ccol_retval_t rv;
  ctpool_future *f = _chttp_do_async_internal(raw, req, NULL, NULL, &rv);
  _chttpcli_resolve_unpin(raw);
  return f;
}

ctpool_future *chttpclient_do_async_streaming(chttpcli h,
                                              const chttp_request_t *req,
                                              chttpcli_write_fn write_fn,
                                              void *write_ctx) {
  if (!write_fn) return NULL;
  struct chttpclient *raw = _chttpcli_resolve(h);
  if (!raw) return NULL;
  ccol_retval_t rv;
  ctpool_future *f =
      _chttp_do_async_internal(raw, req, write_fn, write_ctx, &rv);
  _chttpcli_resolve_unpin(raw);
  return f;
}

chttpcli_async_result_t *chttpclient_async_result_get(ctpool_future *f) {
  return (chttpcli_async_result_t *)ctpool_future_get(f);
}

void chttpclient_async_result_free(chttpcli_async_result_t *result) {
  if (!result) return;
  /* _m_procs points into the very block this call frees. */
  ccol_memmgmt_procs_t procs;
  ccol_memmgmt_procs_t *mp = NULL;
  if (result->_m_procs) {
    procs = *result->_m_procs;
    mp = &procs;
  }
  _ccol_mem_free(mp, result);
}

/* ========================================================================== */
/*                    POOLED-SYNC API (TIER 3)                                */
/* ========================================================================== */

/*
 * Both functions below are thin wrappers. Each submits the request through
 * Tier 2, blocks on the future, unwraps the result into the same shape as
 * chttpclient_do and chttpclient_do_streaming (a ccol_retval_t plus
 * resp_out, or a ccol_retval_t plus status_code_out), and frees the future
 * and its result before returning, so the caller never sees a
 * ctpool_future or a chttpcli_async_result_t.
 *
 * They call _chttp_do_async_internal directly and not the public
 * chttpclient_do_async, because only the internal one says WHY the request
 * couldn't be queued: a bad URL or an unusable TLS configuration gets the
 * same specific code chttpclient_do uses, a failed allocation gets
 * ccol_not_enough_memory, and an engine that doesn't start gets the code of
 * that failure. The public functions turn all of those into a NULL future.
 */

ccol_retval_t chttpclient_do_pooled(chttpcli h, const chttp_request_t *req,
                                    chttpcli_response **resp_out) {
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;

  struct chttpclient *raw = _chttpcli_resolve(h);
  if (!raw) return ccol_invalid_args;
  ccol_retval_t qrv;
  ctpool_future *f = _chttp_do_async_internal(raw, req, NULL, NULL, &qrv);
  /* From here the queued chain keeps the client alive on its own (see
   * async_in_flight_count in struct chttpclient). */
  _chttpcli_resolve_unpin(raw);
  if (!f) return qrv;

  chttpcli_async_result_t *result = chttpclient_async_result_get(f);
  if (!result) {
    ctpool_future_free(f);
    return ccol_unexpected_failure;
  }

  ccol_retval_t rv = result->rv;
  *resp_out = result->resp;
  chttpclient_async_result_free(result);
  ctpool_future_free(f);
  return rv;
}

ccol_retval_t chttpclient_do_pooled_streaming(chttpcli h,
                                              const chttp_request_t *req,
                                              chttpcli_write_fn write_fn,
                                              void *write_ctx,
                                              int *status_code_out) {
  if (!write_fn) return ccol_invalid_args;

  struct chttpclient *raw = _chttpcli_resolve(h);
  if (!raw) return ccol_invalid_args;
  ccol_retval_t qrv;
  ctpool_future *f =
      _chttp_do_async_internal(raw, req, write_fn, write_ctx, &qrv);
  /* From here the queued chain keeps the client alive on its own (see
   * async_in_flight_count in struct chttpclient). */
  _chttpcli_resolve_unpin(raw);
  if (!f) return qrv;

  chttpcli_async_result_t *result = chttpclient_async_result_get(f);
  if (!result) {
    ctpool_future_free(f);
    return ccol_unexpected_failure;
  }

  ccol_retval_t rv = result->rv;
  if (result->resp) {
    /* A streaming request always builds a chttpcli_response internally with
     * body and headers left NULL (see _async_build_response), just so that
     * status_code has a way to travel through the one
     * chttpcli_async_result_t.resp field. This function's public contract
     * has no resp_out, exactly like chttpclient_do_streaming, so we unwrap
     * the response here, free it, and never show it to the caller. */
    if (rv == ccol_success && status_code_out) {
      *status_code_out = result->resp->status_code;
    }
    chttpclient_resp_free(result->resp);
  }
  chttpclient_async_result_free(result);
  ctpool_future_free(f);
  return rv;
}

/* ========================================================================== */
/*                         CLIENT CONSTRUCTORS                                */
/* ========================================================================== */

chttpcli chttpclient_create_mp(ccol_memmgmt_procs_t *mprocs, char **err_str) {
  if (mprocs && !_ccol_verify_memmgmt_procs(mprocs, err_str))
    return CHTTPCLI_INVALID;

  ccol_memmgmt_procs_t *mp = NULL;
  if (mprocs) {
    mp = (ccol_memmgmt_procs_t *)mprocs->malloc(sizeof(ccol_memmgmt_procs_t));
    if (!mp) {
      if (err_str) *err_str = _CCOL_ERR_STR("failed to allocate mprocs");
      return CHTTPCLI_INVALID;
    }
    memcpy(mp, mprocs, sizeof(ccol_memmgmt_procs_t));
  }

  struct chttpclient *cli =
      (struct chttpclient *)_ccol_mem_calloc(mp, 1, sizeof(struct chttpclient));
  if (!cli) {
    if (err_str) *err_str = _CCOL_ERR_STR("failed to allocate client");
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }

  cli->m_procs = mp;
  cli->tls = CHTTP_TLS_DEFAULT;
  if (_ccol_mutex_init(cli->lock) != 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("failed to initialize mutex");
    _ccol_mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }
  /* CLOCK_MONOTONIC, because _slot_acquire waits on this condition variable
   * through _ccol_cond_var_timedwait with a timespec from that clock, built
   * by _deadline_make. The comment of _ccol_cond_var_init_monotonic says
   * why the two clocks must match. */
  if (_ccol_cond_var_init_monotonic(&cli->available) != 0) {
    if (err_str)
      *err_str = _CCOL_ERR_STR("failed to initialize condition variable");
    _ccol_mutex_destroy(cli->lock);
    _ccol_mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }
  if (_ccol_cond_var_init(cli->idle_async_drained) != 0) {
    if (err_str)
      *err_str = _CCOL_ERR_STR("failed to initialize condition variable");
    _ccol_mutex_destroy(cli->lock);
    _ccol_cond_var_destroy(cli->available);
    _ccol_mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }
  if (_ccol_mutex_init(cli->async_count_lock) != 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("failed to initialize mutex");
    _ccol_mutex_destroy(cli->lock);
    _ccol_cond_var_destroy(cli->available);
    _ccol_cond_var_destroy(cli->idle_async_drained);
    _ccol_mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }
  if (_ccol_cond_var_init(cli->async_count_drained) != 0) {
    if (err_str)
      *err_str = _CCOL_ERR_STR("failed to initialize condition variable");
    _ccol_mutex_destroy(cli->lock);
    _ccol_cond_var_destroy(cli->available);
    _ccol_cond_var_destroy(cli->idle_async_drained);
    _ccol_mutex_destroy(cli->async_count_lock);
    _ccol_mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }

  char *herr = NULL;
  cli->idle_pools =
      chmap_create_full(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                        ccol_pointer, mp, NULL, NULL, &herr);
  if (!cli->idle_pools) {
    if (err_str) *err_str = _CCOL_ERR_STR("failed to allocate idle pool map");
    _ccol_mutex_destroy(cli->lock);
    _ccol_cond_var_destroy(cli->available);
    _ccol_cond_var_destroy(cli->idle_async_drained);
    _ccol_mutex_destroy(cli->async_count_lock);
    _ccol_cond_var_destroy(cli->async_count_drained);
    _ccol_mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }

  cli->idle_pools_async =
      chmap_create_full(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                        ccol_pointer, mp, NULL, NULL, &herr);
  if (!cli->idle_pools_async) {
    if (err_str)
      *err_str = _CCOL_ERR_STR("failed to allocate async idle pool map");
    __chmap_destroy(cli->idle_pools);
    _ccol_mutex_destroy(cli->lock);
    _ccol_cond_var_destroy(cli->available);
    _ccol_cond_var_destroy(cli->idle_async_drained);
    _ccol_mutex_destroy(cli->async_count_lock);
    _ccol_cond_var_destroy(cli->async_count_drained);
    _ccol_mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }

  if (_rebuild_tls_ctx_locked(cli) != ccol_success) {
    if (err_str)
      *err_str = _CCOL_ERR_STR("failed to build default TLS context");
    __chmap_destroy(cli->idle_pools);
    __chmap_destroy(cli->idle_pools_async);
    _ccol_mutex_destroy(cli->lock);
    _ccol_cond_var_destroy(cli->available);
    _ccol_cond_var_destroy(cli->idle_async_drained);
    _ccol_mutex_destroy(cli->async_count_lock);
    _ccol_cond_var_destroy(cli->async_count_drained);
    _ccol_mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }

  chttpcli h = _chttpcli_handle_slot_acquire(cli);
  if (h == 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("failed to allocate client slot");
    if (cli->tls_ctx) ctls_ctx_release(cli->tls_ctx);
    __chmap_destroy(cli->idle_pools);
    __chmap_destroy(cli->idle_pools_async);
    _ccol_mutex_destroy(cli->lock);
    _ccol_cond_var_destroy(cli->available);
    _ccol_cond_var_destroy(cli->idle_async_drained);
    _ccol_mutex_destroy(cli->async_count_lock);
    _ccol_cond_var_destroy(cli->async_count_drained);
    _ccol_mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }
  return h;
}

/* ========================================================================== */
/*                         CLIENT CONFIGURATION                               */
/* ========================================================================== */

ccol_retval_t chttpclient_set_pool_size(chttpcli h, size_t n) {
  struct chttpclient *cli = _chttpcli_resolve(h);
  if (!cli) return ccol_invalid_args;
  _ccol_mutex_lock(cli->lock);
  cli->configured_pool_size = n;
  cli->pool_cap = _resolve_pool_cap(n);
  cli->pool_initialized = true;
  _ccol_cond_var_broadcast(cli->available);
  _ccol_mutex_unlock(cli->lock);
  _chttpcli_resolve_unpin(cli);
  return ccol_success;
}

ccol_retval_t chttpclient_set_connect_timeout(chttpcli h, uint64_t us) {
  struct chttpclient *cli = _chttpcli_resolve(h);
  if (!cli) return ccol_invalid_args;
  _ccol_mutex_lock(cli->lock);
  cli->connect_timeout_us = us;
  _ccol_mutex_unlock(cli->lock);
  _chttpcli_resolve_unpin(cli);
  return ccol_success;
}

ccol_retval_t chttpclient_set_request_timeout(chttpcli h, uint64_t us) {
  struct chttpclient *cli = _chttpcli_resolve(h);
  if (!cli) return ccol_invalid_args;
  _ccol_mutex_lock(cli->lock);
  cli->request_timeout_us = us;
  _ccol_mutex_unlock(cli->lock);
  _chttpcli_resolve_unpin(cli);
  return ccol_success;
}

ccol_retval_t chttpclient_set_max_response_body_size(chttpcli h,
                                                     size_t max_bytes) {
  struct chttpclient *cli = _chttpcli_resolve(h);
  if (!cli) return ccol_invalid_args;
  _ccol_mutex_lock(cli->lock);
  cli->max_response_body_size = max_bytes;
  _ccol_mutex_unlock(cli->lock);
  _chttpcli_resolve_unpin(cli);
  return ccol_success;
}

ccol_retval_t chttpclient_set_tls(chttpcli h, const chttp_tls_config_t *tls) {
  if (!h) return ccol_invalid_args;
  /* A client certificate and its private key come as a pair, so having only
   * one of them is never a valid configuration. Without this check we'd read
   * that input as "no client certificate" and say nothing, since the
   * have_cert_pair check of _rebuild_tls_ctx_locked further down simply
   * wants both, and a mutual TLS deployment would believe it presents a
   * client certificate while it never does. The check looks only at the
   * caller's tls argument and never at cli, so it sits here before any
   * resolve and doesn't need a pin or an unpin of its own. */
  if (tls && ((tls->cert_path && !tls->key_path) ||
              (!tls->cert_path && tls->key_path)))
    return ccol_invalid_args;
  /* The only point of a CA bundle is to verify against it. Naming one and
   * turning verification off in the same struct states two policies that
   * can't both hold, and whichever one we picked would surprise half of the
   * callers who wrote it. Guessing the opt-out would also be the dangerous
   * direction, because a deployment that pinned a private CA would quietly
   * accept every peer. So we refuse the pair here, at the call that declares
   * the configuration, and not at the first request that would use it. Like
   * the cert/key check above it reads only the caller's argument, so it
   * needs no resolve and no pin. */
  if (tls && tls->insecure_skip_verify && tls->ca_bundle_path)
    return ccol_invalid_args;

  struct chttpclient *cli = _chttpcli_resolve(h);
  if (!cli) return ccol_invalid_args;

  _ccol_mutex_lock(cli->lock);

  _ccol_mem_free(cli->m_procs, cli->owned_cert_path);
  _ccol_mem_free(cli->m_procs, cli->owned_key_path);
  _ccol_mem_free(cli->m_procs, cli->owned_ca_bundle_path);
  cli->owned_cert_path = cli->owned_key_path = cli->owned_ca_bundle_path = NULL;

  if (!tls) {
    cli->tls = CHTTP_TLS_DEFAULT;
    ccol_retval_t rv = _rebuild_tls_ctx_locked(cli);
    _ccol_mutex_unlock(cli->lock);
    _chttpcli_resolve_unpin(cli); /* Exit 1 of 4. */
    return rv;
  }

  if (tls->cert_path) {
    cli->owned_cert_path = ccol_strdup(cli->m_procs, tls->cert_path);
    if (!cli->owned_cert_path) goto oom;
  }
  if (tls->key_path) {
    cli->owned_key_path = ccol_strdup(cli->m_procs, tls->key_path);
    if (!cli->owned_key_path) goto oom;
  }
  if (tls->ca_bundle_path) {
    cli->owned_ca_bundle_path = ccol_strdup(cli->m_procs, tls->ca_bundle_path);
    if (!cli->owned_ca_bundle_path) goto oom;
  }

  cli->tls = *tls;
  cli->tls.cert_path = cli->owned_cert_path;
  cli->tls.key_path = cli->owned_key_path;
  cli->tls.ca_bundle_path = cli->owned_ca_bundle_path;

  {
    ccol_retval_t rv = _rebuild_tls_ctx_locked(cli);
    _ccol_mutex_unlock(cli->lock);
    _chttpcli_resolve_unpin(cli); /* Exit 2 of 4. */
    return rv;
  }

oom:
  _ccol_mem_free(cli->m_procs, cli->owned_cert_path);
  _ccol_mem_free(cli->m_procs, cli->owned_key_path);
  _ccol_mem_free(cli->m_procs, cli->owned_ca_bundle_path);
  cli->owned_cert_path = cli->owned_key_path = cli->owned_ca_bundle_path = NULL;
  cli->tls = CHTTP_TLS_DEFAULT;
  /* At this point cli->tls_ctx and cli->tls_ctx_usable describe the
   * configuration that was in force before this call, built by an earlier
   * successful chttpclient_set_tls, while the code above has reset cli->tls
   * to the default. Leaving tls_ctx and tls_ctx_usable alone would split
   * what the client says it is configured with from how it really does TLS,
   * and nothing would report it: requests would keep using the OLD
   * certificate and CA material forever and cli->tls wouldn't show it.
   *
   * The rebuild puts tls_ctx back in step with the default configuration
   * that this call really leaves behind. We don't return its result, because
   * the failure the caller needs to hear about is the strdup above, not
   * whatever a rebuild of a plain default configuration allocates. */
  _rebuild_tls_ctx_locked(cli);
  _ccol_mutex_unlock(cli->lock);
  _chttpcli_resolve_unpin(cli); /* Exit 3 of 4, where all three failed strdup
                                   checks above end up. */
  return ccol_not_enough_memory;
}

/* ========================================================================== */
/*                         CLIENT DESTRUCTION                                 */
/* ========================================================================== */

void __chttpclient_destroy(chttpcli cli) {
  if (!cli) return;

  /* A defence: the doc comment of chttp_default_client() invites callers to
   * pass its handle to the chttpclient_set_* functions, and nothing stops
   * them from passing it here as well. So we first clear the singleton's own
   * copy of that handle.
   *
   * Without this, default_client_bundler.client keeps a handle that this
   * call is about to invalidate, and two things break. Every later
   * chttp_default_client(), chttp_do() or chttp_get() in the process hands
   * that dead handle straight back out (a use-after-destroy), because
   * default_client_bundler.once never fires again to build a new client.
   * And the process-exit destructor of this file destroys the client a
   * second time, which the generation-checked handles report as a fatal
   * double destroy rather than a quiet double free.
   *
   * For a client that chttpclient_create or chttpclient_create_mp made, this
   * does nothing: such a handle can never equal the singleton's, so the
   * compare-and-swap just fails. */
  chttpcli expected = cli;
  atomic_compare_exchange_strong(&default_client_bundler.client, &expected, 0);

  /* We resolve cli through the slot table and mark the slot not-in-use in
   * the same critical section as the lookup. That's what makes a second
   * destroy of the same handle value, concurrent or later, fail to resolve,
   * so it can never race this teardown. The file-level comment of the slot
   * table and the comment of _chttpcli_resolve have the full design.
   *
   * A stale or already destroyed handle is exactly the misuse that the
   * generation-checked handles are there to catch, so here it is fatal and
   * never a quiet use-after-free or double free. */
  _ccol_call_once(chttpcli_slot_table.once, _chttpcli_slot_table_init_globals);
  uint32_t idx = (uint32_t)(cli >> 32);
  uint32_t gen = (uint32_t)(cli & 0xFFFFFFFFu);
  _ccol_rw_lock_wrlock(chttpcli_slot_table.rwlock);
  chttpcli_slot_t *slot = NULL;
  struct chttpclient *raw = NULL;
  if (idx < cvector_elem_count(chttpcli_slot_table.slots)) {
    chttpcli_slot_t *s =
        (chttpcli_slot_t *)cvector_at(chttpcli_slot_table.slots, idx);
    if (s->in_use && s->generation == gen) {
      slot = s;
      raw = s->ptr;
    }
  }
  if (!raw) {
    _ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
    _ccol_fatal_err(
        "chttpclient_destroy: handle is stale or already destroyed "
        "(double-destroy / use-after-destroy of a chttpcli handle)");
  }
  if (_chttpcli_call_runs_on_this_thread(raw)) {
    /* The wait below would wait for the call that runs this destroy. */
    _ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
    _ccol_fatal_err(
        "chttpclient_destroy: called from inside a request of the same "
        "client (a write_fn of that client), which would wait for itself");
  }
  if (_chttpcli_runs_on_engine_thread()) {
    /* The waits below depend on the engine threads, since this client's
     * Tier 2 and Tier 3 requests run there and so does the teardown of its
     * idle connections. With one or two reactor threads, this thread is the
     * one that would have to run them and the destroy would never return.
     * We apply it to every client, so a write_fn that destroys a client
     * fails the same way whatever that client happens to be doing. */
    _ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
    _ccol_fatal_err(
        "chttpclient_destroy: called on a thread of the async engine (a "
        "write_fn of chttpclient_do_async_streaming or "
        "chttpclient_do_pooled_streaming); the destroy waits for work that "
        "the engine threads run, so destroy the client on another thread");
  }
  slot->in_use = false; /* From here on EVERY resolve of this handle fails,
                            a second concurrent destroy included. */
  /* Same step, same lock: from here we hand out no new pin, which is what
   * lets the count below reach zero and stay there. Both records of "is
   * this handle resolvable" flip together, so they can't disagree. */
  ccol_pintable_retire(&chttpcli_pintable, idx);
  _ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);

  /* The mark and the wake go BEFORE the pin drain, never after it. A caller
   * parked waiting for pool capacity holds a resolve pin while it sleeps, so
   * draining first would wait out the rest of that caller's request, while
   * the wake makes it return ccol_not_permitted at once. The occupant
   * that frees its pool slot would let that caller go eventually anyway, so
   * here the order only bounds how long a destroy takes and closes no
   * deadlock. In cthreadpool the same order does close a real one, since
   * its workers block on the pool's queue and only its shutdown can release
   * them. */
  _ccol_mutex_lock(raw->lock);
  raw->destroying = true;
  _ccol_cond_var_broadcast(raw->available);
  _ccol_mutex_unlock(raw->lock);

  /* Now wait out every in-flight caller that resolved before the retire
   * above. We poll instead of sleeping on a condition variable because the
   * unpin side sends no wakeup on purpose, its whole point being to touch
   * nothing but the pin. We hold no lock here either: a pinned caller often
   * needs raw->lock to finish its call and drop its pin, so waiting with
   * that lock held would deadlock against the very callers we wait for. */
  {
    long delay_ns = 1000;
    while (ccol_pintable_pins(&chttpcli_pintable, idx) > 0) {
      struct timespec ts = {.tv_sec = 0, .tv_nsec = delay_ns};
      nanosleep(&ts, NULL);
      if (delay_ns < 1000000L) delay_ns *= 2;
    }
  }

  /* in_flight_count is a separate gate with its own condition variable. A
   * caller adds to it while holding its pin and drops the pin only after
   * that, so the drain above can't have missed anyone. */
  _ccol_mutex_lock(raw->lock);
  while (raw->in_flight_count > 0)
    _ccol_cond_var_wait(raw->available, raw->lock);
  _ccol_mutex_unlock(raw->lock);

  /* Wait for every ACTIVE Tier 2 and Tier 3 async chain of this client,
   * meaning one that the idle pool doesn't hold yet. This wait comes before
   * everything else below.
   *
   * An in-flight chain can be connecting, handshaking, writing or
   * reading on a reactor thread or on a thread of the DNS and connect pool,
   * and it dereferences chain->cli and ctx->cli at almost any point (and
   * through them raw->lock, raw->idle_pools_async and raw->m_procs) until
   * the chain tears down or joins the idle pool that we drain further below.
   *
   * Without this wait, a caller that writes
   * `f = chttpclient_do_async(cli, req); chttpclient_destroy(cli);` frees raw
   * under a request that is in flight. The comment of this field on
   * struct chttpclient explains why it has its own lock and condition
   * variable instead of raw->lock and raw->available.
   *
   * It runs before the idle pool drain below, because an active chain
   * that completes during this wait is exactly what puts fresh entries into
   * that pool, and the drain then cleans them up. */
  _ccol_mutex_lock(raw->async_count_lock);
  while (raw->async_in_flight_count > 0)
    _ccol_cond_var_wait(raw->async_count_drained, raw->async_count_lock);
  _ccol_mutex_unlock(raw->async_count_lock);

  /* Every pool map walk below goes through the insertion-order cursor, which
   * allocates nothing and can't fail. If an allocation failure skipped a
   * walk we'd leak every pooled connection, or leave one that no shutdown
   * reaches and then wait for it for ever. */
  if (raw->idle_pools) {
    for (const ccol_chmap_entry_ref *e =
             ccol_chmap_oldest_entry(raw->idle_pools);
         e;) {
      const cmap_pair *k, *v;
      e = ccol_chmap_entry_read(e, &k, &v);
      cvec list = _read_cvec(v->ptr);
      if (list) {
        chttp_conn_t c;
        while (cvector_elem_count(list) > 0 &&
               cvector_pop_back(list, &c) == ccol_success)
          _conn_teardown(raw->m_procs, &c);
        __cvector_destroy(list);
      }
    }
    __chmap_destroy(raw->idle_pools);
  }

  /* The Tier 2 idle pool (see the "ASYNC IDLE POOL" section earlier in this
   * file). We shut down the fd of every connection the pool holds right
   * now, and once the reactor sees the fd, the IDLE-state dispatch of each
   * connection (_async_idle_ctx_finish) removes it from the pool and frees
   * it asynchronously, along with the engine reference it held.
   *
   * Then we wait for idle_total_count_async to reach zero before going on,
   * because the code below is about to free raw and those deferred
   * teardowns read raw->lock and raw->idle_pools_async.
   *
   * Calling shutdown() while we hold raw->lock is safe, and the deadline
   * sweep relies on the same thing: it runs no application-level callback
   * synchronously, so calling it from inside this loop has no reentrancy or
   * lock order hazard. That's why we use shutdown() here and don't close
   * the fd directly. */
  _ccol_mutex_lock(raw->lock);
  if (raw->idle_pools_async) {
    for (const ccol_chmap_entry_ref *e =
             ccol_chmap_oldest_entry(raw->idle_pools_async);
         e;) {
      const cmap_pair *k, *v;
      e = ccol_chmap_entry_read(e, &k, &v);
      cvec list = _read_cvec(v->ptr);
      if (!list) continue;
      size_t n = cvector_elem_count(list);
      for (size_t i = 0; i < n; i++) {
        chttp_async_ctx_t *actx = *(chttp_async_ctx_t **)cvector_at(list, i);
        int afd = actx->fd;
        if (afd >= 0) shutdown(afd, SHUT_RDWR);
      }
    }
  }
  /* idle_total_count_async gets to zero only once _async_idle_ctx_finish
   * has really removed and torn down every pooled connection, since the
   * subtraction happens strictly after _async_ctx_free returns (see that
   * function's comment). So it covers every stale candidate that the
   * shutdown sweep above is only now forcing an EOF or error dispatch for.
   *
   * A concurrent _async_idle_pool_take walk can leave a stale candidate that
   * has been shut down but not reaped yet (the first mechanism, described in
   * that function's comment). The count covers it too, because it is still
   * in the vector.
   *
   * The deferred teardowns of the second mechanism go through
   * pending_app_teardown (see that field's comment), and the wait on
   * async_in_flight_count above has already covered them. Such a ctx keeps
   * ctx->chain, and with it the chain's reference, until its own
   * _async_ctx_teardown really runs from a dispatch, so that earlier wait
   * can't have returned while one is pending. */
  while (raw->idle_total_count_async > 0)
    _ccol_cond_var_wait(raw->idle_async_drained, raw->lock);
  _ccol_mutex_unlock(raw->lock);

  if (raw->idle_pools_async) {
    for (const ccol_chmap_entry_ref *e =
             ccol_chmap_oldest_entry(raw->idle_pools_async);
         e;) {
      const cmap_pair *k, *v;
      e = ccol_chmap_entry_read(e, &k, &v);
      cvec list = _read_cvec(v->ptr);
      if (list) __cvector_destroy(list);
    }
    __chmap_destroy(raw->idle_pools_async);
  }

  if (raw->tls_ctx) ctls_ctx_release(raw->tls_ctx);

  _ccol_mutex_destroy(raw->lock);
  _ccol_cond_var_destroy(raw->available);
  _ccol_cond_var_destroy(raw->idle_async_drained);
  _ccol_mutex_destroy(raw->async_count_lock);
  _ccol_cond_var_destroy(raw->async_count_drained);

  ccol_memmgmt_procs_t *mp = raw->m_procs;
  _ccol_mem_free(mp, raw->owned_cert_path);
  _ccol_mem_free(mp, raw->owned_key_path);
  _ccol_mem_free(mp, raw->owned_ca_bundle_path);
  _ccol_mem_free(mp, raw);
  if (mp) mp->free(mp);

  /* The slot is freed last, after raw is fully torn down and freed, because
   * bumping the slot's generation and pushing the index onto the free list
   * is what marks the handle reusable, and no earlier step does that.
   *
   * We look the slot up again by idx instead of reusing `slot`: a concurrent
   * chttpclient_create_mp can run its own _chttpcli_handle_slot_acquire in
   * between and grow the slot array with cvector_push_back, which makes
   * every pointer into it from before this second lock stale. idx itself
   * stays valid. */
  _ccol_rw_lock_wrlock(chttpcli_slot_table.rwlock);
  chttpcli_slot_t *slot2 =
      (chttpcli_slot_t *)cvector_at(chttpcli_slot_table.slots, idx);
  slot2->ptr = NULL;
  slot2->generation++; /* Moves the slot's generation past the one in the
      handle of the client we just freed, so that stale handle can never
      match a LATER acquire of this same index. */
  _chttpcli_free_index_release_locked(idx);
  /* The process-exit destructor may have run already and found this client
     live. The release it couldn't do then falls to whoever frees the last
     slot, and that can be us. */
  _chttpcli_release_slot_table_if_deferred_locked();
  _ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
}

/* ========================================================================== */
/*                         REQUEST EXECUTION                                  */
/* ========================================================================== */

/*
 * A response that starts arriving while Tier 1 is sending the request.
 *
 * A server may answer before reading the whole request. A 401 from an auth
 * check that never reads the body and a 413 from a body size limit are the
 * everyday cases. Such a server often closes the connection or stops
 * reading once it has answered, so a client that only writes until the
 * request is out sees its write fail with EPIPE or ECONNRESET (or block for
 * good) and loses an answer that is already sitting in its receive buffer.
 * curl and Go's net/http read that answer, and every tier of this client
 * does too.
 *
 * So while it sends, Tier 1 also waits for readability and feeds whatever
 * arrives to a parser that lives across the send and the read after it. We
 * follow curl's rules:
 *   - An interim 1xx response is discarded, and the send goes on.
 *   - A final response of 300 or above stops the send once its status line
 *     and header block have arrived. The rest of the request is never sent.
 *   - A final response of 200 to 299 lets the send go on while it arrives,
 *     since a server may stream its answer while reading the request. Once
 *     that final response is complete the send stops, because nobody is
 *     waiting for the rest of the request any more.
 *   - A write that fails because the peer is gone (EPIPE, ECONNRESET,
 *     ENOTCONN, or a TLS close) stops the send, and the read then picks up
 *     whatever answer the peer left behind.
 * A connection whose response started before its request was fully sent is
 * never reused, whatever the response says about keep-alive. The peer may
 * not read the rest of the request, may be reading it, or may have
 * given up on the connection.
 */
typedef struct {
  chttp1_parser_t parser;
  bool parser_live;    /* The parser holds the start of a message. */
  bool done;           /* A final response is complete. */
  bool keep_alive;     /* The keep-alive answer of that complete response. */
  bool stop_send;      /* No further request byte goes out. */
  bool answered_early; /* The send took in the start of a response. */
  size_t n_discarded;
  ccol_retval_t error; /* ccol_success, or the failure of the parse. */
} chttp_early_resp_t;

/* Sets every field but the parser. The parser holds an 8 KiB line buffer and
 * is only read while parser_live is true, i.e. after _early_resp_start_parser
 * has initialized it. */
static void _early_resp_init(chttp_early_resp_t *e) {
  e->parser_live = false;
  e->done = false;
  e->keep_alive = false;
  e->stop_send = false;
  e->answered_early = false;
  e->n_discarded = 0;
  e->error = ccol_success;
}

static void _early_resp_release(chttp_early_resp_t *e) {
  if (e->parser_live) chttp1_parser_release(&e->parser);
  e->parser_live = false;
}

static void _early_resp_start_parser(chttp_early_resp_t *e,
                                     chttp_parse_ctx_t *pctx) {
  _ccol_call_once(client_http1_settings_bundler.once, _init_chttp1_settings);
  chttp1_parser_init(&e->parser, &client_http1_settings_bundler.settings);
  e->parser.data = pctx;
  (void)chttp1_parser_enable_line_spill(&e->parser, pctx->mp);
  e->parser_live = true;
}

/* Feeds data to e's parser following the rules above. An interim response
 * is dropped and parsing goes on with the bytes after it, the same way
 * _chttp_read_message_loop does it. */
static void _early_resp_feed(chttp_early_resp_t *e, chttp_parse_ctx_t *pctx,
                             const char *data, size_t len) {
  while (len > 0 && !e->done && e->error == ccol_success) {
    if (!e->parser_live) _early_resp_start_parser(e, pctx);
    chttp1_errno_t err = chttp1_parser_execute(&e->parser, data, len);
    if (err == CHTTP1_PAUSED) {
      size_t consumed = chttp1_parser_consumed(&e->parser);
      bool keep_alive = chttp1_should_keep_alive(&e->parser);
      chttp1_parser_release(&e->parser);
      e->parser_live = false;
      data += consumed;
      len -= consumed;
      if (pctx->status_code >= 100 && pctx->status_code < 200) {
        if (e->n_discarded >= CHTTP_MAX_INTERIM_RESPONSES) {
          e->error = ccol_http_transfer_aborted;
          break;
        }
        e->n_discarded++;
        ccol_retval_t rrv = _parse_ctx_reset_for_continue(pctx);
        if (rrv != ccol_success) e->error = rrv;
        continue;
      }
      e->done = true;
      e->keep_alive = keep_alive;
      if (len > 0) {
        /* Nothing legitimate follows a final response on a connection whose
         * request is going out. */
        pctx->trailing_garbage = true;
        e->keep_alive = false;
      }
      break;
    }
    if (err == CHTTP1_USER) {
      e->error = pctx->too_large ? ccol_msg_too_large
                                 : (pctx->error ? ccol_not_enough_memory
                                                : ccol_http_transfer_aborted);
      break;
    }
    if (err != CHTTP1_OK) e->error = ccol_http_transfer_aborted;
    break;
  }
  if (e->done || e->error != ccol_success || pctx->status_code >= 300)
    e->stop_send = true;
}

/* The peer closed its side while the request was going out. A message the
 * parser holds may end at that close, and if it holds none there is no
 * answer at all. */
static void _early_resp_eof(chttp_early_resp_t *e, chttp_parse_ctx_t *pctx) {
  e->stop_send = true;
  if (!e->parser_live || e->done) return;
  chttp1_errno_t fe = chttp1_parser_finish(&e->parser);
  chttp1_parser_release(&e->parser);
  e->parser_live = false;
  if ((fe == CHTTP1_OK || fe == CHTTP1_PAUSED) && pctx->message_complete &&
      !(pctx->status_code >= 100 && pctx->status_code < 200)) {
    e->done = true;
    e->keep_alive = false;
  } else {
    e->error = ccol_http_transfer_aborted;
  }
}

/* Sends data[0..len) like _chttp_send_all, while also taking in a response
 * that shows up in the meantime (see chttp_early_resp_t). Returns
 * ccol_success when the send completes or the rules above stop it, and the
 * failure code otherwise. *sent_out gets the number of bytes that went
 * out. */
static ccol_retval_t _chttp_send_all_watch(
    chttp_conn_t *conn, const char *data, size_t len, chttp_deadline_t *overall,
    chttp_parse_ctx_t *pctx, chttp_early_resp_t *e, bool *any_bytes_read_out,
    size_t *sent_out) {
  size_t sent = 0;
  *sent_out = 0;
  /* _chttp_send_all explains why we work out the wait direction again after
   * a TLS write returns EWOULDBLOCK. */
  short want_events = POLLOUT;
  char buf[8192];
  /* True after a read that returned EWOULDBLOCK, false after one that got
   * bytes or saw the end of the stream (see below). */
  bool read_blocked = false;
  while (sent < len && !e->stop_send) {
    int wait_ms;
    if (!_deadline_remaining_ms(overall, &wait_ms)) return ccol_timed_out;
    /* poll(2) can't see plaintext that the TLS layer already holds, so we
     * read first while ctls_conn_has_pending_input reports input. But that
     * report is also true for the start of a record that isn't complete
     * yet, and no read can take it until the rest reaches the socket. So
     * after a read that blocks we stop trusting the report until a read
     * makes progress again, and the poll below waits on the socket, the
     * same as in _chttp_read_message_with. Otherwise a peer that sends half
     * a record and stops reading turns this loop into a spin: the report
     * stays true, every read blocks, and the write never gets to the poll
     * that would wait for room. */
    bool readable =
        conn->tls && !read_blocked && ctls_conn_has_pending_input(conn->tls);
    bool writable = false;
    if (!readable) {
#ifdef RUNNING_UNIT_TESTS
      if (_tls_dir_note_wait((want_events & POLLOUT) != 0)) {
        writable = true;
      } else
#endif
      {
        struct pollfd pfd = {.fd = conn->fd,
                             .events = (short)(want_events | POLLIN),
                             .revents = 0};
        int rc = poll(&pfd, 1, wait_ms);
        if (rc < 0) {
          if (errno == EINTR) continue;
          return ccol_http_transfer_aborted;
        }
        if (rc == 0) continue; /* The deadline check above ends the wait. */
        if (pfd.revents & POLLNVAL) return ccol_http_transfer_aborted;
        readable = (pfd.revents & POLLIN) != 0;
        /* On an error or a hang-up the next write fails, and that failure
         * is what stops the send. */
        writable = (pfd.revents & (POLLOUT | POLLERR | POLLHUP)) != 0 ||
                   (readable && want_events == POLLIN);
      }
    }

    if (readable) {
      ssize_t n = _conn_read(conn, buf, sizeof(buf));
      if (n > 0) {
        read_blocked = false;
        *any_bytes_read_out = true;
        _early_resp_feed(e, pctx, buf, (size_t)n);
        e->answered_early = e->done || e->parser_live;
        continue;
      }
      if (n == 0) {
        read_blocked = false;
        _early_resp_eof(e, pctx);
        continue;
      }
      if (errno != EWOULDBLOCK) {
        /* The connection is gone; the read after the send will report it. */
        e->stop_send = true;
        continue;
      }
      /* Either a TLS record with no application data (a session ticket, for
       * example) or part of a record, so the write carries on. */
      read_blocked = true;
      if (!writable) continue;
    }

    ssize_t n = _conn_write(conn, data + sent, len - sent);
    if (n < 0) {
      if (errno == EWOULDBLOCK) {
        if (conn->tls)
          want_events = _conn_tls_wants_write(conn->tls) ? POLLOUT : POLLIN;
        continue;
      }
      if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
        /* The peer is gone, but it may have answered first. */
        e->stop_send = true;
        continue;
      }
      return ccol_http_transfer_aborted;
    }
    if (n == 0) {
      /* The TLS peer closed, but it may have answered first. */
      e->stop_send = true;
      continue;
    }
    sent += (size_t)n;
    *sent_out = sent;
    want_events = POLLOUT;
  }
  if (e->error != ccol_success) return e->error;
  return ccol_success;
}

/* Reads the rest of the response once _chttp_send_all_watch is done, either
 * the message its parser already holds or the next one. request_complete
 * tells whether every byte of the request went out before a response
 * started, and when it's false the connection is never reused. */
static ccol_retval_t _chttp_early_resp_finish(
    chttp_conn_t *conn, chttp_parse_ctx_t *pctx, chttp_deadline_t *overall,
    chttp_early_resp_t *e, bool request_complete, bool *keep_alive_out,
    bool *any_bytes_read_out) {
  ccol_retval_t rv;
  if (e->done) {
    *keep_alive_out = e->keep_alive;
    rv = ccol_success;
  } else if (e->parser_live) {
    char *leftover = NULL;
    size_t leftover_len = 0;
    rv = _chttp_read_message_with(&e->parser, conn, pctx, overall, NULL, NULL,
                                  0, keep_alive_out, any_bytes_read_out,
                                  &leftover, &leftover_len);
    chttp1_parser_release(&e->parser);
    e->parser_live = false;
    if (rv == ccol_success && pctx->status_code >= 100 &&
        pctx->status_code < 200) {
      /* The message the send saw starting was an interim one, so the final
       * response comes after it, and the interim budget counts the whole
       * sequence of messages. */
      if (e->n_discarded >= CHTTP_MAX_INTERIM_RESPONSES) {
        rv = ccol_http_transfer_aborted;
      } else {
        rv = _parse_ctx_reset_for_continue(pctx);
      }
      if (rv == ccol_success) {
        char *next_leftover = NULL;
        size_t next_leftover_len = 0;
        rv = _chttp_read_message_loop(conn, pctx, overall, NULL, leftover,
                                      leftover_len, 0, e->n_discarded + 1,
                                      keep_alive_out, any_bytes_read_out,
                                      &next_leftover, &next_leftover_len);
        _ccol_mem_free(pctx->mp, leftover);
        leftover = next_leftover;
        leftover_len = next_leftover_len;
      }
    }
    if (rv == ccol_success && leftover_len > 0) {
      pctx->trailing_garbage = true;
      *keep_alive_out = false;
    }
    _ccol_mem_free(pctx->mp, leftover);
  } else {
    char *leftover = NULL;
    size_t leftover_len = 0;
    rv = _chttp_read_message_loop(conn, pctx, overall, NULL, NULL, 0, 0,
                                  e->n_discarded, keep_alive_out,
                                  any_bytes_read_out, &leftover, &leftover_len);
    if (rv == ccol_success && leftover_len > 0) {
      pctx->trailing_garbage = true;
      *keep_alive_out = false;
    }
    _ccol_mem_free(pctx->mp, leftover);
  }
  if (rv == ccol_success && !request_complete) *keep_alive_out = false;
  return rv;
}

/*
 * Sends `wire`, the fully serialized request of `wire_len` bytes, and reads
 * the response. If there is a body, `wire` carries it as is at the end, and
 * it is exactly `body_len` bytes long.
 *
 * chttp_request_t.expect_continue is honoured when `use_100_continue` is
 * true, which means the caller has already checked that this hop really has
 * a body to hold back (body_carrying_method && body.data && body.len > 0,
 * the same condition _serialize_request uses to write the
 * "expect: 100-continue" header).
 *
 * Without 100-continue it's one send followed by one read.
 *
 * With 100-continue we send only the header part first and then wait up to
 * CHTTP_100_CONTINUE_WAIT_US, cut short by whatever is left of `overall`,
 * for the first byte of a response. The window bounds only the wait for the
 * first byte of each message. A message that starts inside it is read to
 * its end under `overall` alone, and a complete interim message other than
 * "100 Continue" leaves the window running for the next one. The wait ends
 * in one of three ways:
 *   - A "100 Continue" interim response. We reset pctx with a fresh header
 *     map (the usual "fresh state for each message" convention here), since
 *     the interim headers must never leak into the final response. Then we
 *     send the body and read the real final response, carrying over any
 *     bytes that a fast server already sent past the end of the interim
 *     message in the same read (the doc comment of _chttp_read_message says
 *     why that can happen and why we must not drop those bytes as garbage).
 *   - The server answers with no "100 Continue" at all, which RFC 7231
 *     SS5.1.1 explicitly allows, for example to reject a request without
 *     wanting its body. That response IS the final one, and the body is
 *     never sent.
 *   - The window runs out before any response byte arrives. We send the body
 *     anyway and read the final response as usual, the same as curl's
 *     CURLOPT_EXPECT_100_TIMEOUT_MS. A final response that is coming in
 *     when the window ends is the direct answer of the previous case, and
 *     the body is never sent.
 *
 * The wait runs through _chttp_read_message_loop with stop_at_status = 100,
 * because a single _chttp_read_message call isn't enough. A server may
 * legitimately send some OTHER interim 1xx, such as a "103 Early Hints" from
 * RFC 8297, before a "100 Continue" or before its final answer, and we have
 * to drop it and keep waiting inside the same window instead of taking it
 * as the final response.
 *
 * *retry_unsafe_out becomes true in two cases. One is a request that isn't
 * replayable (replayable is false, see _request_is_replayable) once all of
 * its bytes went out. The other is the moment this hop puts the body on the
 * wire after an explicit "100 Continue" from the server on THIS connection,
 * whatever the method. For a replayable request it stays false in every
 * other case, the branch that times out and sends the body anyway included,
 * because that branch got no confirmation at all.
 *
 * The retry-once safety net that chttp_do_internal has for a reused
 * connection assumes the connection MIGHT have died before this hop wrote a
 * single byte to it (see that function's comment). An explicit "100
 * Continue" disproves that outright for this connection, so a later read
 * failure with zero bytes of a final response can't be read as "nothing
 * went out, a retry is free". We already gave the body to a peer that
 * proved a moment ago that it was alive and willing to take it, and sending
 * it again blindly on a second, unrelated connection risks a server running
 * a non-idempotent request twice.
 */
static ccol_retval_t _chttp_send_and_read(
    chttp_conn_t *conn, const char *wire, size_t wire_len, size_t body_len,
    bool use_100_continue, bool replayable, chttp_deadline_t *overall,
    chttp_parse_ctx_t *pctx, bool *keep_alive_out, bool *any_bytes_read_out,
    bool *retry_unsafe_out) {
  *retry_unsafe_out = false;
  chttp_early_resp_t early;
  _early_resp_init(&early);
  size_t sent = 0;
  ccol_retval_t prv;
  if (!use_100_continue) {
    prv = _chttp_send_all_watch(conn, wire, wire_len, overall, pctx, &early,
                                any_bytes_read_out, &sent);
    /* See _request_is_replayable. Once all bytes of a non-replayable request
     * are out, the server may already have acted on it. */
    if (!replayable && sent == wire_len) *retry_unsafe_out = true;
    if (prv == ccol_success)
      prv = _chttp_early_resp_finish(conn, pctx, overall, &early,
                                     !early.answered_early && sent == wire_len,
                                     keep_alive_out, any_bytes_read_out);
    _early_resp_release(&early);
    return prv;
  }

  size_t header_len = wire_len - body_len;
  prv = _chttp_send_all(conn, wire, header_len, overall);
  if (prv != ccol_success) return prv;

  chttp_deadline_t continue_dl = _deadline_make(CHTTP_100_CONTINUE_WAIT_US);
  chttp_deadline_t wait_dl = _deadline_earlier(continue_dl, *overall);

  char *leftover = NULL;
  size_t leftover_len = 0;
  prv = _chttp_read_message_loop(conn, pctx, overall, &wait_dl, NULL, 0, 100, 0,
                                 keep_alive_out, any_bytes_read_out, &leftover,
                                 &leftover_len);
  if (prv == ccol_timed_out) {
    /* The timeout can come from either of two deadlines. Once a byte of a
     * message has reached the parser only `overall` bounds the read, and
     * its expiry ends the request here, so we never send the body after
     * part of the answer has arrived. */
    int overall_ms;
    if (!_deadline_remaining_ms(overall, &overall_ms)) return ccol_timed_out;
    /* The window ran out before any byte of a message came in, so pctx is
     * clean: the read only touches it once a byte reaches the parser, and
     * the loop resets it after each interim message it drops. We send the
     * body anyway and take in a response that starts while the body is
     * going out, just like the case without a continue.
     *
     * *any_bytes_read_out keeps the value the wait left. It's true only
     * when the loop dropped a complete interim response, which proves the
     * server got this request and started answering, so quietly retrying
     * the whole request on a fresh connection isn't safe (see
     * _chttp_read_message_loop). */
    prv = _chttp_send_all_watch(conn, wire + header_len, body_len, overall,
                                pctx, &early, any_bytes_read_out, &sent);
    if (!replayable && sent == body_len) *retry_unsafe_out = true;
    if (prv == ccol_success)
      prv = _chttp_early_resp_finish(conn, pctx, overall, &early,
                                     !early.answered_early && sent == body_len,
                                     keep_alive_out, any_bytes_read_out);
    _early_resp_release(&early);
    return prv;
  }
  if (prv != ccol_success) {
    _ccol_mem_free(pctx->mp, leftover);
    return prv;
  }

  if (pctx->status_code != 100) {
    /* The server answered directly, so this already IS the final response
     * and the body must never be sent.
     *
     * The connection is never safe to reuse, whatever
     * chttp1_should_keep_alive() made of the response's Connection header.
     * RFC 7231 SS5.1.1 only says a server SHOULD close the connection after
     * rejecting a request this way and doesn't REQUIRE it, so a server that
     * follows the RFC can keep the connection open while it expects
     * the body this hop never sent.
     *
     * If we pooled it, the next unrelated request on this client would
     * write its bytes onto it, the server would parse them as the rest of
     * this hop's body, and the two requests would get out of step on one
     * shared connection.
     *
     * Bytes after the end of the response are real trailing garbage too,
     * since nothing legitimate can follow a final response on a connection
     * whose body we never sent. We only track them for diagnostics. */
    *keep_alive_out = false;
    if (leftover_len > 0) pctx->trailing_garbage = true;
    _ccol_mem_free(pctx->mp, leftover);
    return ccol_success;
  }

  prv = _parse_ctx_reset_for_continue(pctx);
  if (prv != ccol_success) {
    _ccol_mem_free(pctx->mp, leftover);
    return prv;
  }

  /* We're about to give the body to a connection that the server itself
   * just confirmed, with a "100 Continue", as alive and reading (see this
   * function's doc comment). From the first body byte on, whatever happens
   * next, that confirmation rules out the caller's usual retry-once safety
   * net for a reused connection. */
  *retry_unsafe_out = true;
  *any_bytes_read_out = false;
  /* Bytes that came in with the "100 Continue" are the start of the final
   * response, so they go to the same parser that takes whatever arrives
   * while the body is going out. */
  if (leftover_len > 0) {
    *any_bytes_read_out = true;
    _early_resp_feed(&early, pctx, leftover, leftover_len);
  }
  _ccol_mem_free(pctx->mp, leftover);
  prv = early.error;
  if (prv == ccol_success)
    prv = _chttp_send_all_watch(conn, wire + header_len, body_len, overall,
                                pctx, &early, any_bytes_read_out, &sent);
  if (prv == ccol_success)
    prv = _chttp_early_resp_finish(conn, pctx, overall, &early,
                                   !early.answered_early && sent == body_len,
                                   keep_alive_out, any_bytes_read_out);
  _early_resp_release(&early);
  return prv;
}

static ccol_retval_t chttp_do_internal(
    struct chttpclient *cli, const chttp_request_t *req, bool streaming,
    chttpcli_write_fn user_write_fn, void *user_write_ctx,
    chttpcli_response **resp_out, int *status_code_out) {
  /* resp_out is checked and *resp_out set to NULL FIRST, before the cli and
   * req checks below, so that a failure from a NULL cli, req or
   * user_write_fn ALSO leaves *resp_out reliably NULL, not just the failures
   * after this point.
   *
   * All the later failure paths of this function (early returns and breaks)
   * then leave *resp_out alone, and that's fine because it's already NULL
   * rather than garbage from the caller's uninitialized local.
   *
   * chttpclient_resp_free() is documented as safe to call with NULL, which
   * allows an unconditional cleanup like
   * `resp = NULL; rv = chttpclient_do(...); ...;
   * chttpclient_resp_free(resp);`. The Tier 3 chttpclient_do_pooled and
   * chttpclient_do_pooled_streaming rely on exactly this, and Tier 1 must
   * behave the same way. */
  if (!streaming) {
    if (!resp_out) return ccol_invalid_args;
    *resp_out = NULL;
  }
  if (!cli || !req) return ccol_invalid_args;
  if (streaming && !user_write_fn) return ccol_invalid_args;

  /* We read request_timeout_us and anchor the overall deadline BEFORE taking
   * a slot of the concurrency limiter below. The documented contract of
   * chttpclient_set_request_timeout is "the maximum time from when
   * chttpclient_do is called...", not from when a pool slot frees up, so
   * time spent blocked on a full pool (sized by chttpclient_set_pool_size)
   * counts against the deadline as well. That's the reason
   * _slot_acquire knows about the deadline (see its comment). */
  uint64_t request_timeout_us;
  _ccol_mutex_lock(cli->lock);
  request_timeout_us = cli->request_timeout_us;
  _ccol_mutex_unlock(cli->lock);
  chttp_deadline_t overall_dl = _deadline_make(request_timeout_us);

  ccol_retval_t rv = _slot_acquire(cli, &overall_dl);
  if (rv != ccol_success) return rv;

  ccol_memmgmt_procs_t *mp = cli->m_procs;

  uint64_t connect_timeout_us;
  bool verify_host;
  ctls_ctx_t *tls_ctx;
  bool tls_ctx_usable;
  size_t max_response_body_size;
  /* Snapshotted together with tls_ctx under the same lock, so that both
   * describe the same configuration. Every connection this request opens
   * gets stamped with it (see _conn_open), and we compare it with the
   * client's current value before reusing a pooled connection. */
  uint64_t tls_generation;
  _ccol_mutex_lock(cli->lock);
  connect_timeout_us = cli->connect_timeout_us;
  tls_ctx = cli->tls_ctx;
  tls_ctx_usable = cli->tls_ctx_usable;
  tls_generation = cli->tls_generation;
  max_response_body_size = cli->max_response_body_size;
  /* Matching the hostname means nothing once the chain check is skipped, so
   * skipping the chain turns the hostname check off too, the same way Tier 2
   * works it out in _chttp_do_async_internal. */
  verify_host =
      !cli->tls.insecure_skip_verify && !cli->tls.insecure_skip_hostname_check;
  if (tls_ctx)
    ctls_ctx_retain(tls_ctx); /* Pin the context so that a concurrent
                                 chttpclient_set_tls can't free it while
                                 this request is using it. */
  _ccol_mutex_unlock(cli->lock);

  char *cur_url = ccol_strdup(mp, req->url);
  if (!cur_url) {
    if (tls_ctx) ctls_ctx_release(tls_ctx);
    _slot_release(cli);
    return ccol_not_enough_memory;
  }

  chttp_request_body_t empty_body = CHTTP_NO_BODY;
  chttp_method_t cur_method = req->method;
  chttp_request_body_t cur_body = req->body;

  /* An Authorization value we inject from the userinfo of a URL. It goes
   * along from hop to hop while the origin (scheme, host and port) stays the
   * same, and we drop it for good the first time the origin changes. That's
   * curl's default behaviour, without its opt-in for forwarding credentials
   * to a trusted redirect. Once dropped it never comes back, even if a later
   * hop returns to the original origin. */
  char *carried_auth = NULL;
  char *carried_auth_origin = NULL;

  /* Unlike carried_auth above, the credential headers and the Host header
   * that the caller sets with chttp_request_set_header have no origin of
   * their own. So we leave them off every hop whose origin (or host) differs
   * from the one the ORIGINAL request named (see
   * CHTTP_REDIRECT_DROP_CREDENTIALS).
   *
   * initial_origin_key is captured once from the URL of hop 0, and
   * redirect_drops holds the CHTTP_REDIRECT_DROP_* flags of the current hop
   * against it, recomputed on every hop. How this plays with the per-hop
   * userinfo case above is in the redirect_drops parameter of
   * _serialize_request and in the way it adjusts has_auth and has_host. */
  char *initial_origin_key = NULL;
  unsigned redirect_drops = 0;

  ccol_retval_t result = ccol_http_too_many_redirects;

  for (int hop = 0; hop <= CHTTP_MAX_REDIRECTS; hop++) {
    chttp_url_t url;
    ccol_retval_t prv = _parse_chttp_url(mp, cur_url, &url);
    if (prv != ccol_success) {
      result = prv;
      break;
    }

    if (!initial_origin_key) {
      initial_origin_key = ccol_strdup(mp, url.origin_key);
      if (!initial_origin_key) {
        _url_free(mp, &url);
        result = ccol_not_enough_memory;
        break;
      }
    } else {
      redirect_drops =
          _redirect_header_drops(initial_origin_key, url.origin_key);
    }

    if (url.is_https && !tls_ctx_usable) {
      /* At the set_tls call the configured cert, key or CA path couldn't be
       * read, or ctls failed to load or parse it, and instead of stopping
       * the process we report that failure here (see
       * _rebuild_tls_ctx_locked).
       *
       * It's ccol_http_tls_cert_load_failed and not
       * ccol_http_tls_handshake_failed because no handshake ever ran, and
       * ctls_ctx_cert_add and ctls_ctx_trust in ctls.c already report this
       * exact failure with this exact code (see ctls.h). Reusing it instead
       * of a generic handshake failure lets a caller tell apart "my local
       * cert, key or CA file is bad", "the peer failed the handshake" and
       * "the peer's certificate did not verify". */
      _url_free(mp, &url);
      result = ccol_http_tls_cert_load_failed;
      break;
    }

    const char *effective_auth = NULL;
    if (url.userinfo_authorization) {
      effective_auth = url.userinfo_authorization;
    } else if (carried_auth &&
               _origin_keys_same(carried_auth_origin, url.origin_key)) {
      effective_auth = carried_auth;
    }
    if (url.userinfo_authorization) {
      char *na = ccol_strdup(mp, url.userinfo_authorization);
      char *no = ccol_strdup(mp, url.origin_key);
      if (!na || !no) {
        _ccol_mem_free(mp, na);
        _ccol_mem_free(mp, no);
        _url_free(mp, &url);
        result = ccol_not_enough_memory;
        break;
      }
      _ccol_mem_free(mp, carried_auth);
      _ccol_mem_free(mp, carried_auth_origin);
      carried_auth = na;
      carried_auth_origin = no;
    } else if (carried_auth &&
               !_origin_keys_same(carried_auth_origin, url.origin_key)) {
      _ccol_mem_free(mp, carried_auth);
      _ccol_mem_free(mp, carried_auth_origin);
      carried_auth = NULL;
      carried_auth_origin = NULL;
    }

    chttp_request_t hop_req = *req;
    hop_req.method = cur_method;
    hop_req.body = cur_body;

    char *wire = NULL;
    size_t wire_len = 0;
    chttp_header_presence_t hop_hp = {0};
    prv = _serialize_request(mp, &hop_req, &url, effective_auth, redirect_drops,
                             &wire, &wire_len, &hop_hp);
    if (prv != ccol_success) {
      _url_free(mp, &url);
      result = prv;
      break;
    }

    chttp_deadline_t connect_dl = _deadline_make(connect_timeout_us);

    chttp_conn_t conn;
    bool reused = _idle_pool_take(cli, url.origin_key, &conn);
    if (!reused) {
      prv = _conn_open(mp, &url, url.is_https, tls_ctx, verify_host,
                       tls_generation, &connect_dl, &overall_dl, &conn);
      if (prv != ccol_success) {
        _ccol_mem_free(mp, wire);
        _url_free(mp, &url);
        result = prv;
        break;
      }
    }

    chttp_parse_ctx_t pctx;
    memset(&pctx, 0, sizeof(pctx));
    pctx.mp = mp;
    pctx.is_head_request = (cur_method == CHTTP_HEAD);

    chttp_bodybuf_t bb;
    memset(&bb, 0, sizeof(bb));
    bb.mp = mp;
    bb.max_size = max_response_body_size;
    pctx.requested_sink_fn = streaming ? user_write_fn : _sink_buffered;
    pctx.requested_sink_ctx = streaming ? user_write_ctx : &bb;

    char *herr = NULL;
    pctx.headers =
        chmap_create_full(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                          ccol_string, mp, NULL, NULL, &herr);
    if (!pctx.headers) {
      _ccol_mem_free(mp, wire);
      _conn_teardown(mp, &conn);
      _url_free(mp, &url);
      result = ccol_not_enough_memory;
      break;
    }

    bool body_carrying_method =
        (cur_method == CHTTP_POST || cur_method == CHTTP_PUT ||
         cur_method == CHTTP_PATCH);
    /* Same check as in _serialize_request, which writes an
     * "expect: 100-continue" header only when the caller hasn't set an
     * explicit Expect header of their own (what
     * chttp_header_presence_t.has_expect reports).
     *
     * Without it, a caller could set req->expect_continue together with an
     * Expect header whose value isn't "100-continue". No "expect:" line
     * would reach the wire, which is right, but this hop would stall
     * for CHTTP_100_CONTINUE_WAIT_US waiting for an interim response that
     * can never come.
     *
     * We reuse hop_hp, which _serialize_request computed over this same
     * header map a few lines up, instead of scanning req->headers again. */
    bool has_explicit_expect = hop_hp.has_expect;
    bool use_100_continue = req->expect_continue && !has_explicit_expect &&
                            body_carrying_method && cur_body.data &&
                            cur_body.len > 0;

    bool keep_alive = false;
    bool any_bytes_read = false;
    bool retry_unsafe = false;
    bool replayable = _request_is_replayable(cur_method, &hop_hp);
    prv = _chttp_send_and_read(&conn, wire, wire_len, cur_body.len,
                               use_100_continue, replayable, &overall_dl, &pctx,
                               &keep_alive, &any_bytes_read, &retry_unsafe);
    if (prv != ccol_success && reused && !any_bytes_read && !retry_unsafe) {
      /* The reused connection may have died between the liveness probe and
       * this attempt. Either the write quietly landed in the local send
       * buffer before the peer's close became visible, or the read got not
       * a single byte. In both cases nothing has been parsed or handed to
       * the caller yet, so one retry on a brand-new connection is safe.
       *
       * retry_unsafe overrides this in two cases, both meaning the server
       * may already have acted on the request: every byte of a request that
       * isn't replayable went out (see _request_is_replayable), or the body
       * went out after an explicit "100 Continue" showed that the connection
       * and the server were alive a moment earlier (see the doc comment of
       * _chttp_send_and_read). Then the request fails rather than reaching
       * the server twice. */
      _conn_teardown(mp, &conn);
      /* A fresh connect deadline for this retry's new connection, NOT the
       * hop-level `connect_dl` above. That one only covers the fresh-connect
       * branch for `!reused`, and nothing has spent it against the clock
       * before this point.
       *
       * The failed attempt on the dead reused connection just above went
       * through _chttp_send_and_read, bounded only by `overall_dl`, and it
       * can really take a while to fail, e.g. against a half-open or
       * blackholed peer that says nothing until part of overall_dl is gone.
       *
       * Without a fresh deadline that unrelated time would quietly eat into
       * the connect_timeout_us budget of the new connection, and the
       * retry's connect would time out (or get far less than
       * connect_timeout_us) although the caller configured the full budget.
       * _async_retry_hop does the same fresh connect deadline for this case
       * in Tier 2 and Tier 3 (see that function's connect_deadline field
       * comment). */
      chttp_deadline_t retry_connect_dl = _deadline_make(connect_timeout_us);
      prv = _conn_open(mp, &url, url.is_https, tls_ctx, verify_host,
                       tls_generation, &retry_connect_dl, &overall_dl, &conn);
      if (prv == ccol_success) {
        reused = false;
        prv = _chttp_send_and_read(
            &conn, wire, wire_len, cur_body.len, use_100_continue, replayable,
            &overall_dl, &pctx, &keep_alive, &any_bytes_read, &retry_unsafe);
      }
    }
    _ccol_mem_free(mp, wire);
    if (prv != ccol_success) {
      _conn_teardown(mp, &conn);
      _parse_ctx_free_fields(&pctx);
      _ccol_mem_free(mp, bb.buf);
      _url_free(mp, &url);
      result = prv;
      break;
    }

    /* If the request's Connection header asked for "close", we're done with
     * the connection whatever the response says (RFC 9112 section 9.6). */
    bool reusable =
        keep_alive && !pctx.trailing_garbage && !hop_hp.request_close;
    if (reusable) {
      _idle_pool_offer(cli, &conn);
    } else {
      _conn_teardown(mp, &conn);
    }

    if (pctx.will_redirect) {
      if (hop >= CHTTP_MAX_REDIRECTS) {
        /* This response belongs to the 51st request of the chain and is
         * ALSO a redirect, so following it would go past the budget of 50
         * redirects. We return an error rather than quietly handing over
         * the response, because a caller who relies on
         * ccol_http_too_many_redirects to spot a redirect loop needs a real
         * error, not a stale 3xx that it has to notice and inspect itself. */
        _parse_ctx_free_fields(&pctx);
        _ccol_mem_free(mp, bb.buf);
        _url_free(mp, &url);
        result = ccol_http_too_many_redirects;
        break;
      }
      ccol_retval_t resolve_rv = ccol_success;
      char *next_url =
          _resolve_redirect_url(mp, &url, pctx.location, &resolve_rv);
      /* See _redirect_transport_allowed. A refused target comes back as
       * ccol_http_invalid_url, the same code an unsupported scheme in the
       * original request URL gets, and the code below tells it apart from a
       * Location that doesn't resolve (next_url is NULL then). We check it
       * here while `url` describes the hop this Location came in on,
       * since the target needs to be allowed against that hop. */
      bool transport_refused = false;
      ccol_retval_t transport_rv =
          next_url
              ? _redirect_transport_allowed(
                    mp, &url, next_url, req->prevent_tls_downgrade_on_redirect)
              : ccol_success;
      if (transport_rv != ccol_success) {
        _ccol_mem_free(mp, next_url);
        next_url = NULL;
        transport_refused = true;
      }
      bool preserve = (pctx.status_code == 307 || pctx.status_code == 308);
      _parse_ctx_free_fields(&pctx);
      _ccol_mem_free(mp, bb.buf);
      _url_free(mp, &url);
      _ccol_mem_free(mp, cur_url);
      /* Set the pointer to NULL right after the free, not only when it's
       * reassigned on the success path below.
       *
       * Besides an allocation failure, a redirect can carry an empty
       * Location header or one that doesn't resolve for another reason, a
       * failed RFC 3986 SS5.2 and SS5.3 resolution for example. The very
       * first check of _resolve_redirect_url is
       * `if (!location || !*location) return NULL;`, and an ordinary server
       * response gets there easily without any malformed input.
       *
       * next_url is NULL then, and we fall through to the
       * _ccol_mem_free(mp, cur_url) of the cleanup after the loop below.
       * Without this assignment the pointer would hold the value we
       * just freed, a real double free that a remote peer can trigger.
       * clang's static analyzer reports it, and no dynamic test covers it
       * because no mock route sends a redirect with an empty Location. */
      cur_url = NULL;

      if (!next_url) {
        result = transport_refused ? transport_rv : resolve_rv;
        break;
      }
      cur_url = next_url;
      if (!preserve && cur_method != CHTTP_HEAD) {
        cur_method = CHTTP_GET;
        cur_body = empty_body;
      }
      continue;
    }

    /* Final hop. */
    if (streaming) {
      if (status_code_out) *status_code_out = pctx.status_code;
      _parse_ctx_free_fields(&pctx);
      _url_free(mp, &url);
      result = ccol_success;
      break;
    }

    chttpcli_response *resp = _resp_alloc(mp);
    if (!resp) {
      _parse_ctx_free_fields(&pctx);
      _ccol_mem_free(mp, bb.buf);
      _url_free(mp, &url);
      result = ccol_not_enough_memory;
      break;
    }
    resp->status_code = pctx.status_code;
    resp->body = bb.buf;
    resp->body_len = bb.len;
    resp->headers = pctx.headers;
    pctx.headers = NULL; /* Ownership moves to resp. */
    resp->_field_lines = pctx.field_lines;
    pctx.field_lines = NULL; /* Ownership moves to resp. */
    _parse_ctx_free_fields(&pctx);
    _url_free(mp, &url);
    *resp_out = resp;
    result = ccol_success;
    break;
  }

  _ccol_mem_free(mp, cur_url);
  _ccol_mem_free(mp, carried_auth);
  _ccol_mem_free(mp, carried_auth_origin);
  _ccol_mem_free(mp, initial_origin_key);
  if (tls_ctx) ctls_ctx_release(tls_ctx);
  _slot_release(cli);
  return result;
}

ccol_retval_t chttpclient_do(chttpcli h, const chttp_request_t *req,
                             chttpcli_response **resp_out) {
  /* *resp_out is NULL before anything can fail, even for a stale or invalid
   * handle. chttp_do_internal does it in the same order. */
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;
  struct chttpclient *raw = _chttpcli_resolve(h);
  if (!raw) return ccol_invalid_args;
  _chttpcli_call_frame_t frame;
  _chttpcli_call_push(&frame, raw, false);
  ccol_retval_t rv =
      chttp_do_internal(raw, req, false, NULL, NULL, resp_out, NULL);
  _chttpcli_call_pop(&frame);
  /* chttp_do_internal has its own _slot_acquire/_slot_release pair inside,
   * around the whole request and the blocking network I/O with it. */
  _chttpcli_resolve_unpin(raw);
  return rv;
}

ccol_retval_t chttpclient_do_streaming(chttpcli h, const chttp_request_t *req,
                                       chttpcli_write_fn write_fn,
                                       void *write_ctx, int *status_code_out) {
  struct chttpclient *raw = _chttpcli_resolve(h);
  if (!raw) return ccol_invalid_args;
  _chttpcli_call_frame_t frame;
  _chttpcli_call_push(&frame, raw, false);
  ccol_retval_t rv = chttp_do_internal(raw, req, true, write_fn, write_ctx,
                                       NULL, status_code_out);
  _chttpcli_call_pop(&frame);
  _chttpcli_resolve_unpin(raw);
  return rv;
}

/* ========================================================================== */
/*                         DEFAULT CLIENT                                     */
/* ========================================================================== */

static void _init_default_client(void) {
  /* The default client is ours, and _client_at_exit destroys it before
   * OpenSSL releases its global state. */
  _ccol_call_once(cli_engine_bundler.exit_hook_once, _client_exit_hook_install);
  atomic_store(&default_client_bundler.client, chttpclient_create(NULL));
}

/* Called from _client_at_exit to destroy the default client, except when a
 * Tier 1 call is running on it. That call may be the one that called
 * exit() from inside its write_fn, and a destroy would wait for it. After
 * this, chttp_default_client() gives CHTTPCLI_INVALID, the same as after
 * the library's destructor has run. */
static void _default_client_destroy_if_idle(void) {
  chttpcli h = atomic_load(&default_client_bundler.client);
  if (!h) return;
  struct chttpclient *raw = _chttpcli_resolve(h);
  if (!raw) return;
  _ccol_mutex_lock(raw->lock);
  bool busy = raw->in_flight_count > 0;
  _ccol_mutex_unlock(raw->lock);
  _chttpcli_resolve_unpin(raw);
  if (busy) return;
  if (atomic_compare_exchange_strong(&default_client_bundler.client, &h, 0))
    __chttpclient_destroy(h);
}

chttpcli chttp_default_client(void) {
  _ccol_call_once(default_client_bundler.once, _init_default_client);
  return atomic_load(&default_client_bundler.client);
}

__attribute__((destructor)) static void _cleanup_default_client(void) {
  /* Clear the handle first and destroy the client after it. Otherwise the
   * defensive compare-and-swap inside __chttpclient_destroy would race our
   * read of the handle (see that function's doc comment). The race needs
   * another thread destroying the same handle at process exit, which is
   * very unlikely, but clearing first makes that compare-and-swap a no-op
   * so it's never a second writer racing this one. */
  chttpcli cli = atomic_exchange(&default_client_bundler.client, 0);
  if (cli) __chttpclient_destroy(cli);

  /* We just destroyed the one client we can own ourselves and freed
   * its slot, so now we free the slot table's bookkeeping arrays, which the
   * --show-leak-kinds=all run of make memtest would otherwise report as
   * still reachable. We free them ONLY when no other chttpcli handle owned
   * by the application is in use.
   *
   * We don't control the order of __attribute__((destructor)) functions and
   * atexit handlers across the shared objects of a process. An application
   * may rely on process exit to reclaim a client it created directly and
   * never called chttpclient_destroy on, and it can have a live
   * handle that a destructor or atexit handler touches after this one runs,
   * with chttpclient_destroy, chttpclient_do, a chttpclient_set_* function
   * or even a new chttpclient_create.
   *
   * Freeing the shared slot table under a live handle makes that a
   * use-after-free. Skipping the free only leaves the leak this comment
   * already describes for the struct itself, the leak of a caller that
   * never destroyed its client, now with the slot table's arrays on top.
   *
   * The _ccol_call_once here is required even though the line above may
   * already have made one. When cli was 0 the default client was never
   * created in this process, which is what happens when an application links
   * the library only for cvector, chashmap or chttpserver and never touches
   * chttpclient. The `if (cli) __chttpclient_destroy(cli);` line above is
   * skipped entirely then, nothing in the process ever called
   * _ccol_call_once, and the block below would lock a mutex that nothing
   * initialized.
   *
   * And that isn't a rare race but a sure thing: it happens on every run of
   * any process that links this shared object without creating a chttpcli
   * handle, because a __attribute__((destructor)) function runs for the
   * whole shared object, whichever parts of it the process actually used. */
  _ccol_call_once(chttpcli_slot_table.once, _chttpcli_slot_table_init_globals);
  _ccol_rw_lock_wrlock(chttpcli_slot_table.rwlock);
  if (_chttpcli_any_slot_live_locked()) {
    /* Rather than skipping the release, leave it to whichever destroy frees
       the last slot, so an application that does destroy its clients leaves
       nothing behind, whatever order the destructors ran in. */
    chttpcli_slot_table.release_deferred = true;
  } else {
    _chttpcli_release_slot_table_locked();
    /* We never free the pin index's slot storage while the process runs,
     * on purpose, because a resolve indexes it with no lock held. Left
     * alone, though, a leak checker that treats still-reachable memory as
     * an error reports it.
     *
     * We free it here rather than from a destructor of its own, so it gets
     * the same "no handle is live" guard as the two vectors above and
     * a fixed order against them, instead of whatever order the destructors
     * happen to run in.
     *
     * The guard proves that no handle is resolvable, and that's the
     * condition that matters, since any resolve possible at this
     * point names a handle this table would reject anyway. What it doesn't
     * exclude is a concurrent resolver: a resolve takes no lock at all, and
     * the write lock we hold only orders this against changes to the slot
     * table.
     *
     * Unpublishing each chunk before the free narrows that down to a
     * resolver that hasn't loaded the chunk pointer yet, but doesn't close
     * it. So a thread that keeps calling into this module while the process
     * tears down is something the caller has to avoid, the same as for the
     * two vectors above. */
  }
  _ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
}

/* Tells whether any slot names a client. The caller holds the write
 * lock.
 *
 * We read slot->ptr and not slot->in_use. A destroy clears in_use as its
 * very first step, to turn away a second destroy or a new resolve as early
 * as possible, and the rest of its teardown (quiescing the engine, draining
 * the pins, the final locked release of the index) comes after that.
 *
 * A scan trusting in_use alone would free this table under a destroy that
 * is inside that window, and the destroy's last step would then index
 * freed memory. ptr is written only once a slot is fully acquired and
 * cleared only in that final locked step, so it is set for exactly as long
 * as the table must not be released. */
static bool _chttpcli_any_slot_live_locked(void) {
  size_t slot_count = cvector_elem_count(chttpcli_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++) {
    chttpcli_slot_t *slot =
        (chttpcli_slot_t *)cvector_at(chttpcli_slot_table.slots, i);
    if (slot->ptr != NULL) return true;
  }
  return false;
}

/* Frees the bookkeeping of the table and of the pin index. The caller holds
 * the write lock and has made sure no slot is live. Each vector is set to
 * NULL on the way, so a later call answers "already released" instead of
 * indexing a freed vector. */
/* The check and the release both live behind one out-of-line call, so the
 * destroy path that needs to make it keeps the same shape it would have
 * without any of this. Cold code in a hot object file isn't free: inlined
 * here, the same few instructions measurably slow down the push path of an
 * unrelated container, because they shift what the linker lays out around
 * it, with no change in the instruction count. */
static __attribute__((noinline)) void
_chttpcli_release_slot_table_if_deferred_locked(void) {
  if (chttpcli_slot_table.release_deferred &&
      !_chttpcli_any_slot_live_locked()) {
    _chttpcli_release_slot_table_locked();
  }
}

static void _chttpcli_release_slot_table_locked(void) {
  cvector_destroy(chttpcli_slot_table.slots);
  cvector_destroy(chttpcli_slot_table.free_indices);
  ccol_pintable_dispose(&chttpcli_pintable);
  chttpcli_slot_table.release_deferred = false;
}

/* ========================================================================== */
/*                         CONVENIENCE API                                    */
/* ========================================================================== */

ccol_retval_t chttp_do(const chttp_request_t *req,
                       chttpcli_response **resp_out) {
  /* We check resp_out and set *resp_out to NULL here, before calling
   * chttp_default_client(), instead of leaving it to the same check inside
   * chttpclient_do. Otherwise the early return for a NULL cli just below
   * would leave *resp_out untouched, the same kind of gap that
   * chttp_do_internal closes with the same order (see its comment). */
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;
  chttpcli cli = chttp_default_client();
  if (!cli) return ccol_unexpected_failure;
  return chttpclient_do(cli, req, resp_out);
}

ccol_retval_t chttp_run_query(chttp_method_t method, const char *url,
                              const chttp_request_body_t *body, chmap headers,
                              chttpcli_response **resp_out) {
  /* resp_out is checked and *resp_out set to NULL before the url check, so
   * a NULL url ALSO leaves *resp_out reliably NULL, not just failures after
   * this point. chttp_do_internal does the same for the same reason. */
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;
  if (!url) return ccol_invalid_args;
  /* chttp_request_create also returns NULL for a method that names no HTTP
   * method and for a body with NULL data and a non-zero length. We refuse
   * both here first, so a NULL from the create below can only mean a failed
   * allocation. */
  if ((unsigned)method >= (unsigned)CHTTP_ANY) return ccol_invalid_args;
  if (body && !body->data && body->len > 0) return ccol_invalid_args;
  chttp_request_t *req = chttp_request_create(method, url, body, NULL);
  if (!req) return ccol_not_enough_memory;

  req->headers = headers;
  ccol_retval_t rv = chttp_do(req, resp_out);
  req->headers = NULL; /* req doesn't own headers, and the free below
                          must not destroy the caller's map. */
  chttp_request_free(req);
  return rv;
}

ccol_retval_t chttp_get(const char *url, chttpcli_response **resp_out) {
  return chttp_run_query(CHTTP_GET, url, NULL, NULL, resp_out);
}

ccol_retval_t chttp_post(const char *url, const chttp_request_body_t *body,
                         chttpcli_response **resp_out) {
  return chttp_run_query(CHTTP_POST, url, body, NULL, resp_out);
}

ccol_retval_t chttp_put(const char *url, const chttp_request_body_t *body,
                        chttpcli_response **resp_out) {
  return chttp_run_query(CHTTP_PUT, url, body, NULL, resp_out);
}

ccol_retval_t chttp_delete(const char *url, chttpcli_response **resp_out) {
  return chttp_run_query(CHTTP_DELETE, url, NULL, NULL, resp_out);
}

ccol_retval_t chttp_patch(const char *url, const chttp_request_body_t *body,
                          chttpcli_response **resp_out) {
  return chttp_run_query(CHTTP_PATCH, url, body, NULL, resp_out);
}

/* ========================================================================== */
/*                         RESPONSE API                                       */
/* ========================================================================== */

/* _lower_name_copy with resp's procs. Looking up a few known header names on
 * every response is a common and hot pattern, so the copy goes on the stack
 * whenever the name fits. */
enum { CHTTP_RESP_NAME_STACK_BUF = CHTTP_NAME_STACK_BUF };
static char *_resp_lower_name(const chttpcli_response *resp, const char *name,
                              char *stack_buf, size_t *len_out) {
  return _lower_name_copy(resp->_m_procs, name, stack_buf, len_out);
}

/* Looks up the lower-case name in resp's header map. */
static const char *_resp_map_value(const chttpcli_response *resp,
                                   const char *lower, size_t nlen) {
  cmap_pair kp = {.ptr = (void *)lower, .size = nlen + 1};
  cmap_pair vp;
  if (chmap_get_elem_ref((chmap)resp->headers, &kp, &vp) != ccol_success)
    return NULL;
  return (const char *)vp.ptr;
}

/* Finds occurrence `index` of the lower-case name and, when count_out isn't
 * NULL, counts all occurrences into *count_out. A repeated name has one
 * record per occurrence in resp's field lines, while a name that appears
 * once only has its map entry. */
static const char *_resp_occurrence(const chttpcli_response *resp,
                                    const char *lower, size_t nlen,
                                    size_t index, size_t *count_out) {
  const chttp_field_lines_t *lines =
      (const chttp_field_lines_t *)resp->_field_lines;
  size_t seen = 0;
  const char *found = NULL;
  const chttp_field_slot_t *slot = _field_lines_find(lines, lower, nlen);
  for (uint32_t o = slot ? slot->head : 0; o;) {
    chttp_field_rec_t r = _field_rec_at(lines, o - 1);
    if (seen == index) {
      found = _field_rec_value(lines, o - 1, &r);
      if (!count_out) return found;
    }
    seen++;
    o = r.next;
  }
  if (seen == 0) {
    const char *v = _resp_map_value(resp, lower, nlen);
    seen = v ? 1 : 0;
    found = (v && index == 0) ? v : NULL;
  }
  if (count_out) *count_out = seen;
  return found;
}

const char *chttpclient_resp_header(const chttpcli_response *resp,
                                    const char *name) {
  if (!resp || !name || !resp->headers) return NULL;
  char stack_lower[CHTTP_RESP_NAME_STACK_BUF];
  size_t nlen;
  char *lower = _resp_lower_name(resp, name, stack_lower, &nlen);
  if (!lower) return NULL;
  const char *v = _resp_map_value(resp, lower, nlen);
  if (lower != stack_lower) _ccol_mem_free(resp->_m_procs, lower);
  return v;
}

size_t chttpclient_resp_header_count(const chttpcli_response *resp,
                                     const char *name) {
  if (!resp || !name || !resp->headers) return 0;
  char stack_lower[CHTTP_RESP_NAME_STACK_BUF];
  size_t nlen;
  char *lower = _resp_lower_name(resp, name, stack_lower, &nlen);
  if (!lower) return 0;
  size_t count = 0;
  (void)_resp_occurrence(resp, lower, nlen, 0, &count);
  if (lower != stack_lower) _ccol_mem_free(resp->_m_procs, lower);
  return count;
}

const char *chttpclient_resp_header_at(const chttpcli_response *resp,
                                       const char *name, size_t index) {
  if (!resp || !name || !resp->headers) return NULL;
  char stack_lower[CHTTP_RESP_NAME_STACK_BUF];
  size_t nlen;
  char *lower = _resp_lower_name(resp, name, stack_lower, &nlen);
  if (!lower) return NULL;
  const char *v = _resp_occurrence(resp, lower, nlen, index, NULL);
  if (lower != stack_lower) _ccol_mem_free(resp->_m_procs, lower);
  return v;
}

void chttpclient_resp_free(chttpcli_response *resp) {
  if (!resp) return;
  /* _m_procs points into the same block as resp, so we free that block
   * through a local copy. The header map has its own copy of the procs. */
  ccol_memmgmt_procs_t procs;
  ccol_memmgmt_procs_t *mp = NULL;
  if (resp->_m_procs) {
    procs = *resp->_m_procs;
    mp = &procs;
  }
  _ccol_mem_free(mp, resp->body);
  if (resp->headers) __chmap_destroy((chmap)resp->headers);
  _field_lines_free(mp, (chttp_field_lines_t *)resp->_field_lines);
  _ccol_mem_free(mp, resp);
}
