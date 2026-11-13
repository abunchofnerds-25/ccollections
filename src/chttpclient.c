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

// Constants
//
// The most redirects one request follows (as in Go), and the caps of the
// keep-alive idle pool, idle connections per origin and in total, so idle
// sockets can't pile up.
#define CHTTP_MAX_REDIRECTS 50
#define CHTTP_MAX_IDLE_PER_ORIGIN 4
#define CHTTP_MAX_IDLE_TOTAL 32
// The most distinct origin keys the idle pool's map holds. A client that talks
// to many hosts once each would otherwise keep a map entry for every origin it
// ever saw. When the cap is full, a connection to a new origin simply isn't
// pooled.
#define CHTTP_MAX_IDLE_ORIGINS 128
#ifdef RUNNING_UNIT_TESTS
// A test sets a small value here, so it can hit the origin cap without 128 real
// origins. 0 means the real constant.
static _Atomic size_t g_max_idle_origins_override_for_tests = 0;
#endif
// Gives the origin cap in effect, so the pool code reads one value whether or
// not a test set an override in g_max_idle_origins_override_for_tests.
static size_t effective_max_idle_origins(void) {
#ifdef RUNNING_UNIT_TESTS
  size_t override = atomic_load(&g_max_idle_origins_override_for_tests);
  if (override > 0) return override;
#endif
  return CHTTP_MAX_IDLE_ORIGINS;
}
// How long an idle connection may sit in the pool before we drop it, since
// servers close idle connections after a while and a stale one would fail the
// next request.
#define CHTTP_IDLE_MAX_AGE_MS 60000L
// How long we wait for a "100 Continue" before sending the body anyway. It's
// the default of curl's CURLOPT_EXPECT_100_TIMEOUT_MS.
#define CHTTP_100_CONTINUE_WAIT_US UINT64_C(1000000)
// Happy Eyeballs' Connection Attempt Delay (RFC 8305 section 5): With several
// addresses, we start on the next one when the attempts in flight give no
// result for this long. Go's net.Dialer uses the same value.
#define CHTTP_CONNECT_ATTEMPT_DELAY_MS 250L
#ifdef RUNNING_UNIT_TESTS
// Hooks for the Happy Eyeballs tests. The first replaces the attempt delay.
// While the second is above 0, each attempt delay that starts takes one from it
// and lasts 1 ms. While the third is above 0, each Tier 1 attempt that
// completes takes one from it and counts as ECONNREFUSED; a loopback connect
// that completes inside connect(2) counts as in progress then, so the hook sees
// it.
static _Atomic long g_connect_attempt_delay_ms_for_tests = 0;
static _Atomic int g_connect_short_delays_for_tests = 0;
static _Atomic int g_connect_async_failures_for_tests = 0;

// Set the three Happy Eyeballs hooks above, so a test can shape the attempt
// timing and failures.
void chttp__set_connect_attempt_delay_ms_for_tests(long ms) {
  atomic_store(&g_connect_attempt_delay_ms_for_tests, ms);
}

void chttp__set_connect_short_delays_for_tests(int n) {
  atomic_store(&g_connect_short_delays_for_tests, n);
}

void chttp__set_connect_async_failures_for_tests(int n) {
  atomic_store(&g_connect_async_failures_for_tests, n);
}

// Takes one from *counter if it's above 0 and tells whether it did. The
// counting test hooks use it to fire a given number of times.
static bool take_one_for_tests(_Atomic int *counter) {
  int v = atomic_load(counter);
  while (v > 0)
    if (atomic_compare_exchange_weak(counter, &v, v - 1)) return true;
  return false;
}
#endif

// Gives the attempt delay in effect, so every tier reads one value whether or
// not a test replaced it through the hooks above.
static inline long connect_attempt_delay_ms(void) {
#ifdef RUNNING_UNIT_TESTS
  if (take_one_for_tests(&g_connect_short_delays_for_tests)) return 1;
  long ms = atomic_load(&g_connect_attempt_delay_ms_for_tests);
  if (ms > 0) return ms;
#endif
  return CHTTP_CONNECT_ATTEMPT_DELAY_MS;
}
// The most interim (1xx) responses chttp_read_message_loop throws away in one
// message. Without a cap, a server that sends "103 Early Hints" forever would
// hold the caller's thread and its limiter slot for good, since requests have
// no timeout by default.
#define CHTTP_MAX_INTERIM_RESPONSES 64

// Internal types
//
// A parsed URL. Every string is an owned copy from the client's allocator. A
// "http+unix://" URL fills is_unix and unix_socket_path and leaves host NULL
// and port 0. origin_key ("scheme://host:port" or "unix://<path>") is the idle
// pool's key and, for TCP, gives the SNI name.
typedef struct {
  bool is_https;
  // host is an IPv6 literal without brackets, which is what connect() and TLS
  // want; the Host header, origin_key and a rebuilt URL add them back.
  bool is_ipv6;
  bool is_unix;
  char *unix_socket_path;
  char *host;
  uint16_t port;
  char *path_and_query;
  char *origin_key;
  // "Basic <b64>" from the "user:pass@" part of this URL, or NULL.
  char *userinfo_authorization;
} chttp_url;

// An absolute deadline; active == false means no limit.
typedef struct {
  bool active;
  struct timespec deadline;
} chttp_deadline;

// One connection, the thing the idle pool holds. It's always kept by value (on
// the stack, in the pool's cvector, or inside another object), so it never
// costs a heap block of its own.
typedef struct {
  int fd;
  ctls_conn *tls;
  char *origin_key;
  struct timespec last_used;
  // The client's tls_generation when this connection did its handshake. The
  // pool reuses a TLS connection only while it matches (see idle_pool_take).
  uint64_t tls_generation;
  // The inactivity timeout of the request this connection serves (0 for none),
  // and how long that request has waited since a byte last moved.
  uint64_t inactivity_us;
  uint64_t inactivity_waited_ns;
} chttp_conn;

// A growable buffer for a response body. max_size is the cap of
// chttpcli_set_max_response_body_size (0 for none); the first append past it
// sets too_large instead of growing, so an oversized body is cut off early.
typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  ccol_memmgmt_procs *mp;
  bool oom;
  size_t max_size;
  bool too_large;
} chttp_bodybuf;

// Every occurrence, in wire order, of each field name that appears more than
// once in one response. data holds one record per occurrence: A
// chttp_field_rec, then "name\0value\0" with the name lower-cased, padded to 4
// bytes. The records of one name are chained through next, and index is an
// open-addressing table with one slot per repeated name (first and last
// record), so adding an occurrence and reading them all are linear. A response
// with no repeated name allocates none of this.
typedef struct {
  uint32_t name_len;
  uint32_t value_len;
  uint32_t next;
} chttp_field_rec;

// One index slot: The first and last record of a name's chain.
typedef struct {
  uint32_t head;
  uint32_t tail;
} chttp_field_slot;

// The field-lines block of one response (see chttp_field_rec).
typedef struct {
  size_t len;
  size_t cap;
  chttp_field_slot *index;
  size_t index_cap;
  size_t names;
  char data[];
} chttp_field_lines;

// The parse state of one response (one hop). Every hop of a redirect chain gets
// a fresh one, so nothing from a redirect's headers leaks into the final
// response.
typedef struct {
  ccol_memmgmt_procs *mp;
  chmap headers;
  chttp_field_lines *field_lines;

  bool is_head_request;
  bool will_redirect;
  char *location;

  bool message_complete;
  bool trailing_garbage;
  bool error;
  bool aborted;
  bool too_large;
  int status_code;

  chttpcli_write_fn requested_sink_fn;
  void *requested_sink_ctx;
  chttpcli_write_fn sink_fn;
  void *sink_ctx;
} chttp_parse_ctx;

// Client handle slot table
//
// A chttpcli handle holds the slot index in its top 32 bits and the generation
// in the bottom 32. Every call resolves the handle through this table before it
// touches the client, so a double destroy or a destroy racing a call is a
// ccol__fatal_err instead of a use-after-free.
typedef struct {
  struct chttpclient *ptr;
  uint32_t generation;
  bool in_use;
} chttpcli_slot;

// The slots, guarded by lock. slots only grows, so an index never goes away;
// free_indices is a LIFO list for reuse. When the exit destructor finds a live
// client it leaves the table alone, and the destroy that releases the last slot
// frees it later. lost_indices counts released indices that a failed push kept
// off the free list.
static struct {
  ccol__rw_lock rwlock;
  ccol__once_flag once;
  cvec slots;
  cvec free_indices;
  bool release_deferred;
  size_t lost_indices;
} chttpcli_slot_table = {0};

#ifdef RUNNING_UNIT_TESTS
// When true, the next push onto free_indices acts as if its allocation failed.
static _Atomic bool g_chttpcli_fail_next_free_index_push = false;

// Arms g_chttpcli_fail_next_free_index_push, so a test can reach the lost-index
// path without a real allocation failure.
void chttpcli__force_next_free_index_push_failure_for_tests(void) {
  atomic_store(&g_chttpcli_fail_next_free_index_push, true);
}
#endif

// Gives a released slot's index back for reuse, so the table doesn't grow on
// every create. A failed push counts the index in lost_indices instead of
// dropping it, for chttpcli_reclaim_lost_index_locked to find. The caller holds
// the table's write lock.
static void chttpcli_free_index_release_locked(uint32_t idx) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_chttpcli_fail_next_free_index_push, false)) {
    chttpcli_slot_table.lost_indices++;
    return;
  }
#endif
  if (cvector_push_back(chttpcli_slot_table.free_indices, &idx) != ccol_success)
    chttpcli_slot_table.lost_indices++;
}

// Finds one slot whose index a failed push stranded. The caller holds the write
// lock and saw free_indices empty, which is what makes "ptr NULL and not in
// use" mean lost. A scan that finds nothing zeroes the counter, so later
// acquires don't scan again.
static __attribute__((noinline)) bool chttpcli_reclaim_lost_index_locked(
    uint32_t *out_idx) {
  size_t slot_count = cvector_elem_count(chttpcli_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++) {
    chttpcli_slot *slot =
        (chttpcli_slot *)cvector_at(chttpcli_slot_table.slots, i);
    if (slot->ptr == NULL && !slot->in_use) {
      *out_idx = (uint32_t)i;
      chttpcli_slot_table.lost_indices--;
      return true;
    }
  }
  chttpcli_slot_table.lost_indices = 0;
  return false;
}

static bool chttpcli_any_slot_live_locked(void);
static void chttpcli_release_slot_table_locked(void);
static void chttpcli_release_slot_table_if_deferred_locked(void);

// The hot half of the slot table: It maps a handle to a client pointer and
// holds the pin that keeps the client alive during a call. A resolve runs on
// every request and writes nothing other threads read.
static ccol_pintable chttpcli_pintable;

// Initializes the slot table's lock and pin table, once, from the first create
// or resolve that needs them.
static void chttpcli_slot_table_init_globals(void) {
  if (ccol__rw_lock_init(chttpcli_slot_table.rwlock) != 0)
    ccol__fatal_err("chttpcli slot table: failed to initialize rwlock");
  chttpcli_slot_table.slots = cvector_create(sizeof(chttpcli_slot), NULL);
  if (!chttpcli_slot_table.slots)
    ccol__fatal_err("chttpcli slot table: failed to allocate slots vector");
  chttpcli_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!chttpcli_slot_table.free_indices)
    ccol__fatal_err(
        "chttpcli slot table: failed to allocate free-index vector");
}

// The client behind a chttpcli handle. lock guards everything here except the
// async counter, which has its own lock.
struct chttpclient {
  ccol__mutex lock;
  // The concurrency limiter: At most pool_cap requests in flight.
  ccol__cond_var available;

  size_t pool_cap;
  size_t configured_pool_size;
  size_t in_flight_count;
  bool pool_initialized;
  bool destroying;

  // The Tier 1 idle pool, chmap(origin_key -> cvec of chttp_conn).
  chmap idle_pools;
  size_t idle_total_count;

  // The Tier 2 idle pool, chmap(origin_key -> cvec of chttp_async_ctx *), and
  // the condition destroy waits on until it's empty.
  chmap idle_pools_async;
  size_t idle_total_count_async;
  ccol__cond_var idle_async_drained;

  // Active Tier 2 and Tier 3 chains; destroy waits for the count to reach 0.
  ccol__mutex async_count_lock;
  ccol__cond_var async_count_drained;
  size_t async_in_flight_count;

  // Settings that requests copy when they start.
  uint64_t connect_timeout_us;
  uint64_t request_timeout_us;
  uint64_t inactivity_timeout_us;
  size_t max_response_body_size;
  chttp_tls_config tls;
  char *owned_cert_path;
  char *owned_key_path;
  char *owned_ca_bundle_path;
  // The TLS context, rebuilt by every chttpcli_set_tls. tls_ctx_usable is false
  // when a configured file couldn't be read or the last rebuild ran out of
  // memory; tls_ctx_oom makes the next request try that rebuild again.
  ctls_ctx *tls_ctx;
  bool tls_ctx_usable;
  bool tls_ctx_oom;

  ccol_memmgmt_procs *m_procs;

  chttpcli self_handle;

  // Bumped by every TLS change, so the pool never reuses a connection made
  // under an older policy.
  uint64_t tls_generation;
};

// One frame of the per-thread stack of clients with a request running on this
// thread: A Tier 1 call for its whole length, or the write_fn of a streaming
// Tier 2 or Tier 3 request on an engine thread. Frames live on the pusher's C
// stack. chttpcli__destroy walks the stack, because destroying a client that
// has a frame here would wait for this very call to end; it makes that a fatal
// error instead. on_engine marks a frame pushed on an engine thread.
typedef struct chttpcli_call_frame {
  const struct chttpclient *cli;
  struct chttpcli_call_frame *prev;
  bool on_engine;
} chttpcli_call_frame;

// The top of this thread's frame stack.
static __thread chttpcli_call_frame *g_chttpcli_call_top = NULL;

// Push and pop a call frame on this thread's stack, around a Tier 1 call or a
// streaming write_fn, so chttpcli__destroy can spot a destroy from inside a
// request of the same client.
static inline void chttpcli_call_push(chttpcli_call_frame *f,
                                      const struct chttpclient *cli,
                                      bool on_engine) {
  f->cli = cli;
  f->prev = g_chttpcli_call_top;
  f->on_engine = on_engine;
  g_chttpcli_call_top = f;
}

static inline void chttpcli_call_pop(chttpcli_call_frame *f) {
  g_chttpcli_call_top = f->prev;
}

// Tells whether cli has a frame on this thread's stack. chttpcli__destroy uses
// it to make a destroy from inside one of cli's own requests fatal instead of a
// wait for itself.
static bool chttpcli_call_runs_on_this_thread(const struct chttpclient *cli) {
  for (const chttpcli_call_frame *f = g_chttpcli_call_top; f; f = f->prev)
    if (f->cli == cli) return true;
  return false;
}

// Tells whether the calling thread is an engine thread running an application's
// write_fn, the only application code that runs there. Destroy and
// chttpcli_engine_wait use it, because waiting there would wait for the engine
// itself.
static bool chttpcli_runs_on_engine_thread(void) {
  for (const chttpcli_call_frame *f = g_chttpcli_call_top; f; f = f->prev)
    if (f->on_engine) return true;
  return false;
}

// Client handle resolve and unpin
//
// Resolves h and pins the client so a concurrent destroy can't free it. It
// gives NULL for 0, a garbage handle, a free slot or a reused one (the
// generation doesn't match). On success the caller calls chttpcli_resolve_unpin
// exactly once, as soon as its tier's own count protects the client. It takes
// no lock and writes nothing shared, since every public call comes through
// here.
static struct chttpclient *chttpcli_resolve(chttpcli h) {
  return (struct chttpclient *)ccol_pintable_pin(&chttpcli_pintable, h);
}

// Drops the pin and nothing else, without the client's lock.
static void chttpcli_resolve_unpin(struct chttpclient *raw) {
  ccol_pintable_unpin(&chttpcli_pintable, raw->self_handle);
}

// Gives cli a new or freed slot and returns its handle, or 0 when an allocation
// fails. chttpcli_create_mp calls it once, after the client is fully built. It
// refuses an index the pin table can't hold, skips the generation that would
// make the handle equal CHTTPCLI_NULL, and sets self_handle before the publish.
static chttpcli chttpcli_handle_slot_acquire(struct chttpclient *cli) {
  ccol__call_once(chttpcli_slot_table.once, chttpcli_slot_table_init_globals);
  ccol__rw_lock_wrlock(chttpcli_slot_table.rwlock);
  uint32_t idx;
  chttpcli_slot *slot;
  if (cvector_elem_count(chttpcli_slot_table.free_indices) > 0) {
    cvector_pop_back(chttpcli_slot_table.free_indices, &idx);
    slot = (chttpcli_slot *)cvector_at(chttpcli_slot_table.slots, idx);
  } else if (chttpcli_slot_table.lost_indices > 0 &&
             chttpcli_reclaim_lost_index_locked(&idx)) {
    slot = (chttpcli_slot *)cvector_at(chttpcli_slot_table.slots, idx);
  } else {
    if (cvector_elem_count(chttpcli_slot_table.slots) >= CCOL_PIN_MAX_SLOTS) {
      ccol__rw_lock_unlock(chttpcli_slot_table.rwlock);
      return 0;
    }
    chttpcli_slot fresh = {0};
    if (cvector_push_back(chttpcli_slot_table.slots, &fresh) != ccol_success) {
      ccol__rw_lock_unlock(chttpcli_slot_table.rwlock);
      return 0;
    }
    idx = (uint32_t)cvector_elem_count(chttpcli_slot_table.slots) - 1;
    slot = (chttpcli_slot *)cvector_at(chttpcli_slot_table.slots, idx);
  }
  slot->generation++;
  if (slot->generation == 0) slot->generation++;
  chttpcli h = ((chttpcli)idx << 32) | (chttpcli)slot->generation;

  cli->self_handle = h;

  if (!ccol_pintable_publish(&chttpcli_pintable, idx, slot->generation, cli)) {
    cli->self_handle = 0;
    chttpcli_free_index_release_locked(idx);
    ccol__rw_lock_unlock(chttpcli_slot_table.rwlock);
    return 0;
  }

  slot->ptr = cli;
  slot->in_use = true;
  ccol__rw_lock_unlock(chttpcli_slot_table.rwlock);
  return h;
}

// Default client
//
// The singleton behind chttp_default_client, created once and never
// re-initialized. client is atomic so chttpcli__destroy can clear it with a
// compare-and-swap when it's given this handle.
static struct {
  _Atomic(chttpcli) client;
  ccol__once_flag once;
} default_client_bundler = {0};

// URL parsing
//
// Frees the strings of u, for every path that drops a parsed URL. mp is the
// allocator they came from.
static void url_free(ccol_memmgmt_procs *mp, chttp_url *u) {
  if (!u) return;
  ccol__mem_free(mp, u->unix_socket_path);
  ccol__mem_free(mp, u->host);
  ccol__mem_free(mp, u->path_and_query);
  ccol__mem_free(mp, u->origin_key);
  ccol__mem_free(mp, u->userinfo_authorization);
  memset(u, 0, sizeof(*u));
}

// Percent-encodes len bytes of a unix socket path into a new string, for
// rebuilding a "http+unix://<path>" URL (origin_key, a resolved redirect).
// Every byte outside RFC 3986's unreserved set is escaped, '/' included,
// because the whole path has to fit into one authority component. The length
// comes in as a parameter so a test can reach the overflow guard.
static char *percent_encode_unix_path_len(ccol_memmgmt_procs *mp,
                                          const char *path, size_t len) {
  static const char *unreserved =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
  if (len > (SIZE_MAX - 1) / 3) return NULL;
  char *out = (char *)ccol__mem_alloc(mp, len * 3 + 1);
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

// Percent-encodes a NUL-terminated unix socket path into a new string, for
// building origin_key and redirect URLs of "http+unix://" targets. Gives NULL
// when the allocation fails.
static char *percent_encode_unix_path(ccol_memmgmt_procs *mp,
                                      const char *path) {
  return percent_encode_unix_path_len(mp, path, strlen(path));
}

#ifdef RUNNING_UNIT_TESTS
// Lets a test drive the overflow guard of percent_encode_unix_path_len with a
// made-up length. Call it only with fake_len above (SIZE_MAX - 1) / 3, so the
// guard refuses the call before anything reads path.
bool chttp__percent_encode_unix_path_overflow_guard_for_tests(
    ccol_memmgmt_procs *mp, const char *path, size_t fake_len) {
  char *r = percent_encode_unix_path_len(mp, path, fake_len);
  bool rejected = (r == NULL);
  ccol__mem_free(mp, r);
  return rejected;
}
#endif

// Percent-decodes [start, start+len) into a new string. Only userinfo is
// decoded; a malformed escape and an escaped NUL are refused, since a password
// cut at a NUL would give a wrong Authorization header.
static ccol_retval percent_decode_component(ccol_memmgmt_procs *mp,
                                            const char *start, size_t len,
                                            char **out) {
  char *buf = (char *)ccol__mem_alloc(mp, len + 1);
  if (!buf) return ccol_not_enough_memory;
  size_t w = 0;
  for (size_t i = 0; i < len; i++) {
    char c = start[i];
    if (c == '%') {
      if (i + 2 >= len || !isxdigit((unsigned char)start[i + 1]) ||
          !isxdigit((unsigned char)start[i + 2])) {
        ccol__mem_free(mp, buf);
        return ccol_http_invalid_url;
      }
      char hex[3] = {start[i + 1], start[i + 2], '\0'};
      int v = (int)strtol(hex, NULL, 16);
      if (v == 0) {
        ccol__mem_free(mp, buf);
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

// True for the bytes RFC 3986 allows as they are in a path or query
// (unreserved, sub-delims, ':', '@', '/', '?'). The caller handles '%'.
static bool is_target_literal_byte(unsigned char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9'))
    return true;
  return c != 0 && strchr("-._~!$&'()*+,;=:@/?", (int)c) != NULL;
}

// Builds the request-target "/path[?query]" from pq[0..pq_len), so the request
// line is always valid RFC 3986 whatever bytes the caller's URL or a Location
// holds. Every byte not allowed there becomes "%XX" (upper-case hex) and a
// valid escape is kept. *out gets the new string from mp; a stray '%' is
// ccol_http_invalid_url (as in Go's net/url), a failed allocation
// ccol_not_enough_memory.
static ccol_retval build_request_target(ccol_memmgmt_procs *mp, const char *pq,
                                        size_t pq_len, char **out) {
  *out = NULL;
  bool add_slash = !(pq_len > 0 && pq[0] == '/');
  size_t need = add_slash ? 1 : 0;
  for (size_t i = 0; i < pq_len; i++) {
    unsigned char c = (unsigned char)pq[i];
    if (need > SIZE_MAX - 4) return ccol_not_enough_memory;
    if (c == '%') {
      if (i + 2 >= pq_len || !isxdigit((unsigned char)pq[i + 1]) ||
          !isxdigit((unsigned char)pq[i + 2]))
        return ccol_http_invalid_url;
      need += 3;
      i += 2;
    } else if (is_target_literal_byte(c)) {
      need += 1;
    } else {
      need += 3;
    }
  }
  char *t = (char *)ccol__mem_alloc(mp, need + 1);
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
    } else if (is_target_literal_byte(c)) {
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

// Parses the authority of a "http+unix://" URL for parse_chttp_url. p points at
// a percent-encoded path such as "%2Fvar%2Frun%2Fapp.sock" (the
// requests-unixsocket convention); the decoded path goes into out. There's no
// "https+unix://" and no userinfo, and a raw CR or LF is refused.
static ccol_retval parse_chttp_unix_url(ccol_memmgmt_procs *mp, const char *p,
                                        chttp_url *out) {
  const char *authority_end = p;
  while (*authority_end && *authority_end != '/' && *authority_end != '?' &&
         *authority_end != '#')
    authority_end++;
  size_t enc_len = (size_t)(authority_end - p);
  if (enc_len == 0) return ccol_http_invalid_url;

  char *unix_path = NULL;
  ccol_retval drv = percent_decode_component(mp, p, enc_len, &unix_path);
  if (drv != ccol_success) return drv;
  if (unix_path[0] == '\0') {
    ccol__mem_free(mp, unix_path);
    return ccol_http_invalid_url;
  }

  const char *q = authority_end;
  const char *pq = (*q && *q != '#') ? q : "/";
  size_t pq_raw_len = strlen(pq);
  const char *frag = memchr(pq, '#', pq_raw_len);
  size_t pq_len = frag ? (size_t)(frag - pq) : pq_raw_len;

  if (memchr(pq, '\r', pq_len) || memchr(pq, '\n', pq_len)) {
    ccol__mem_free(mp, unix_path);
    return ccol_http_invalid_url;
  }

  char *path_and_query = NULL;
  ccol_retval trv = build_request_target(mp, pq, pq_len, &path_and_query);
  if (trv != ccol_success) {
    ccol__mem_free(mp, unix_path);
    return trv;
  }

  int needed = snprintf(NULL, 0, "unix://%s", unix_path);
  if (needed < 0) {
    ccol__mem_free(mp, unix_path);
    ccol__mem_free(mp, path_and_query);
    return ccol_unexpected_failure;
  }
  char *origin_key = (char *)ccol__mem_alloc(mp, (size_t)needed + 1);
  if (!origin_key) {
    ccol__mem_free(mp, unix_path);
    ccol__mem_free(mp, path_and_query);
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

// Gives an owned "[host]" for an IPv6 literal and a plain copy for any other
// host, for the places where a host goes back into a URL string.
static char *format_bracketed_host(ccol_memmgmt_procs *mp, const char *host,
                                   bool is_ipv6) {
  if (!is_ipv6) return ccol_strdup(mp, host);
  size_t hlen = strlen(host);
  if (hlen > SIZE_MAX - 3) return NULL;
  char *out = (char *)ccol__mem_alloc(mp, hlen + 3);
  if (!out) return NULL;
  out[0] = '[';
  memcpy(out + 1, host, hlen);
  out[hlen + 1] = ']';
  out[hlen + 2] = '\0';
  return out;
}

// Tells whether c is RFC 3986 unreserved or a sub-delim, in ASCII whatever the
// locale, for the host and userinfo grammar checks.
static bool uri_unreserved_or_sub_delim(unsigned char c) {
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

// Tells whether c is an ASCII hex digit, for the pct-encoded checks.
static bool uri_hexdig(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

// Checks that s[0..len) is a non-empty RFC 3986 reg-name (unreserved,
// sub-delims, pct-encoded). parse_chttp_url refuses anything else, because such
// a host would reach the Host header, the pool key, SNI and the resolver in a
// form that recipients read differently. A non-ASCII name must come in its
// punycode form.
static bool uri_reg_name_is_valid(const char *s, size_t len) {
  if (len == 0) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '%') {
      if (len - i < 3 || !uri_hexdig((unsigned char)s[i + 1]) ||
          !uri_hexdig((unsigned char)s[i + 2]))
        return false;
      i += 2;
      continue;
    }
    if (!uri_unreserved_or_sub_delim(c)) return false;
  }
  return true;
}

// Whether s[0..len), the inside of an IP-literal's brackets, is an IPv6address
// or IPvFuture. Zone IDs are refused. Only hex digits, ':' and '.' reach
// inet_pton(), because macOS's inet_pton() accepts a zone suffix.
static bool uri_ip_literal_is_valid(const char *s, size_t len) {
  if (len == 0) return false;
  if (s[0] == 'v' || s[0] == 'V') {
    size_t j = 1;
    while (j < len && uri_hexdig((unsigned char)s[j])) j++;
    if (j == 1 || j >= len || s[j] != '.') return false;
    j++;
    if (j >= len) return false;
    for (; j < len; j++) {
      unsigned char c = (unsigned char)s[j];
      if (!uri_unreserved_or_sub_delim(c) && c != ':') return false;
    }
    return true;
  }
  char text[INET6_ADDRSTRLEN];
  if (len >= sizeof(text)) return false;
  for (size_t j = 0; j < len; j++) {
    unsigned char c = (unsigned char)s[j];
    if (!uri_hexdig(c) && c != ':' && c != '.') return false;
  }
  memcpy(text, s, len);
  text[len] = '\0';
  struct in6_addr addr;
  return inet_pton(AF_INET6, text, &addr) == 1;
}

// Whether s[0..len) is RFC 3986 userinfo. A raw '@' is accepted too (we split
// at the last '@', as Go does); a backslash is refused, since WHATWG readers
// treat it as '/' and would find a different host.
static bool uri_userinfo_is_valid(const char *s, size_t len) {
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (!uri_unreserved_or_sub_delim(c) && c != ':' && c != '@' && c != '%')
      return false;
  }
  return true;
}

// Parses an "http://", "https://" or "http+unix://" URL into *out. Userinfo
// becomes a ready "Basic <base64>" value, the host is checked against the RFC
// 3986 grammar, an empty port means the scheme's default, and a fragment is
// dropped (a client never sends one). The '@' and ':' delimiters are found in
// the raw string before each part is decoded, and raw CR or LF anywhere is
// refused, since it could inject header lines. Any failure is
// ccol_http_invalid_url or ccol_not_enough_memory, and leaves *out empty.
static ccol_retval parse_chttp_url(ccol_memmgmt_procs *mp, const char *url,
                                   chttp_url *out) {
  memset(out, 0, sizeof(*out));
  if (!url) return ccol_http_invalid_url;

  if (ccol_ascii_ncasecmp(url, "http+unix://", 12) == 0)
    return parse_chttp_unix_url(mp, url + 12, out);

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

  const char *authority_end = p;
  while (*authority_end && *authority_end != '/' && *authority_end != '?' &&
         *authority_end != '#')
    authority_end++;

  const char *last_at = NULL;
  for (const char *s = p; s < authority_end; s++)
    if (*s == '@') last_at = s;

  char *userinfo_authorization = NULL;
  const char *host_scan_start = p;
  if (last_at) {
    if (!uri_userinfo_is_valid(p, (size_t)(last_at - p)))
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
    ccol_retval drv =
        percent_decode_component(mp, user_start, user_len, &user_dec);
    if (drv == ccol_success)
      drv = percent_decode_component(mp, pass_start, pass_len, &pass_dec);
    if (drv != ccol_success) {
      ccol__mem_free(mp, user_dec);
      ccol__mem_free(mp, pass_dec);
      return drv;
    }

    if (strchr(user_dec, ':')) {
      ccol__mem_free(mp, user_dec);
      ccol__mem_free(mp, pass_dec);
      return ccol_http_invalid_url;
    }

    userinfo_authorization = chttp_basic_auth_mp(mp, user_dec, pass_dec);
    ccol__mem_free(mp, user_dec);
    ccol__mem_free(mp, pass_dec);
    if (!userinfo_authorization) return ccol_not_enough_memory;

    host_scan_start = last_at + 1;
  }

  bool is_ipv6 = false;
  const char *host_start;
  const char *q;
  if (*host_scan_start == '[') {
    const char *close = host_scan_start + 1;
    while (close < authority_end && *close != ']') close++;
    if (close >= authority_end) {
      ccol__mem_free(mp, userinfo_authorization);
      return ccol_http_invalid_url;
    }
    char after = close[1];
    if (after != '\0' && after != ':' && after != '/' && after != '?' &&
        after != '#') {
      ccol__mem_free(mp, userinfo_authorization);
      return ccol_http_invalid_url;
    }
    host_start = host_scan_start + 1;
    q = close;
    is_ipv6 = true;
  } else {
    host_start = host_scan_start;
    q = host_scan_start;
    while (*q && *q != ':' && *q != '/' && *q != '?' && *q != '#') q++;
  }
  size_t host_len = (size_t)(q - host_start);
  if (host_len == 0) {
    ccol__mem_free(mp, userinfo_authorization);
    return ccol_http_invalid_url;
  }
  if (is_ipv6 ? !uri_ip_literal_is_valid(host_start, host_len)
              : !uri_reg_name_is_valid(host_start, host_len)) {
    ccol__mem_free(mp, userinfo_authorization);
    return ccol_http_invalid_url;
  }
  if (is_ipv6) q++;

  uint16_t port = https ? 443 : 80;
  if (*q == ':') {
    q++;
    unsigned long pv = 0;
    size_t pd = 0;
    while (*q && isdigit((unsigned char)*q)) {
      pv = pv * 10 + (unsigned long)(*q - '0');
      if (pv > 65535) {
        ccol__mem_free(mp, userinfo_authorization);
        return ccol_http_invalid_url;
      }
      q++;
      pd++;
    }
    if (pd > 0 && pv == 0) {
      ccol__mem_free(mp, userinfo_authorization);
      return ccol_http_invalid_url;
    }
    if (*q != '\0' && *q != '/' && *q != '?' && *q != '#') {
      ccol__mem_free(mp, userinfo_authorization);
      return ccol_http_invalid_url;
    }
    if (pd > 0) port = (uint16_t)pv;
  }

  const char *pq = (*q && *q != '#') ? q : "/";
  size_t pq_raw_len = strlen(pq);
  const char *frag = memchr(pq, '#', pq_raw_len);
  size_t pq_len = frag ? (size_t)(frag - pq) : pq_raw_len;

  if (memchr(pq, '\r', pq_len) || memchr(pq, '\n', pq_len)) {
    ccol__mem_free(mp, userinfo_authorization);
    return ccol_http_invalid_url;
  }

  char *host = (char *)ccol__mem_alloc(mp, host_len + 1);
  if (!host) {
    ccol__mem_free(mp, userinfo_authorization);
    return ccol_not_enough_memory;
  }
  memcpy(host, host_start, host_len);
  host[host_len] = '\0';

  char *path_and_query = NULL;
  ccol_retval trv = build_request_target(mp, pq, pq_len, &path_and_query);
  if (trv != ccol_success) {
    ccol__mem_free(mp, host);
    ccol__mem_free(mp, userinfo_authorization);
    return trv;
  }

  char *bracketed_host = format_bracketed_host(mp, host, is_ipv6);
  if (!bracketed_host) {
    ccol__mem_free(mp, host);
    ccol__mem_free(mp, path_and_query);
    ccol__mem_free(mp, userinfo_authorization);
    return ccol_not_enough_memory;
  }

  int needed = snprintf(NULL, 0, "%s://%s:%u", https ? "https" : "http",
                        bracketed_host, (unsigned)port);
  if (needed < 0) {
    ccol__mem_free(mp, host);
    ccol__mem_free(mp, path_and_query);
    ccol__mem_free(mp, bracketed_host);
    ccol__mem_free(mp, userinfo_authorization);
    return ccol_unexpected_failure;
  }
  char *origin_key = (char *)ccol__mem_alloc(mp, (size_t)needed + 1);
  if (!origin_key) {
    ccol__mem_free(mp, host);
    ccol__mem_free(mp, path_and_query);
    ccol__mem_free(mp, bracketed_host);
    ccol__mem_free(mp, userinfo_authorization);
    return ccol_not_enough_memory;
  }
  snprintf(origin_key, (size_t)needed + 1, "%s://%s:%u",
           https ? "https" : "http", bracketed_host, (unsigned)port);
  ccol__mem_free(mp, bracketed_host);
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

// RFC 3986 section 5.2.4 "Remove Dot Segments" on a path. It must never see the
// query, so the caller cuts that off first and adds it back.
static char *remove_dot_segments(ccol_memmgmt_procs *mp, const char *path) {
  size_t len = strlen(path);
  char *in = (char *)ccol__mem_alloc(mp, len + 1);
  if (!in) return NULL;
  memcpy(in, path, len + 1);
  char *out = (char *)ccol__mem_alloc(mp, len + 1);
  if (!out) {
    ccol__mem_free(mp, in);
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
  ccol__mem_free(mp, in);
  return out;
}

// Merges a reference path with the base path (RFC 3986 section 5.3) for
// resolve_redirect_url, on the path only; the caller adds the query back after
// the dot segments are removed. ref_path_len is a parameter so a test can reach
// the overflow guard. Gives a new string, or NULL on failure.
static char *merge_ref_path(ccol_memmgmt_procs *mp,
                            const char *base_path_and_query,
                            const char *ref_path, size_t ref_path_len) {
  const char *base_query = strchr(base_path_and_query, '?');
  size_t base_path_len = base_query ? (size_t)(base_query - base_path_and_query)
                                    : strlen(base_path_and_query);

  size_t dir_len;
  if (ref_path_len == 0) {
    dir_len = base_path_len;
  } else {
    const char *last_slash = NULL;
    for (size_t i = 0; i < base_path_len; i++)
      if (base_path_and_query[i] == '/') last_slash = &base_path_and_query[i];
    dir_len = last_slash ? (size_t)(last_slash - base_path_and_query) + 1 : 0;
  }

  if (ref_path_len > SIZE_MAX - dir_len ||
      dir_len + ref_path_len > SIZE_MAX - 1)
    return NULL;
  char *out = (char *)ccol__mem_alloc(mp, dir_len + ref_path_len + 1);
  if (!out) return NULL;
  memcpy(out, base_path_and_query, dir_len);
  memcpy(out + dir_len, ref_path, ref_path_len);
  out[dir_len + ref_path_len] = '\0';
  return out;
}

#ifdef RUNNING_UNIT_TESTS
// Lets a test drive the overflow guard of merge_ref_path with a made-up length.
// Call it only with a length above the guard's threshold.
bool chttp__merge_ref_path_overflow_guard_for_tests(
    ccol_memmgmt_procs *mp, const char *base_path_and_query,
    const char *ref_path, size_t fake_ref_path_len) {
  char *r =
      merge_ref_path(mp, base_path_and_query, ref_path, fake_ref_path_len);
  bool rejected = (r == NULL);
  ccol__mem_free(mp, r);
  return rejected;
}
#endif

// Joins a[0..a_len) and b[0..b_len) into a new string. The lengths are
// parameters so a test can reach the overflow guard.
static char *concat_len(ccol_memmgmt_procs *mp, const char *a, size_t a_len,
                        const char *b, size_t b_len) {
  if (b_len > SIZE_MAX - a_len || a_len + b_len > SIZE_MAX - 1) return NULL;
  char *out = (char *)ccol__mem_alloc(mp, a_len + b_len + 1);
  if (!out) return NULL;
  memcpy(out, a, a_len);
  memcpy(out + a_len, b, b_len);
  out[a_len + b_len] = '\0';
  return out;
}

#ifdef RUNNING_UNIT_TESTS
// Lets a test drive the overflow guard of concat_len. Call it only with lengths
// above the guard's threshold.
bool chttp__concat_len_overflow_guard_for_tests(ccol_memmgmt_procs *mp,
                                                const char *a,
                                                size_t fake_a_len,
                                                const char *b,
                                                size_t fake_b_len) {
  char *r = concat_len(mp, a, fake_a_len, b, fake_b_len);
  bool rejected = (r == NULL);
  ccol__mem_free(mp, r);
  return rejected;
}
#endif

// True if s starts with an RFC 3986 "scheme ':'". A ':' after the first '/',
// '?' or '#' never ends a scheme. The character classes are written out by
// hand, so the locale doesn't matter.
static bool location_has_scheme(const char *s) {
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

// Removes the dot segments of the path of url, a reference with an authority
// (RFC 3986 section 5.2.2), for resolve_redirect_url. The authority, query and
// fragment are copied as they are. Gives a new string, or NULL when an
// allocation fails.
static char *normalize_authority_ref(ccol_memmgmt_procs *mp, const char *url) {
  const char *auth = strstr(url, "//");
  if (!auth) return ccol_strdup(mp, url);
  const char *path = auth + 2;
  while (*path && *path != '/' && *path != '?' && *path != '#') path++;
  size_t path_len = strcspn(path, "?#");
  if (path_len == 0) return ccol_strdup(mp, url);
  char *path_only = (char *)ccol__mem_alloc(mp, path_len + 1);
  if (!path_only) return NULL;
  memcpy(path_only, path, path_len);
  path_only[path_len] = '\0';
  char *clean = remove_dot_segments(mp, path_only);
  ccol__mem_free(mp, path_only);
  if (!clean) return NULL;
  size_t prefix_len = (size_t)(path - url);
  size_t clean_len = strlen(clean);
  const char *rest = path + path_len;
  size_t rest_len = strlen(rest);
  char *out =
      (char *)ccol__mem_alloc(mp, prefix_len + clean_len + rest_len + 1);
  if (out) {
    memcpy(out, url, prefix_len);
    memcpy(out + prefix_len, clean, clean_len);
    memcpy(out + prefix_len + clean_len, rest, rest_len + 1);
  }
  ccol__mem_free(mp, clean);
  return out;
}

// Resolves a Location value against the hop's URL (RFC 3986 sections 5.2 and
// 5.3), so any non-empty Location gives some absolute URL, as in a browser or
// curl. A reference with an unknown scheme is absolute and is refused later by
// the transport check. On NULL, *rv_out is ccol_http_transfer_aborted for a
// NULL or empty location and ccol_not_enough_memory for a failed allocation (an
// overflowing size counts as one).
static char *resolve_redirect_url_impl(ccol_memmgmt_procs *mp,
                                       const chttp_url *base,
                                       const char *location,
                                       ccol_retval *rv_out) {
  if (!location || !*location) {
    *rv_out = ccol_http_transfer_aborted;
    return NULL;
  }
  *rv_out = ccol_not_enough_memory;
  if (ccol_ascii_ncasecmp(location, "http://", 7) == 0 ||
      ccol_ascii_ncasecmp(location, "https://", 8) == 0 ||
      ccol_ascii_ncasecmp(location, "http+unix://", 12) == 0) {
    return normalize_authority_ref(mp, location);
  }
  if (location_has_scheme(location)) return ccol_strdup(mp, location);

  const char *scheme = base->is_https ? "https" : "http";

  if (location[0] == '/' && location[1] == '/') {
    int needed = snprintf(NULL, 0, "%s:%s", scheme, location);
    if (needed < 0) return NULL;
    char *out = (char *)ccol__mem_alloc(mp, (size_t)needed + 1);
    if (!out) return NULL;
    snprintf(out, (size_t)needed + 1, "%s:%s", scheme, location);
    char *clean = normalize_authority_ref(mp, out);
    ccol__mem_free(mp, out);
    return clean;
  }

  const char *frag = strchr(location, '#');
  size_t loc_len_nf = frag ? (size_t)(frag - location) : strlen(location);
  char *location_nf = (char *)ccol__mem_alloc(mp, loc_len_nf + 1);
  if (!location_nf) return NULL;
  memcpy(location_nf, location, loc_len_nf);
  location_nf[loc_len_nf] = '\0';
  location = location_nf;

  char *authority = base->is_unix
                        ? percent_encode_unix_path(mp, base->unix_socket_path)
                        : format_bracketed_host(mp, base->host, base->is_ipv6);
  if (!authority) {
    ccol__mem_free(mp, location_nf);
    return NULL;
  }

  bool default_port =
      !base->is_unix && ((base->is_https && base->port == 443) ||
                         (!base->is_https && base->port == 80));

  if (loc_len_nf == 0) {
    ccol__mem_free(mp, location_nf);
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
      ccol__mem_free(mp, authority);
      return NULL;
    }
    char *out0 = (char *)ccol__mem_alloc(mp, (size_t)needed0 + 1);
    if (!out0) {
      ccol__mem_free(mp, authority);
      return NULL;
    }
    if (base->is_unix || default_port) {
      snprintf(out0, (size_t)needed0 + 1, "%s://%s%s", out_scheme0, authority,
               base->path_and_query);
    } else {
      snprintf(out0, (size_t)needed0 + 1, "%s://%s:%u%s", out_scheme0,
               authority, (unsigned)base->port, base->path_and_query);
    }
    ccol__mem_free(mp, authority);
    return out0;
  }

  const char *ref_query = strchr(location, '?');
  size_t ref_path_len =
      ref_query ? (size_t)(ref_query - location) : strlen(location);

  char *new_path = NULL;
  if (location[0] == '/') {
    char *path_only = (char *)ccol__mem_alloc(mp, ref_path_len + 1);
    if (path_only) {
      memcpy(path_only, location, ref_path_len);
      path_only[ref_path_len] = '\0';
      new_path = remove_dot_segments(mp, path_only);
      ccol__mem_free(mp, path_only);
    }
  } else if (ref_path_len == 0) {
    new_path = merge_ref_path(mp, base->path_and_query, location, 0);
  } else {
    char *merged =
        merge_ref_path(mp, base->path_and_query, location, ref_path_len);
    if (merged) {
      new_path = remove_dot_segments(mp, merged);
      ccol__mem_free(mp, merged);
    }
  }
  if (new_path && ref_query) {
    char *with_query = concat_len(mp, new_path, strlen(new_path), ref_query,
                                  strlen(ref_query));
    if (!with_query) {
      ccol__mem_free(mp, new_path);
      new_path = NULL;
    } else {
      ccol__mem_free(mp, new_path);
      new_path = with_query;
    }
  }
  if (!new_path) {
    ccol__mem_free(mp, authority);
    ccol__mem_free(mp, location_nf);
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
    ccol__mem_free(mp, authority);
    ccol__mem_free(mp, new_path);
    ccol__mem_free(mp, location_nf);
    return NULL;
  }
  char *out = (char *)ccol__mem_alloc(mp, (size_t)needed + 1);
  if (!out) {
    ccol__mem_free(mp, authority);
    ccol__mem_free(mp, new_path);
    ccol__mem_free(mp, location_nf);
    return NULL;
  }
  if (base->is_unix || default_port) {
    snprintf(out, (size_t)needed + 1, "%s://%s%s", out_scheme, authority,
             new_path);
  } else {
    snprintf(out, (size_t)needed + 1, "%s://%s:%u%s", out_scheme, authority,
             (unsigned)base->port, new_path);
  }
  ccol__mem_free(mp, authority);
  ccol__mem_free(mp, new_path);
  ccol__mem_free(mp, location_nf);
  return out;
}

#ifdef RUNNING_UNIT_TESTS
// Set while this thread runs resolve_redirect_url, so a test allocator can fail
// exactly the allocations of the redirect resolution, on any tier and thread.
static __thread bool g_chttp_in_redirect_resolve;
// Tells a test allocator whether the calling thread is resolving a redirect, so
// it can fail exactly those allocations.
bool chttp__in_redirect_resolve_for_tests(void) {
  return g_chttp_in_redirect_resolve;
}
#endif

// Resolves location against base for every tier's redirect handling, through
// resolve_redirect_url_impl, and sets g_chttp_in_redirect_resolve around the
// call for the tests. Read *rv_out only when the result is NULL.
static char *resolve_redirect_url(ccol_memmgmt_procs *mp, const chttp_url *base,
                                  const char *location, ccol_retval *rv_out) {
#ifdef RUNNING_UNIT_TESTS
  g_chttp_in_redirect_resolve = true;
  char *r = resolve_redirect_url_impl(mp, base, location, rv_out);
  g_chttp_in_redirect_resolve = false;
  return r;
#else
  return resolve_redirect_url_impl(mp, base, location, rv_out);
#endif
}

// Decides whether we may fetch a resolved redirect target over the transport it
// names. A redirect never adds the AF_UNIX transport and never points it at
// another socket, since that would let any server reach every local socket of
// the process. An https to http downgrade is refused only when the request set
// prevent_tls_downgrade_on_redirect, because curl and Go follow it by default.
// A target that can't be parsed for lack of memory gives
// ccol_not_enough_memory, never an approval.
static ccol_retval redirect_transport_allowed(ccol_memmgmt_procs *mp,
                                              const chttp_url *base,
                                              const char *next_url,
                                              bool prevent_downgrade) {
  if (ccol_ascii_ncasecmp(next_url, "http+unix://", 12) != 0) {
    if (!prevent_downgrade || !base->is_https) return ccol_success;
    chttp_url next;
    ccol_retval prv = parse_chttp_url(mp, next_url, &next);
    if (prv == ccol_not_enough_memory) return prv;
    if (prv != ccol_success) return ccol_success;
    bool allowed = next.is_https;
    url_free(mp, &next);
    return allowed ? ccol_success : ccol_http_invalid_url;
  }
  if (!base->is_unix || !base->unix_socket_path) return ccol_http_invalid_url;
  chttp_url next;
  ccol_retval prv = parse_chttp_url(mp, next_url, &next);
  if (prv == ccol_not_enough_memory) return prv;
  if (prv != ccol_success) return ccol_success;
  bool allowed = next.is_unix && next.unix_socket_path &&
                 strcmp(next.unix_socket_path, base->unix_socket_path) == 0;
  url_free(mp, &next);
  return allowed ? ccol_success : ccol_http_invalid_url;
}

#ifdef RUNNING_UNIT_TESTS
// Runs redirect_transport_allowed with the caller's allocator, so a test can
// fail each of its allocations; the base URL is parsed with plain malloc.
ccol_retval chttp__redirect_transport_check_for_tests(ccol_memmgmt_procs *mp,
                                                      const char *base_url,
                                                      const char *next_url,
                                                      bool prevent_downgrade) {
  chttp_url base;
  ccol_retval rv = parse_chttp_url(NULL, base_url, &base);
  if (rv != ccol_success) return rv;
  rv = redirect_transport_allowed(mp, &base, next_url, prevent_downgrade);
  url_free(NULL, &base);
  return rv;
}
#endif

#ifdef RUNNING_UNIT_TESTS
// Let a test call parse_chttp_url, resolve_redirect_url and the transport
// check. chttp_url is private to this file, so they flatten the result into out
// parameters or a string, which the test frees with plain free().
ccol_retval chttp__parse_url_for_tests(
    const char *url, bool *is_https_out, bool *is_ipv6_out, char **host_out,
    uint16_t *port_out, char **path_and_query_out, char **origin_key_out,
    char **userinfo_authorization_out, bool *is_unix_out,
    char **unix_socket_path_out) {
  ccol_memmgmt_procs *mp = NULL;
  chttp_url parsed;
  ccol_retval rv = parse_chttp_url(mp, url, &parsed);
  if (rv != ccol_success) return rv;
  if (is_https_out) *is_https_out = parsed.is_https;
  if (is_ipv6_out) *is_ipv6_out = parsed.is_ipv6;
  if (port_out) *port_out = parsed.port;
  if (is_unix_out) *is_unix_out = parsed.is_unix;
  if (unix_socket_path_out)
    *unix_socket_path_out = parsed.unix_socket_path;
  else
    ccol__mem_free(mp, parsed.unix_socket_path);
  if (host_out)
    *host_out = parsed.host;
  else
    ccol__mem_free(mp, parsed.host);
  if (path_and_query_out)
    *path_and_query_out = parsed.path_and_query;
  else
    ccol__mem_free(mp, parsed.path_and_query);
  if (origin_key_out)
    *origin_key_out = parsed.origin_key;
  else
    ccol__mem_free(mp, parsed.origin_key);
  if (userinfo_authorization_out)
    *userinfo_authorization_out = parsed.userinfo_authorization;
  else
    ccol__mem_free(mp, parsed.userinfo_authorization);
  return ccol_success;
}

char *chttp__resolve_redirect_url_for_tests(const char *base_url,
                                            const char *location) {
  ccol_memmgmt_procs *mp = NULL;
  chttp_url base;
  ccol_retval rv = parse_chttp_url(mp, base_url, &base);
  if (rv != ccol_success) return NULL;
  ccol_retval rrv;
  char *result = resolve_redirect_url(mp, &base, location, &rrv);
  url_free(mp, &base);
  return result;
}

bool chttp__redirect_transport_allowed_for_tests(const char *base_url,
                                                 const char *location,
                                                 bool prevent_downgrade,
                                                 char **resolved_out) {
  ccol_memmgmt_procs *mp = NULL;
  chttp_url base;
  if (resolved_out) *resolved_out = NULL;
  ccol_retval rv = parse_chttp_url(mp, base_url, &base);
  if (rv != ccol_success) return false;
  ccol_retval rrv;
  char *resolved = resolve_redirect_url(mp, &base, location, &rrv);
  bool allowed =
      resolved && redirect_transport_allowed(mp, &base, resolved,
                                             prevent_downgrade) == ccol_success;
  url_free(mp, &base);
  if (resolved_out) {
    *resolved_out = resolved;
  } else {
    ccol__mem_free(mp, resolved);
  }
  return allowed;
}
#endif

// Request lifecycle
//
// Copies url, the body and its content type into a new request. CHTTP_ANY, a
// value outside the enum, and a NULL body->data with a non-zero length are
// refused, because none of them describes a request a server can read.
chttp_request *chttp_request_create_mp(chttp_method method, const char *url,
                                       const chttp_request_body *body,
                                       ccol_memmgmt_procs *mprocs,
                                       char **err_str) {
  if (!url) {
    if (err_str) *err_str = CCOL__ERR_STR("url must not be NULL");
    return NULL;
  }
  if ((unsigned)method >= (unsigned)CHTTP_ANY) {
    if (err_str)
      *err_str = CCOL__ERR_STR("method must name a real HTTP method");
    return NULL;
  }
  if (body && !body->data && body->len > 0) {
    if (err_str)
      *err_str =
          CCOL__ERR_STR("body->data must not be NULL when body->len > 0");
    return NULL;
  }
  if (mprocs && !ccol__verify_memmgmt_procs(mprocs, err_str)) return NULL;

  ccol_memmgmt_procs *mp = NULL;
  if (mprocs) {
    mp = (ccol_memmgmt_procs *)mprocs->malloc(sizeof(ccol_memmgmt_procs));
    if (!mp) {
      if (err_str) *err_str = CCOL__ERR_STR("failed to allocate mprocs");
      return NULL;
    }
    memcpy(mp, mprocs, sizeof(ccol_memmgmt_procs));
  }

  chttp_request *req =
      (chttp_request *)ccol__mem_alloc(mp, sizeof(chttp_request));
  if (!req) {
    if (err_str) *err_str = CCOL__ERR_STR("failed to allocate request");
    if (mp) mp->free(mp);
    return NULL;
  }
  memset(req, 0, sizeof(*req));
  req->method = method;
  req->ccol__m_procs = mp;

  req->url = ccol_strdup(mp, url);
  if (!req->url) {
    if (err_str) *err_str = CCOL__ERR_STR("failed to copy url");
    ccol__mem_free(mp, req);
    if (mp) mp->free(mp);
    return NULL;
  }

  if (body && body->data && body->len > 0) {
    void *body_copy = ccol__mem_alloc(mp, body->len);
    if (!body_copy) {
      if (err_str) *err_str = CCOL__ERR_STR("failed to copy body");
      ccol__mem_free(mp, req->url);
      ccol__mem_free(mp, req);
      if (mp) mp->free(mp);
      return NULL;
    }
    memcpy(body_copy, body->data, body->len);
    req->body.data = body_copy;
    req->body.len = body->len;
  }

  if (body && body->content_type) {
    req->body.content_type = ccol_strdup(mp, body->content_type);
    if (!req->body.content_type) {
      if (err_str) *err_str = CCOL__ERR_STR("failed to copy content_type");
      ccol__mem_free(mp, (void *)req->body.data);
      ccol__mem_free(mp, req->url);
      ccol__mem_free(mp, req);
      if (mp) mp->free(mp);
      return NULL;
    }
  }

  return req;
}

// The size of the stack buffer for a lower-cased header name. Real field names
// fit, so a header lookup allocates nothing.
enum { CHTTP_NAME_STACK_BUF = 128 };
// Lower-cases name (NUL included) into stack_buf when it fits, else into a
// buffer from mp, for the header setter and lookups, which store and find names
// in lower case. Sets *len_out to its length. Gives the buffer, or NULL when
// the allocation fails; a result other than stack_buf is freed through mp by
// the caller.
static char *lower_name_copy(ccol_memmgmt_procs *mp, const char *name,
                             char *stack_buf, size_t *len_out) {
  size_t nlen = strlen(name);
  char *lower = stack_buf;
  if (nlen >= CHTTP_NAME_STACK_BUF) {
    lower = (char *)ccol__mem_alloc(mp, nlen + 1);
    if (!lower) return NULL;
  }
  for (size_t i = 0; i <= nlen; i++)
    lower[i] = (char)ccol_ascii_tolower((unsigned char)name[i]);
  *len_out = nlen;
  return lower;
}

// Tells whether every byte of a field value is a field-vchar, a space or a tab
// (RFC 9110 section 5.5). The header setter, the serializer and the content
// type check use it, because CR and LF would split the request and other
// control bytes make recipients read it differently.
static bool header_value_is_valid(const char *v) {
  for (const unsigned char *p = (const unsigned char *)v; *p; p++)
    if ((*p < 0x20 && *p != '\t') || *p == 0x7F) return false;
  return true;
}

// Stores a header, replacing an earlier value of the same name. The name must
// be a token and the value must pass header_value_is_valid, since the
// serializer writes both as they are. Transfer-Encoding is refused, because we
// have no transfer coding for a request body.
ccol_retval chttp_request_set_header(chttp_request *req, const char *name,
                                     const char *value) {
  if (!req || !name || !value) return ccol_invalid_args;
  if (!*name) return ccol_invalid_args;
  for (const char *p = name; *p; p++) {
    if (!chttp1_is_tchar((unsigned char)*p)) return ccol_invalid_args;
  }
  if (!header_value_is_valid(value)) return ccol_invalid_args;
  if (ccol_ascii_casecmp(name, "transfer-encoding") == 0)
    return ccol_invalid_args;

  if (!req->headers) {
    char *err = NULL;
    chmap hm = chashmap_create_full(CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,
                                    ccol_string, ccol_string,
                                    req->ccol__m_procs, NULL, NULL, &err);
    if (!hm) return ccol_not_enough_memory;
    req->headers = hm;
  }

  char name_buf[CHTTP_NAME_STACK_BUF];
  size_t nlen = 0;
  char *lower = lower_name_copy(req->ccol__m_procs, name, name_buf, &nlen);
  if (!lower) return ccol_not_enough_memory;

  cmap_pair kp = {.ptr = lower, .size = nlen + 1};
  cmap_pair vp = {.ptr = (void *)value, .size = strlen(value) + 1};

  ccol_retval rv = chashmap_insert_elem((chmap)req->headers, &kp, &vp);
  if (lower != name_buf) ccol__mem_free(req->ccol__m_procs, lower);
  if (rv == ccol_key_already_present) rv = ccol_success;
  return rv;
}

// Gives the value stored for name in req, or NULL. The lookup lower-cases name,
// as the setter stores it.
const char *chttp_request_get_header(const chttp_request *req,
                                     const char *name) {
  if (!req || !name || !req->headers) return NULL;

  char name_buf[CHTTP_NAME_STACK_BUF];
  size_t nlen = 0;
  char *lower = lower_name_copy(req->ccol__m_procs, name, name_buf, &nlen);
  if (!lower) return NULL;

  cmap_pair kp = {.ptr = lower, .size = nlen + 1};
  cmap_pair vp;
  ccol_retval rv = chashmap_get_elem_ref((chmap)req->headers, &kp, &vp);
  if (lower != name_buf) ccol__mem_free(req->ccol__m_procs, lower);

  if (rv != ccol_success) return NULL;
  return (const char *)vp.ptr;
}

// Frees req with its URL, body, content type and header map. NULL is allowed.
void chttp_request_free(chttp_request *req) {
  if (!req) return;
  ccol_memmgmt_procs *mp = req->ccol__m_procs;
  ccol__mem_free(mp, req->url);
  ccol__mem_free(mp, (void *)req->body.data);
  ccol__mem_free(mp, (void *)req->body.content_type);
  if (req->headers) chashmap__destroy((chmap)req->headers);
  ccol__mem_free(mp, req);
  if (mp) mp->free(mp);
}

// Deadline helpers
//
// Makes a deadline timeout_us from now on CLOCK_MONOTONIC. 0 gives no limit,
// and a value too large for the clock saturates instead of wrapping into the
// past.
static chttp_deadline deadline_make(uint64_t timeout_us) {
  chttp_deadline d = {0};
  if (timeout_us == 0) return d;
  d.active = true;
  ccol_deadline_after_us(timeout_us, &d.deadline);
  return d;
}

// Gives the milliseconds from from to to, rounded down, for the idle pool's age
// check. It works from the full nanosecond difference, because a per-field
// computation rounds a short span across a second boundary up.
static inline int64_t ms_elapsed_floor(const struct timespec *from,
                                       const struct timespec *to) {
  int64_t ns = ((int64_t)to->tv_sec - (int64_t)from->tv_sec) * 1000000000 +
               ((int64_t)to->tv_nsec - (int64_t)from->tv_nsec);
  int64_t ms = ns / 1000000;
  if (ns < 0 && ms * 1000000 != ns) ms--;
  return ms;
}

// Gives false once d has passed at now. Otherwise *out_ms is the time left
// rounded up, so a poll(2) for that long can't end before the deadline; it's -1
// with no active deadline. The math is int64_t and saturates, whatever the
// widths of long and time_t.
static bool deadline_remaining_ms_at(const chttp_deadline *d,
                                     const struct timespec *now, int *out_ms) {
  if (!d->active) {
    *out_ms = -1;
    return true;
  }
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

// Gives the time left on d at the current time, for every Tier 1 wait. See
// deadline_remaining_ms_at for the result.
static bool deadline_remaining_ms(const chttp_deadline *d, int *out_ms) {
  if (!d->active) {
    *out_ms = -1;
    return true;
  }
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return deadline_remaining_ms_at(d, &now, out_ms);
}

#ifdef RUNNING_UNIT_TESTS
// Let a test run deadline_remaining_ms for a deadline add_sec seconds away,
// ms_elapsed_floor on two given instants, and deadline_remaining_ms_at at a
// fixed instant, so the rounding can be checked without racing the clock.
bool chttp__deadline_remaining_ms_for_tests(long long add_sec, int *out_ms) {
  chttp_deadline d = {0};
  d.active = true;
  clock_gettime(CLOCK_MONOTONIC, &d.deadline);
  d.deadline.tv_sec = (time_t)((long long)d.deadline.tv_sec + add_sec);
  return deadline_remaining_ms(&d, out_ms);
}

long long chttp__ms_elapsed_floor_for_tests(long long from_sec, long from_nsec,
                                            long long to_sec, long to_nsec) {
  struct timespec a = {.tv_sec = (time_t)from_sec, .tv_nsec = from_nsec};
  struct timespec b = {.tv_sec = (time_t)to_sec, .tv_nsec = to_nsec};
  return (long long)ms_elapsed_floor(&a, &b);
}

bool chttp__deadline_remaining_ms_at_for_tests(long long ahead_ns,
                                               int *out_ms) {
  struct timespec now = {.tv_sec = 1000, .tv_nsec = 0};
  chttp_deadline d = {0};
  d.active = true;
  d.deadline.tv_sec = (time_t)(1000 + ahead_ns / 1000000000LL);
  d.deadline.tv_nsec = (long)(ahead_ns % 1000000000LL);
  if (d.deadline.tv_nsec < 0) {
    d.deadline.tv_nsec += 1000000000L;
    d.deadline.tv_sec -= 1;
  }
  return deadline_remaining_ms_at(&d, &now, out_ms);
}

// While set, every read point of every tier waits for its fd to be readable and
// then sees its overall deadline as passed, so a test hits that case at the
// same point on each tier.
static _Atomic bool g_expire_at_read_for_tests = false;

// Turns g_expire_at_read_for_tests on or off, for the tests of a deadline that
// passes with the whole response buffered.
void chttp__set_expire_at_read_for_tests(bool on) {
  atomic_store(&g_expire_at_read_for_tests, on);
}
#endif

// Whether the request's overall deadline has passed before a read of response
// bytes. Every tier checks it there, so a response not fully read by the
// deadline ends in ccol_timed_out even when the rest is already buffered.
static bool read_deadline_passed(const chttp_deadline *overall, int fd) {
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
  return !deadline_remaining_ms(overall, &ms);
}

// Gives the tighter of two poll timeouts (-1 means no limit), so a wait can
// honour two budgets at once.
static int combine_ms(int a, int b) {
  if (a < 0) return b;
  if (b < 0) return a;
  return (a < b) ? a : b;
}

// Gives whichever of a and b passes first; an inactive deadline never wins. The
// 100-continue wait uses it to honour both its own window and the overall
// deadline.
static chttp_deadline deadline_earlier(chttp_deadline a, chttp_deadline b) {
  if (!a.active) return b;
  if (!b.active) return a;
  if (a.deadline.tv_sec != b.deadline.tv_sec)
    return (a.deadline.tv_sec < b.deadline.tv_sec) ? a : b;
  return (a.deadline.tv_nsec <= b.deadline.tv_nsec) ? a : b;
}

// Initializes *cv on CLOCK_MONOTONIC, or on the default clock when the platform
// has no condattr clock. Every deadline here is monotonic, and a timed wait on
// a wall-clock condition variable would misread it.
static int ccol_cond_var_init_monotonic(ccol__cond_var *cv) {
  ccol__cond_var_attr cv_attr;
  if (ccol__cond_var_attr_init(cv_attr) == 0) {
    ccol__cond_var_attr_setclock(cv_attr, CLOCK_MONOTONIC);
    int rv = ccol__cond_var_init_ca(*cv, cv_attr);
    ccol__cond_var_attr_destroy(cv_attr);
    return rv;
  }
  return ccol__cond_var_init(*cv);
}

#ifdef RUNNING_UNIT_TESTS
// Low-level socket and TLS I/O
//
// A test hook that fakes the TLS case a cooperating peer can't produce: An
// SSL_write that must read first, or an SSL_read that must write first (a
// renegotiation or a badly timed KeyUpdate). The mode picks the direction, and
// the flags record whether a call was blocked and whether the waiting code
// asked for the other direction, which releases it.
#define CHTTP_TLS_DIR_INJECT_OFF 0
#define CHTTP_TLS_DIR_INJECT_WRITE_WANTS_READ 1
#define CHTTP_TLS_DIR_INJECT_READ_WANTS_WRITE 2

static _Atomic int g_tls_dir_inject_mode = CHTTP_TLS_DIR_INJECT_OFF;
static _Atomic bool g_tls_dir_blocked = false;
static _Atomic bool g_tls_dir_released = false;
// A guard against a hang only: After this many refused attempts the injection
// stops refusing without marking itself released, so a broken test fails
// instead of hanging.
#define CHTTP_TLS_DIR_INJECT_MAX_BLOCKS 256
static _Atomic unsigned g_tls_dir_block_attempts = 0;
static _Atomic unsigned g_tls_dir_read_interest_while_writing = 0;
static _Atomic unsigned g_tls_dir_write_interest_while_reading = 0;

// True when a TLS attempt with home direction home_is_write must be reported as
// blocked and asking for the other direction.
static bool tls_dir_inject_blocks(bool home_is_write) {
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

// Records a wait in the waiting_for_write direction while an injected block is
// pending, so the TLS direction tests can see which direction the code waits
// in. Gives true, and releases the block, when that's the direction the
// injection asked for; for Tier 1's poll waits that also plays the peer's bytes
// arriving.
static bool tls_dir_note_wait(bool waiting_for_write) {
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
#endif

#ifdef RUNNING_UNIT_TESTS
// When non-zero, the first ctls_conn_read of each Tier 2 read dispatch asks for
// at most this many bytes, so a test can leave part of a record in the TLS
// layer.
static _Atomic size_t g_async_tls_first_read_cap_for_tests = 0;

// Sets g_async_tls_first_read_cap_for_tests, so a test can leave part of a
// record inside the TLS layer.
void chttp__set_async_tls_first_read_cap_for_tests(size_t cap) {
  atomic_store(&g_async_tls_first_read_cap_for_tests, cap);
}
#endif

// Tells whether a blocked TLS call on tls needs the write direction, for every
// loop that waits after a TLS call. It's ctls_conn_wants_write, plus the
// direction injection in a test build.
static bool conn_tls_wants_write(ctls_conn *tls) {
#ifdef RUNNING_UNIT_TESTS
  int mode = atomic_load(&g_tls_dir_inject_mode);
  if (mode != CHTTP_TLS_DIR_INJECT_OFF && atomic_load(&g_tls_dir_blocked) &&
      !atomic_load(&g_tls_dir_released))
    return (mode == CHTTP_TLS_DIR_INJECT_READ_WANTS_WRITE);
#endif
  return ctls_conn_wants_write(tls);
}

#ifdef RUNNING_UNIT_TESTS
// While above 0, each capped wait of conn_wait returns at once as if it had run
// out, so a test can check that callers wait again until their real deadline.
static atomic_int g_chttp_short_capped_waits;
// Makes the next n capped waits of conn_wait return at once, so a test can
// check that callers wait again until their real deadline.
void chttp__short_next_capped_waits_for_tests(int n) {
  atomic_store(&g_chttp_short_capped_waits, n);
}
#endif
// Polls fd for events for at most timeout_ms (-1 waits forever) and retries
// after EINTR against the original budget. ccol_timed_out only means this wait
// ran out; callers check their own deadline again, since waits are capped at
// INT_MAX ms. POLLERR or POLLNVAL gives error_rv.
static ccol_retval conn_wait(int fd, short events, int timeout_ms,
                             ccol_retval error_rv) {
#ifdef RUNNING_UNIT_TESTS
  if (tls_dir_note_wait((events & POLLOUT) != 0)) return ccol_success;
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
          int64_t elapsed_ms = ms_elapsed_floor(&start, &now);
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

// Narrows *wait_ms to what's left of c's inactivity budget, rounded up, so a
// silent peer ends the request. Gives false once the budget is used up. Only
// the waits of conn_wait_io and the duplex send count, and every byte moved
// resets it (c->inactivity_waited_ns).
static bool conn_inactivity_left_ms(const chttp_conn *c, int *wait_ms) {
  if (c->inactivity_us == 0) return true;
  uint64_t budget_ns = c->inactivity_us > UINT64_MAX / 1000
                           ? UINT64_MAX
                           : c->inactivity_us * 1000;
  if (c->inactivity_waited_ns >= budget_ns) return false;
  uint64_t left_ns = budget_ns - c->inactivity_waited_ns;
  uint64_t left_ms = left_ns / 1000000 + (left_ns % 1000000 != 0);
  *wait_ms = combine_ms(*wait_ms, left_ms > INT_MAX ? INT_MAX : (int)left_ms);
  return true;
}

// Adds the time since *start to what c's request has waited with no byte
// moving.
static void conn_inactivity_charge(chttp_conn *c,
                                   const struct timespec *start) {
  if (c->inactivity_us == 0) return;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  int64_t ns = (int64_t)(now.tv_sec - start->tv_sec) * 1000000000 +
               (now.tv_nsec - start->tv_nsec);
  if (ns > 0) c->inactivity_waited_ns += (uint64_t)ns;
}

// Waits on c's socket like conn_wait and charges the time to c's inactivity
// budget, so the inactivity timeout counts only time spent waiting on the peer.
static ccol_retval conn_wait_io(chttp_conn *c, short events, int timeout_ms,
                                ccol_retval error_rv) {
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  ccol_retval rv = conn_wait(c->fd, events, timeout_ms, error_rv);
  conn_inactivity_charge(c, &start);
  return rv;
}

// Starts the inactivity count of a request that has just taken c, fresh or from
// the pool, since the budget belongs to the request.
static void conn_inactivity_start(chttp_conn *c, uint64_t timeout_us) {
  c->inactivity_us = timeout_us;
  c->inactivity_waited_ns = 0;
}

// Reads from c through TLS or recv, for every Tier 1 read. Gives what the call
// gave, and resets c's inactivity count when bytes moved.
static ssize_t conn_read(chttp_conn *c, void *buf, size_t len) {
  if (c->tls) {
#ifdef RUNNING_UNIT_TESTS
    if (tls_dir_inject_blocks(false)) {
      errno = EWOULDBLOCK;
      return -1;
    }
#endif
    ssize_t n = ctls_conn_read(c->tls, buf, len);
    if (n < 0 && (errno == EAGAIN)) errno = EWOULDBLOCK;
    if (n > 0) c->inactivity_waited_ns = 0;
    return n;
  }
  ssize_t n;
  do {
    n = recv(c->fd, buf, len, 0);
  } while (n < 0 && errno == EINTR);
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) errno = EWOULDBLOCK;
  if (n > 0) c->inactivity_waited_ns = 0;
  return n;
}

// Writes to c through TLS or send, for every Tier 1 write; send uses
// MSG_NOSIGNAL so it never raises SIGPIPE. Gives what the call gave, and resets
// c's inactivity count when bytes moved.
static ssize_t conn_write(chttp_conn *c, const void *buf, size_t len) {
  if (c->tls) {
#ifdef RUNNING_UNIT_TESTS
    if (tls_dir_inject_blocks(true)) {
      errno = EWOULDBLOCK;
      return -1;
    }
#endif
    ssize_t n = ctls_conn_write(c->tls, buf, len);
    if (n < 0 && (errno == EAGAIN)) errno = EWOULDBLOCK;
    if (n > 0) c->inactivity_waited_ns = 0;
    return n;
  }
  ssize_t n;
  do {
    n = send(c->fd, buf, len, CCOL_MSG_NOSIGNAL);
  } while (n < 0 && errno == EINTR);
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) errno = EWOULDBLOCK;
  if (n > 0) c->inactivity_waited_ns = 0;
  return n;
}

// Sends all of data within the deadline. On TLS the direction of the next wait
// is worked out again after each blocked attempt, since a TLS write may need to
// read first. A closed peer is ccol_http_transfer_aborted.
static ccol_retval chttp_send_all(chttp_conn *conn, const char *data,
                                  size_t len, chttp_deadline *overall) {
  size_t sent = 0;
  short want_events = POLLOUT;
  while (sent < len) {
    int wait_ms;
    if (!deadline_remaining_ms(overall, &wait_ms) ||
        !conn_inactivity_left_ms(conn, &wait_ms))
      return ccol_timed_out;
    ccol_retval prv =
        conn_wait_io(conn, want_events, wait_ms, ccol_http_transfer_aborted);
    if (prv == ccol_timed_out) continue;
    if (prv != ccol_success) return prv;
    ssize_t n = conn_write(conn, data + sent, len - sent);
    if (n < 0) {
      if (errno == EWOULDBLOCK) {
        if (conn->tls)
          want_events = conn_tls_wants_write(conn->tls) ? POLLOUT : POLLIN;
        continue;
      }
      return ccol_http_transfer_aborted;
    }
    if (n == 0) return ccol_http_transfer_aborted;
    sent += (size_t)n;
    want_events = POLLOUT;
  }
  return ccol_success;
}

// Sets TCP_NODELAY on a TCP socket, best effort, because a request goes out in
// small writes that must not wait for an ACK. Every connect path goes through
// here.
static void apply_tcp_nodelay(int fd) {
  int one = 1;
  (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

// One resolved address of a TCP target, copied out of the getaddrinfo() result.
typedef struct {
  struct sockaddr_storage addr;
  socklen_t addr_len;
  int protocol;
} chttp_resolved_addr;

// The candidate addresses of one TCP target. They keep the resolver's RFC 6724
// order and addr_list_interleave then alternates the families (RFC 8305 section
// 4). Every tier connects with Happy Eyeballs: The first candidate starts at
// once, the next one when the attempts in flight have had no result for the
// attempt delay or one fails, the first to connect wins and the connect timeout
// bounds the whole race.
typedef struct {
  size_t count;
  chttp_resolved_addr addrs[];
} chttp_addr_list;

// True when host is an IPv4 or IPv6 literal, so no name lookup is needed.
static bool host_is_ip_literal(const char *host) {
  unsigned char buf[sizeof(struct in6_addr)];
  return inet_pton(AF_INET, host, buf) == 1 ||
         inet_pton(AF_INET6, host, buf) == 1;
}

// Resolves host:port into a new list from mp; an address literal is converted
// without a lookup. A name that resolves to nothing is
// ccol_http_host_resolution_failed.
static ccol_retval addr_list_resolve(ccol_memmgmt_procs *mp, const char *host,
                                     uint16_t port, chttp_addr_list **out) {
  *out = NULL;
  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (host_is_ip_literal(host)) hints.ai_flags = AI_NUMERICHOST;

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

  chttp_addr_list *l = (chttp_addr_list *)ccol__mem_alloc(
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

// Alternates the address families of l, starting with the first address's
// family and keeping each family's order. It rotates in place without
// allocating, since lists are short.
static void addr_list_interleave(chttp_addr_list *l) {
  if (l->count < 3) return;
  sa_family_t first = l->addrs[0].addr.ss_family;
  for (size_t i = 1; i < l->count; i++) {
    bool want_first = (i % 2) == 0;
    size_t j = i;
    while (j < l->count &&
           ((l->addrs[j].addr.ss_family == first) != want_first))
      j++;
    if (j == l->count) break;
    if (j != i) {
      chttp_resolved_addr moved = l->addrs[j];
      memmove(&l->addrs[i + 1], &l->addrs[i], (j - i) * sizeof(l->addrs[0]));
      l->addrs[i] = moved;
    }
  }
}

#ifdef RUNNING_UNIT_TESTS
static bool addr_is_silent_for_tests(const chttp_resolved_addr *a);
static int silent_attempt_fd_for_tests(void);
#endif

// Starts a non-blocking, close-on-exec connect to a, for one Happy Eyeballs
// attempt; a child the process spawns must not inherit the connection. Gives
// the fd, with *in_progress true while the connect is pending, or -1 when
// socket() or connect() fails at once.
static int addr_connect_start(const chttp_resolved_addr *a, bool *in_progress) {
  int fd = ccol_socket_nb(a->addr.ss_family, SOCK_STREAM, a->protocol);
  if (fd < 0) return -1;
#ifdef RUNNING_UNIT_TESTS
  if (addr_is_silent_for_tests(a)) {
    close(fd);
    *in_progress = true;
    return silent_attempt_fd_for_tests();
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
// A test hook: An attempt on an address in this set gets one end of a socket
// pair with a full send buffer and a quiet peer instead of a TCP socket, which
// looks exactly like a connect whose SYN is dropped. The peer ends (stored as
// fd + 1, 0 for an empty slot) stay open until the set is emptied.
enum { SILENT_ADDRS_FOR_TESTS = 4, SILENT_PEERS_FOR_TESTS = 32 };
static struct sockaddr_storage g_silent_addrs_for_tests[SILENT_ADDRS_FOR_TESTS];
static socklen_t g_silent_lens_for_tests[SILENT_ADDRS_FOR_TESTS];
static atomic_int g_silent_count_for_tests;
static atomic_int g_silent_peers_for_tests[SILENT_PEERS_FOR_TESTS];

// Makes one silent attempt socket for the hook above and keeps its peer.
static int silent_attempt_fd_for_tests(void) {
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

// Adds addr to the silent set (NULL empties it and closes the peers), so a test
// can make an address behave like one whose SYN is dropped.
void chttp__set_silent_addr_for_tests(const struct sockaddr *addr,
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

// Tells whether a is in the silent set, so addr_connect_start hands it a silent
// socket.
static bool addr_is_silent_for_tests(const chttp_resolved_addr *a) {
  int n = atomic_load(&g_silent_count_for_tests);
  for (int i = 0; i < n; i++) {
    if (g_silent_lens_for_tests[i] == a->addr_len &&
        memcmp(&g_silent_addrs_for_tests[i], &a->addr, a->addr_len) == 0)
      return true;
  }
  return false;
}

// While non-NULL, every TCP resolution returns this list in this order, so a
// test controls the candidates whatever /etc/hosts says.
static _Atomic(const chttp_addr_list *) g_addr_list_override_for_tests = NULL;

// Installs a fixed candidate list from addrs (NULL or n == 0 clears it), so a
// test controls what every TCP resolution gives.
void chttp__set_addr_override_for_tests(const struct sockaddr *const *addrs,
                                        const socklen_t *lens, size_t count) {
  enum { MAX_OVERRIDE_ADDRS = 8 };
  static _Alignas(max_align_t) unsigned char
      storage[sizeof(chttp_addr_list) +
              MAX_OVERRIDE_ADDRS * sizeof(chttp_resolved_addr)];
  chttp_addr_list *l = (chttp_addr_list *)(void *)storage;
  if (!addrs || count == 0) {
    atomic_store(&g_addr_list_override_for_tests, NULL);
    chttp__set_silent_addr_for_tests(NULL, 0);
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
// Runs addr_list_interleave on a list with the given families and writes the
// new order of the original indexes to order_out, so a test can check the
// interleave.
void chttp__addr_list_interleave_for_tests(const int *families, size_t n,
                                           size_t *order_out) {
  enum { MAX_ADDRS = 16 };
  static _Alignas(max_align_t) unsigned char
      storage[sizeof(chttp_addr_list) +
              MAX_ADDRS * sizeof(chttp_resolved_addr)];
  chttp_addr_list *l = (chttp_addr_list *)(void *)storage;
  if (n > MAX_ADDRS) n = MAX_ADDRS;
  memset(storage, 0, sizeof(storage));
  for (size_t i = 0; i < n; i++) {
    l->addrs[i].addr.ss_family = (sa_family_t)families[i];
    ((struct sockaddr_in *)&l->addrs[i].addr)->sin_port = htons((uint16_t)i);
  }
  l->count = n;
  addr_list_interleave(l);
  for (size_t i = 0; i < n; i++)
    order_out[i] = ntohs(((struct sockaddr_in *)&l->addrs[i].addr)->sin_port);
}
#endif

static uint64_t monotonic_now_ns(void);

#ifdef RUNNING_UNIT_TESTS
// When non-zero, every name lookup sleeps this long first, like a resolver that
// doesn't answer. Address literals aren't affected.
static _Atomic unsigned g_name_lookup_delay_ms_for_tests = 0;

// Sets g_name_lookup_delay_ms_for_tests, so a test can play a resolver that
// doesn't answer.
void chttp__set_name_lookup_delay_ms_for_tests(unsigned ms) {
  atomic_store(&g_name_lookup_delay_ms_for_tests, ms);
}

// Set by chttp__sweep_fd_wait_arm_for_tests until a sweep tick catches the
// armed hop before its connect. While set, a name lookup waits (bounded) for
// that tick after its delay, so the test doesn't depend on tick timing.
static _Atomic bool g_sweep_fd_wait_pending_for_tests = false;

// What the armed hold saw, for explaining a failure on a system that can't be
// debugged: The ctxs the sweep showed it, why each was turned away, the last
// one's fd and state, and how the lookup's wait ended. Arming resets them.
static atomic_ulong g_sweep_fd_wait_seen_for_tests;
static atomic_ulong g_sweep_fd_wait_not_armed_for_tests;
static atomic_ulong g_sweep_fd_wait_had_fd_for_tests;
static atomic_ulong g_sweep_fd_wait_wrong_state_for_tests;
static atomic_ulong g_sweep_fd_wait_had_race_for_tests;
static atomic_int g_sweep_fd_wait_last_fd_for_tests = -2;
static atomic_int g_sweep_fd_wait_last_state_for_tests = -1;
static atomic_int g_sweep_fd_wait_lookup_end_for_tests;
static atomic_int g_sweep_fd_wait_hold_end_for_tests;

static void client_deadline_note_ns(uint64_t at_ns);

// The name lookups started by all tiers (address literals don't count).
static _Atomic unsigned long g_name_lookups_for_tests = 0;

// Gives g_name_lookups_for_tests, so a test can check that a request made no
// lookup.
unsigned long chttp__name_lookups_for_tests(void) {
  return atomic_load(&g_name_lookups_for_tests);
}
#endif

// The resolution step every tier calls. It counts and delays name lookups for
// the tests, takes the override list when one is set, and interleaves the
// families.
static ccol_retval addr_list_resolve_for_target(ccol_memmgmt_procs *mp,
                                                const char *host, uint16_t port,
                                                chttp_addr_list **out) {
#ifdef RUNNING_UNIT_TESTS
  if (!host_is_ip_literal(host))
    atomic_fetch_add_explicit(&g_name_lookups_for_tests, 1,
                              memory_order_relaxed);
  unsigned delay_ms = atomic_load(&g_name_lookup_delay_ms_for_tests);
  if (delay_ms && !host_is_ip_literal(host)) {
    struct timespec ts = {(time_t)(delay_ms / 1000),
                          (long)(delay_ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
    struct timespec tick = {0, 1000000};
    bool waited = atomic_load(&g_sweep_fd_wait_pending_for_tests);
    int i = 0;
    for (; i < 5000 && atomic_load(&g_sweep_fd_wait_pending_for_tests); i++) {
      client_deadline_note_ns(monotonic_now_ns());
      nanosleep(&tick, NULL);
    }
    if (waited)
      atomic_store(&g_sweep_fd_wait_lookup_end_for_tests, i < 5000 ? 1 : 2);
    else if (atomic_load(&g_sweep_fd_wait_lookup_end_for_tests) == 0)
      atomic_store(&g_sweep_fd_wait_lookup_end_for_tests, 3);
  }
  const chttp_addr_list *ov = atomic_load(&g_addr_list_override_for_tests);
  if (ov) {
    size_t bytes = sizeof(*ov) + ov->count * sizeof(ov->addrs[0]);
    chttp_addr_list *l = (chttp_addr_list *)ccol__mem_alloc(mp, bytes);
    if (!l) return ccol_not_enough_memory;
    memcpy(l, ov, bytes);
    *out = l;
    return ccol_success;
  }
#endif
  return addr_list_resolve(mp, host, port, out);
}

// Gives the milliseconds from now until at, rounded up (0 once it's past), for
// the Happy Eyeballs timing.
static int ms_until(const struct timespec *at, const struct timespec *now) {
  chttp_deadline d = {.active = true, .deadline = *at};
  int ms;
  return deadline_remaining_ms_at(&d, now, &ms) ? ms : 0;
}

// Gives t plus ms milliseconds, for the Happy Eyeballs attempt starts.
static struct timespec timespec_add_ms(struct timespec t, long ms) {
  t.tv_sec += ms / 1000;
  t.tv_nsec += (ms % 1000) * 1000000L;
  if (t.tv_nsec >= 1000000000L) {
    t.tv_nsec -= 1000000000L;
    t.tv_sec += 1;
  }
  return t;
}

// Resolves host:port and connects with the Happy Eyeballs race, for Tier 1's
// TCP connections. Both the connect deadline and the request's overall deadline
// apply, whichever is tighter. On success *fd_out is the winner; the losing
// attempts are closed.
static ccol_retval tcp_connect(ccol_memmgmt_procs *mp, const char *host,
                               uint16_t port, chttp_deadline *connect_dl,
                               chttp_deadline *overall, int *fd_out) {
  chttp_addr_list *list = NULL;
  ccol_retval rrv = addr_list_resolve_for_target(mp, host, port, &list);
  if (rrv != ccol_success) return rrv;
  addr_list_interleave(list);

  struct pollfd stack_pfds[8];
  struct pollfd *pfds = stack_pfds;
  if (list->count > sizeof(stack_pfds) / sizeof(stack_pfds[0])) {
    pfds = (struct pollfd *)ccol__mem_alloc(mp, list->count * sizeof(*pfds));
    if (!pfds) {
      ccol__mem_free(mp, list);
      return ccol_not_enough_memory;
    }
  }
  size_t npend = 0;
  size_t next = 0;
  int winner = -1;
  ccol_retval result = ccol_http_connection_failed;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  struct timespec next_start = now;

  for (;;) {
    bool due = npend == 0 || ms_until(&next_start, &now) == 0;
    while (due && next < list->count && winner < 0) {
      bool in_progress = false;
      int fd = addr_connect_start(&list->addrs[next++], &in_progress);
      if (fd < 0) continue;
      if (!in_progress) {
        winner = fd;
        break;
      }
      pfds[npend].fd = fd;
      pfds[npend].events = POLLOUT;
      pfds[npend].revents = 0;
      npend++;
      next_start = timespec_add_ms(now, connect_attempt_delay_ms());
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
    if (!deadline_remaining_ms_at(connect_dl, &now, &wait_ms) ||
        !deadline_remaining_ms_at(overall, &now, &overall_ms)) {
      result = ccol_timed_out;
      break;
    }
    wait_ms = combine_ms(wait_ms, overall_ms);
    if (next < list->count)
      wait_ms = combine_ms(wait_ms, ms_until(&next_start, &now));

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
          take_one_for_tests(&g_connect_async_failures_for_tests))
        soerr = ECONNREFUSED;
#endif
      if (gso == 0 && soerr == 0) {
        winner = pfds[i].fd;
        continue;
      }
      close(pfds[i].fd);
      next_start = now;
    }
    npend = kept;
    if (winner >= 0) {
      result = ccol_success;
      break;
    }
  }

  for (size_t i = 0; i < npend; i++) close(pfds[i].fd);
  if (pfds != stack_pfds) ccol__mem_free(mp, pfds);
  ccol__mem_free(mp, list);
  if (result == ccol_success) {
    apply_tcp_nodelay(winner);
    *fd_out = winner;
  }
  return result;
}

// Connects to the unix stream socket at path within the same deadlines as
// tcp_connect. The socket stays non-blocking with a poll step, so the deadline
// holds even in the rare EINPROGRESS case.
static ccol_retval unix_connect(const char *path, chttp_deadline *connect_dl,
                                chttp_deadline *overall, int *fd_out) {
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

  ccol_retval prv;
  do {
    int wait_ms, overall_ms;
    if (!deadline_remaining_ms(connect_dl, &wait_ms) ||
        !deadline_remaining_ms(overall, &overall_ms)) {
      close(fd);
      return ccol_timed_out;
    }
    prv = conn_wait(fd, POLLOUT, combine_ms(wait_ms, overall_ms),
                    ccol_http_connection_failed);
  } while (prv == ccol_timed_out);
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

// Runs the client TLS handshake on conn for Tier 1, within the connect and
// overall deadlines (the handshake counts against the connect timeout, as in
// curl). A failed peer verification and a socket error during it are both
// ccol_http_tls_handshake_failed, as in Tiers 2 and 3.
static ccol_retval tls_handshake(chttp_conn *conn, chttp_deadline *connect_dl,
                                 chttp_deadline *overall) {
  for (;;) {
    ctls_handshake_result r = ctls_conn_handshake_step(conn->tls);
    if (r == CTLS_HANDSHAKE_DONE) return ccol_success;
    if (r == CTLS_HANDSHAKE_ERROR) {
      long vr = ctls_conn_verify_result(conn->tls);
      return (vr != 0) ? ccol_http_tls_cert_verification_failed
                       : ccol_http_tls_handshake_failed;
    }

    short ev = (r == CTLS_HANDSHAKE_WANT_WRITE) ? POLLOUT : POLLIN;
    int wait_ms, overall_ms;
    if (!deadline_remaining_ms(connect_dl, &wait_ms)) return ccol_timed_out;
    if (!deadline_remaining_ms(overall, &overall_ms)) return ccol_timed_out;
    ccol_retval prv = conn_wait(conn->fd, ev, combine_ms(wait_ms, overall_ms),
                                ccol_http_tls_handshake_failed);
    if (prv == ccol_timed_out) continue;
    if (prv != ccol_success) return prv;
  }
}

// Maps a failure to create the client side of a TLS connection to the code the
// request reports, from ctls's err. ctls's refusal to verify an empty host name
// is ccol_http_invalid_url (the URL can't be used for https at all); anything
// else is ccol_not_enough_memory.
static ccol_retval tls_client_create_failure_code(const char *err) {
  if (!err) return ccol_not_enough_memory;
  if (strstr(err, "verification requested with no hostname"))
    return ccol_http_invalid_url;
  if (strstr(err, "invalid arguments")) return ccol_unexpected_failure;
  return ccol_not_enough_memory;
}

// Creates the client TLS connection on fd. One trailing dot is stripped from
// the name TLS sends and verifies, as curl and Go do (SNI forbids it), while
// the Host header keeps the name as written.
static ctls_conn *tls_client_create(ctls_ctx *tls_ctx, int fd, const char *host,
                                    bool verify_host, ccol_retval *rv_out) {
  char undotted[256];
  const char *tls_name = host;
  size_t hlen = host ? strlen(host) : 0;
  if (hlen > 1 && host[hlen - 1] == '.' && hlen - 1 < sizeof(undotted)) {
    memcpy(undotted, host, hlen - 1);
    undotted[hlen - 1] = '\0';
    tls_name = undotted;
  }
  char *err = NULL;
  ctls_conn *c =
      ctls_conn_create_client(tls_ctx, fd, tls_name, verify_host, &err);
  *rv_out = c ? ccol_success : tls_client_create_failure_code(err);
  return c;
}

#ifdef RUNNING_UNIT_TESTS
// Runs tls_client_create on a socket pair and gives the code a request would
// report, so a test can reach the empty-host refusal that no URL can produce.
ccol_retval chttp__tls_client_create_code_for_tests(const char *host,
                                                    bool verify_host) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
    return ccol_unexpected_failure;
  ctls_ctx *tctx = ctls_ctx_new_mp(NULL, NULL);
  ccol_retval rv = ccol_unexpected_failure;
  if (tctx) {
    ctls_conn *c = tls_client_create(tctx, sv[0], host, verify_host, &rv);
    if (c) ctls_conn_destroy(c);
    ctls_ctx_release(tctx);
  }
  close(sv[0]);
  close(sv[1]);
  return rv;
}

// Runs a full TLS handshake over a socket pair, so a test can check SNI and
// name verification: The client verifies host against ca_path, the server
// offers default_cert and named_cert under the SNI name named_as. Gives true
// when both sides finish.
bool chttp__tls_client_handshake_for_tests(
    const char *host, const char *ca_path, const char *default_cert,
    const char *default_key, const char *named_as, const char *named_cert,
    const char *named_key) {
  int sv[2];
  if (ccol_socketpair_nb(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return false;
  bool ok = false;
  ctls_ctx *cctx = ctls_ctx_new_mp(NULL, NULL);
  ctls_ctx *sctx = ctls_ctx_new_mp(NULL, NULL);
  ctls_conn *cc = NULL, *sc = NULL;
  ccol_retval rv = ccol_unexpected_failure;
  if (cctx && sctx && ctls_ctx_trust(cctx, ca_path, NULL) == ccol_success &&
      ctls_ctx_cert_add(sctx, NULL, default_cert, default_key, NULL, NULL) ==
          ccol_success &&
      ctls_ctx_cert_add(sctx, named_as, named_cert, named_key, NULL, NULL) ==
          ccol_success) {
    cc = tls_client_create(cctx, sv[0], host, true, &rv);
    sc = ctls_conn_create_server(sctx, sv[1], NULL, NULL);
  }
  if (cc && sc) {
    bool cdone = false, sdone = false, failed = false;
    for (int i = 0; i < 2000 && !failed && !(cdone && sdone); i++) {
      if (!cdone) {
        ctls_handshake_result r = ctls_conn_handshake_step(cc);
        cdone = (r == CTLS_HANDSHAKE_DONE);
        failed = (r == CTLS_HANDSHAKE_ERROR);
      }
      if (!sdone && !failed) {
        ctls_handshake_result r = ctls_conn_handshake_step(sc);
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

// Opens a connection to url (unix or TCP, then TLS for https) into *out within
// the deadlines. tls_generation is the client's generation, read by the caller
// under cli->lock together with tls_ctx; we stamp it on the connection for the
// idle pool.
static ccol_retval conn_open(ccol_memmgmt_procs *mp, const chttp_url *url,
                             bool want_tls, ctls_ctx *tls_ctx, bool verify_host,
                             uint64_t tls_generation,
                             chttp_deadline *connect_dl,
                             chttp_deadline *overall, chttp_conn *out) {
  memset(out, 0, sizeof(*out));
  out->fd = -1;
  out->tls_generation = tls_generation;

  int fd = -1;
  ccol_retval rv =
      url->is_unix
          ? unix_connect(url->unix_socket_path, connect_dl, overall, &fd)
          : tcp_connect(mp, url->host, url->port, connect_dl, overall, &fd);
  if (rv != ccol_success) return rv;
  out->fd = fd;

  if (want_tls) {
    ccol_retval crv;
    out->tls = tls_client_create(tls_ctx, fd, url->host, verify_host, &crv);
    if (!out->tls) {
      close(fd);
      out->fd = -1;
      return crv;
    }
    rv = tls_handshake(out, connect_dl, overall);
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

// Closes connection c (TLS first) and frees its origin key, for every path that
// drops a Tier 1 connection. c's fields are reset.
static void conn_teardown(ccol_memmgmt_procs *mp, chttp_conn *c) {
  if (!c) return;
  if (c->tls) ctls_conn_destroy(c->tls);
  if (c->fd >= 0) close(c->fd);
  ccol__mem_free(mp, c->origin_key);
  memset(c, 0, sizeof(*c));
  c->fd = -1;
}

// Concurrency limiter
//
// Gives the limiter size for a configured pool size (0 picks a default from the
// CPU count), so create and chttpcli_set_pool_size agree on it.
static size_t resolve_pool_cap(size_t configured) {
  if (configured != 0) return configured;
  long np = sysconf(_SC_NPROCESSORS_ONLN);
  return (np > 0) ? (size_t)np : 1;
}

// Takes a limiter slot, blocking while the pool is full. If deadline (NULL or
// inactive for none) passes first it gives ccol_timed_out, so the request
// timeout holds while the pool is full. Fails with ccol_not_permitted once the
// client is being destroyed. Takes cli->lock.
static ccol_retval slot_acquire(struct chttpclient *cli,
                                const chttp_deadline *deadline) {
  ccol__mutex_lock(cli->lock);
  if (cli->destroying) {
    ccol__mutex_unlock(cli->lock);
    return ccol_not_permitted;
  }
  if (!cli->pool_initialized) {
    cli->pool_cap = resolve_pool_cap(cli->configured_pool_size);
    cli->pool_initialized = true;
  }
  while (cli->in_flight_count >= cli->pool_cap && !cli->destroying) {
    if (deadline && deadline->active) {
      int wrc = ccol__cond_var_timedwait(cli->available, cli->lock,
                                         deadline->deadline);
      if (wrc == ETIMEDOUT) {
        ccol__mutex_unlock(cli->lock);
        return ccol_timed_out;
      }
    } else {
      ccol__cond_var_wait(cli->available, cli->lock);
    }
  }
  if (cli->destroying) {
    ccol__mutex_unlock(cli->lock);
    return ccol_not_permitted;
  }
  cli->in_flight_count++;
  ccol__mutex_unlock(cli->lock);
  return ccol_success;
}

// Gives a limiter slot back and wakes one waiter, at the end of every Tier 1
// request. Takes cli->lock and changes in_flight_count.
static void slot_release(struct chttpclient *cli) {
  ccol__mutex_lock(cli->lock);
  cli->in_flight_count--;
  ccol__cond_var_broadcast(cli->available);
  ccol__mutex_unlock(cli->lock);
}

// Keep-alive idle pool
//
// Reads a cvec handle stored in a map value through memcpy, the pattern every
// module uses for pointers it keeps in a chmap.
static inline cvec read_cvec(const void *src) {
  cvec v;
  memcpy(&v, src, sizeof(v));
  return v;
}

// Pops a usable idle connection for origin_key into *out, or gives false.
// Stale, dead and TLS-outdated candidates are closed on the way. The age check
// and the liveness probe run outside cli->lock, since we never do I/O under it.
// An origin whose list runs empty loses its map entry.
static bool idle_pool_take(struct chttpclient *cli, const char *origin_key,
                           chttp_conn *out) {
  for (;;) {
    bool got = false;
    uint64_t cur_tls_generation;
    ccol__mutex_lock(cli->lock);
    cur_tls_generation = cli->tls_generation;
    if (cli->idle_pools) {
      cmap_pair kp = {.ptr = (void *)origin_key,
                      .size = strlen(origin_key) + 1};
      cmap_pair vp;
      if (chashmap_get_elem_ref(cli->idle_pools, &kp, &vp) == ccol_success) {
        cvec list = read_cvec(vp.ptr);
        if (list && cvector_elem_count(list) > 0 &&
            cvector_pop_back(list, out) == ccol_success) {
          got = true;
          if (cli->idle_total_count > 0) cli->idle_total_count--;
          if (cvector_elem_count(list) == 0) {
            cvector__destroy(list);
            chashmap_delete_elem(cli->idle_pools, &kp);
          }
        }
      }
    }
    ccol__mutex_unlock(cli->lock);
    if (!got) return false;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t age_ms = ms_elapsed_floor(&out->last_used, &now);

    bool tls_config_current =
        (!out->tls || out->tls_generation == cur_tls_generation);

    bool alive = false;
    if (age_ms <= CHTTP_IDLE_MAX_AGE_MS && tls_config_current) {
      char probe;
      ssize_t pn;
      if (out->tls) {
        pn = ctls_conn_read(out->tls, &probe, 1);
      } else {
        do {
          pn = recv(out->fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
        } while (pn < 0 && errno == EINTR);
      }
      alive = (pn < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    }
    if (alive) return true;

    conn_teardown(cli->m_procs, out);
  }
}

// Gives a good connection back to cli's idle pool, so the next request to the
// same origin skips the connect. It closes c instead when a cap is full, the
// origin cap leaves no room for a new origin, or the client is being destroyed.
// Takes cli->lock and changes idle_pools and idle_total_count.
static void idle_pool_offer(struct chttpclient *cli, chttp_conn *c) {
  bool pooled = false;
  ccol__mutex_lock(cli->lock);
  if (!cli->destroying && cli->idle_pools &&
      cli->idle_total_count < CHTTP_MAX_IDLE_TOTAL) {
    cmap_pair kp = {.ptr = c->origin_key, .size = strlen(c->origin_key) + 1};
    cmap_pair vp;
    cvec list = NULL;
    if (chashmap_get_elem_ref(cli->idle_pools, &kp, &vp) == ccol_success) {
      list = read_cvec(vp.ptr);
    }
    if (!list &&
        chashmap_elem_count(cli->idle_pools) < effective_max_idle_origins()) {
      char *cverr = NULL;
      list = cvector_create_full(sizeof(chttp_conn), cli->m_procs, &cverr);
      if (list) {
        cmap_pair vp2 = {.ptr = &list, .size = sizeof(list)};
        if (chashmap_insert_elem(cli->idle_pools, &kp, &vp2) != ccol_success) {
          cvector__destroy(list);
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
  ccol__mutex_unlock(cli->lock);
  if (!pooled) conn_teardown(cli->m_procs, c);
}

// Request serialization
//
// Which of the headers serialize_request has a default for are present, found
// in one case-insensitive pass over the map (a caller may lend a map whose keys
// keep their natural case). idempotency_key and connection_close feed the retry
// and pooling decisions.
typedef struct {
  bool has_host;
  bool has_accept;
  bool has_user_agent;
  bool has_content_length;
  bool has_content_type;
  bool has_authorization;
  bool has_expect;
  bool has_transfer_encoding;
  bool has_idempotency_key;
  bool request_close;
} chttp_header_presence;

// Tells whether a Connection value v lists the "close" token (RFC 9110 section
// 7.6.1, ASCII case ignored), so a request that asks for close is never pooled.
static bool connection_value_has_close(const char *v) {
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

// Scans headers once for the names serialize_request and the retry rules care
// about, case ignored, since a borrowed map may keep natural case. Gives the
// flags; *failed is set when the map iterator can't be allocated, and the flags
// are unknown then.
static chttp_header_presence scan_header_presence(chmap headers, bool *failed) {
  chttp_header_presence p = {0};
  *failed = false;
  if (!headers) return p;
  char *ierr = NULL;
  cmap_iterator *it = chashmap_begin_iter(headers, &ierr);
  if (!it && ierr) {
    *failed = true;
    return p;
  }
  for (; it; it = it->ccol__next_fn(it)) {
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
             connection_value_has_close((const char *)it->val_pair->ptr))
      p.request_close = true;
  }
  return p;
}

// True when a request that went out in full on a reused connection that died
// without an answer may be sent again: An idempotent method (GET, HEAD,
// OPTIONS, PUT, DELETE) or an Idempotency-Key or X-Idempotency-Key header, as
// in Go.
static bool request_is_replayable(chttp_method method,
                                  const chttp_header_presence *hp) {
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

// The header names serialize_request has sent, compared ignoring case, so a
// borrowed map with both "Host" and "host" puts one line on the wire. The names
// are borrowed.
typedef struct {
  const char **names;
  size_t count;
  size_t cap;
} chttp_seen_names;

// Tells whether seen already holds name (ASCII case ignored), so the serializer
// writes each header name once.
static bool seen_names_contains(const chttp_seen_names *seen,
                                const char *name) {
  for (size_t i = 0; i < seen->count; i++)
    if (ccol_ascii_casecmp(seen->names[i], name) == 0) return true;
  return false;
}

// Records the borrowed name in seen, so later names that differ only in case
// are skipped. Gives false when the array can't grow (the size computation is
// overflow-guarded).
static bool seen_names_add(ccol_memmgmt_procs *mp, chttp_seen_names *seen,
                           const char *name) {
  if (seen->count == seen->cap) {
    if (seen->cap > SIZE_MAX / 2) return false;
    size_t nc = seen->cap ? seen->cap * 2 : 8;
    if (nc > SIZE_MAX / sizeof(*seen->names)) return false;
    const char **nn = (const char **)ccol__mem_realloc(mp, (void *)seen->names,
                                                       nc * sizeof(*nn));
    if (!nn) return false;
    seen->names = nn;
    seen->cap = nc;
  }
  seen->names[seen->count++] = name;
  return true;
}

#ifdef RUNNING_UNIT_TESTS
// Lets a test drive the overflow guard of seen_names_add with a made-up
// capacity and no real array.
bool chttp__seen_names_add_overflow_guard_for_tests(ccol_memmgmt_procs *mp,
                                                    size_t fake_cap) {
  chttp_seen_names seen = {.names = NULL, .count = fake_cap, .cap = fake_cap};
  bool added = seen_names_add(mp, &seen, "x");
  ccol__mem_free(mp, (void *)seen.names);
  return !added;
}
#endif

// A growable output buffer. oom latches the first failed growth.
typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  ccol_memmgmt_procs *mp;
  bool oom;
} chttp_outbuf;

// Gives the next capacity for a buffer of cap bytes that needs need (> cap):
// initial for an empty one, else doubled until it fits. A doubling past
// SIZE_MAX gives need, so the capacity never wraps (on ILP32 2 GiB doubles to
// 0).
static size_t chttp_grow_capacity(size_t cap, size_t need, size_t initial) {
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

// Appends n bytes to b, for building the request bytes. On an overflowing size
// or a failed growth it sets b->oom and leaves the content as it was.
static void ob_append(chttp_outbuf *b, const char *data, size_t n) {
  if (b->oom) return;
  if (n > SIZE_MAX - b->len || b->len + n > SIZE_MAX - 1) {
    b->oom = true;
    return;
  }
  size_t need = b->len + n + 1;
  if (need > b->cap) {
    size_t nc = chttp_grow_capacity(b->cap, need, 1024);
    char *nb = (char *)ccol__mem_realloc(b->mp, b->buf, nc);
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

// Appends the NUL-terminated string s to b, for the literal parts of the
// request.
static void ob_append_cstr(chttp_outbuf *b, const char *s) {
  ob_append(b, s, strlen(s));
}

#ifdef RUNNING_UNIT_TESTS
// Let a test drive ob_append: One drives the overflow guard with a made-up
// length and a counting allocator, the other grows from a capacity whose
// doubling would pass SIZE_MAX. Both give whether b->oom was set.
bool chttp__ob_append_overflow_guard_for_tests(ccol_memmgmt_procs *mp,
                                               size_t fake_len, size_t n) {
  chttp_outbuf ob = {.mp = mp, .len = fake_len};
  char probe[64] = {0};
  if (n > sizeof(probe)) n = sizeof(probe);
  ob_append(&ob, probe, n);
  bool oom = ob.oom;
  ccol__mem_free(mp, ob.buf);
  return oom;
}

bool chttp__ob_append_cap_overflow_for_tests(ccol_memmgmt_procs *mp,
                                             size_t fake_cap) {
  chttp_outbuf ob = {.mp = mp, .len = fake_cap - 1, .cap = fake_cap};
  char probe[8] = {0};
  ob_append(&ob, probe, sizeof(probe));
  return ob.oom;
}
#endif

// Which caller-set headers a redirect hop leaves off. Every tier judges each
// hop against the first request's origin, never the previous hop (curl's rule).
// DROP_CREDENTIALS: Scheme, host or port differ, so Authorization, Cookie,
// Cookie2 and WWW-Authenticate go. DROP_HOST: The host differs, so a caller-set
// Host goes too.
#define CHTTP_REDIRECT_DROP_CREDENTIALS 0x1u
#define CHTTP_REDIRECT_DROP_HOST 0x2u

// Finds the host part of an origin key (between "://" and the port for TCP, the
// whole path for "unix://"), so redirect_header_drops can compare hosts. *start
// and *len describe it.
static void origin_key_host_span(const char *key, const char **start,
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

// Tells whether two origin keys are the same origin for carrying credentials
// across a redirect. A TCP key compares ignoring ASCII case, like DNS names; a
// "unix://" key compares byte for byte, like a file name.
static bool origin_keys_same(const char *a, const char *b) {
  bool a_unix = strncmp(a, "unix://", 7) == 0;
  bool b_unix = strncmp(b, "unix://", 7) == 0;
  if (a_unix || b_unix) return a_unix == b_unix && strcmp(a, b) == 0;
  return ccol_ascii_casecmp(a, b) == 0;
}

// Gives the CHTTP_REDIRECT_DROP_* flags for a hop to origin_key, judged against
// the first request's origin, so every tier drops the same headers.
static unsigned redirect_header_drops(const char *initial_origin_key,
                                      const char *origin_key) {
  if (!initial_origin_key || origin_keys_same(initial_origin_key, origin_key))
    return 0;
  unsigned drops = CHTTP_REDIRECT_DROP_CREDENTIALS;
  const char *a, *b;
  size_t alen, blen;
  origin_key_host_span(initial_origin_key, &a, &alen);
  origin_key_host_span(origin_key, &b, &blen);
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

// True for the headers CHTTP_REDIRECT_DROP_CREDENTIALS takes off.
static bool is_redirect_credential_header(const char *name) {
  return ccol_ascii_casecmp(name, "authorization") == 0 ||
         ccol_ascii_casecmp(name, "cookie") == 0 ||
         ccol_ascii_casecmp(name, "cookie2") == 0 ||
         ccol_ascii_casecmp(name, "www-authenticate") == 0;
}

// Serializes the request line, the headers and the body into one buffer. req is
// a per-hop view, so a redirect changes the method and body there. It refuses
// an invalid method, a Transfer-Encoding header, a value that fails
// header_value_is_valid and a failed header walk; it adds Host, Content-Length,
// Content-Type and the URL's Authorization unless the caller set them, and
// writes each name once. out_presence (may be NULL) gets the header presence it
// computed.
static ccol_retval serialize_request(
    ccol_memmgmt_procs *mp, const chttp_request *req, const chttp_url *url,
    const char *auto_authorization, unsigned redirect_drops, char **out_buf,
    size_t *out_len, chttp_header_presence *out_presence) {
  if ((unsigned)req->method >= (unsigned)CHTTP_ANY) return ccol_invalid_args;
  chttp_outbuf ob = {.mp = mp};

  ob_append_cstr(&ob, chttp_method_str(req->method));
  ob_append(&ob, " ", 1);
  ob_append_cstr(&ob, url->path_and_query);
  ob_append_cstr(&ob, " HTTP/1.1\r\n");

  bool scan_failed;
  chttp_header_presence hp =
      scan_header_presence((chmap)req->headers, &scan_failed);
  if (scan_failed) {
    ccol__mem_free(mp, ob.buf);
    return ccol_not_enough_memory;
  }
  if (hp.has_transfer_encoding) {
    ccol__mem_free(mp, ob.buf);
    return ccol_invalid_args;
  }
  bool has_host = hp.has_host && !(redirect_drops & CHTTP_REDIRECT_DROP_HOST);
  bool has_accept = hp.has_accept;
  bool has_ua = hp.has_user_agent;
  bool has_cl = hp.has_content_length;
  bool has_ct = hp.has_content_type;
  bool has_auth = hp.has_authorization &&
                  !(redirect_drops & CHTTP_REDIRECT_DROP_CREDENTIALS);

  if (!has_host) {
    if (url->is_unix) {
      ob_append_cstr(&ob, "host: localhost\r\n");
    } else {
      bool default_port = (url->is_https && url->port == 443) ||
                          (!url->is_https && url->port == 80);
      ob_append_cstr(&ob, "host: ");
      if (url->is_ipv6) ob_append(&ob, "[", 1);
      ob_append_cstr(&ob, url->host);
      if (url->is_ipv6) ob_append(&ob, "]", 1);
      if (!default_port) {
        char portbuf[16];
        int pn = snprintf(portbuf, sizeof(portbuf), ":%u", (unsigned)url->port);
        if (pn > 0) ob_append(&ob, portbuf, (size_t)pn);
      }
      ob_append(&ob, "\r\n", 2);
    }
  }
  if (!has_accept) ob_append_cstr(&ob, "accept: */*\r\n");
  if (!has_ua)
    ob_append_cstr(&ob, "user-agent: c_collections-chttpclient/1.0\r\n");
  if (!has_auth && auto_authorization) {
    ob_append_cstr(&ob, "authorization: ");
    ob_append_cstr(&ob, auto_authorization);
    ob_append(&ob, "\r\n", 2);
  }

  bool body_carrying_method =
      (req->method == CHTTP_POST || req->method == CHTTP_PUT ||
       req->method == CHTTP_PATCH);

  if (req->headers) {
    chttp_seen_names seen = {0};
    char *ierr = NULL;
    cmap_iterator *it = chashmap_begin_iter((chmap)req->headers, &ierr);
    if (!it && ierr) {
      ccol__mem_free(mp, ob.buf);
      return ccol_not_enough_memory;
    }
    for (; it; it = it->ccol__next_fn(it)) {
      const char *name = (const char *)it->key_pair->ptr;
      const char *val = (const char *)it->val_pair->ptr;
      if (!*name || !header_value_is_valid(val)) {
        ccol_iter_destroy(it);
        ccol__mem_free(mp, (void *)seen.names);
        ccol__mem_free(mp, ob.buf);
        return ccol_invalid_args;
      }
      for (const char *p = name; *p; p++) {
        if (!chttp1_is_tchar((unsigned char)*p)) {
          ccol_iter_destroy(it);
          ccol__mem_free(mp, (void *)seen.names);
          ccol__mem_free(mp, ob.buf);
          return ccol_invalid_args;
        }
      }
      if (!body_carrying_method &&
          (ccol_ascii_casecmp(name, "content-length") == 0 ||
           ccol_ascii_casecmp(name, "content-type") == 0 ||
           ccol_ascii_casecmp(name, "expect") == 0))
        continue;
      if ((redirect_drops & CHTTP_REDIRECT_DROP_CREDENTIALS) &&
          is_redirect_credential_header(name))
        continue;
      if ((redirect_drops & CHTTP_REDIRECT_DROP_HOST) &&
          ccol_ascii_casecmp(name, "host") == 0)
        continue;
      if (seen_names_contains(&seen, name)) continue;
      if (body_carrying_method &&
          ccol_ascii_casecmp(name, "content-length") == 0) {
        size_t expected = req->body.data ? req->body.len : (size_t)0;
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
          ccol__mem_free(mp, (void *)seen.names);
          ccol__mem_free(mp, ob.buf);
          return ccol_invalid_args;
        }
      }
      if (!seen_names_add(mp, &seen, name)) {
        ccol_iter_destroy(it);
        ccol__mem_free(mp, (void *)seen.names);
        ccol__mem_free(mp, ob.buf);
        return ccol_not_enough_memory;
      }
      ob_append_cstr(&ob, name);
      ob_append(&ob, ": ", 2);
      ob_append_cstr(&ob, val);
      ob_append(&ob, "\r\n", 2);
    }
    ccol__mem_free(mp, (void *)seen.names);
  }

  if (!has_ct && body_carrying_method && req->body.content_type) {
    if (!header_value_is_valid(req->body.content_type)) {
      ccol__mem_free(mp, ob.buf);
      return ccol_invalid_args;
    }
    ob_append_cstr(&ob, "content-type: ");
    ob_append_cstr(&ob, req->body.content_type);
    ob_append(&ob, "\r\n", 2);
  }

  if (body_carrying_method && !has_cl) {
    char clbuf[48];
    int cln = snprintf(clbuf, sizeof(clbuf), "content-length: %zu\r\n",
                       req->body.data ? req->body.len : (size_t)0);
    if (cln > 0) ob_append(&ob, clbuf, (size_t)cln);
  }

  bool has_expect = hp.has_expect;
  if (!has_expect && req->expect_continue && body_carrying_method &&
      req->body.data && req->body.len > 0) {
    ob_append_cstr(&ob, "expect: 100-continue\r\n");
  }

  ob_append(&ob, "\r\n", 2);

  if (body_carrying_method && req->body.data && req->body.len > 0)
    ob_append(&ob, (const char *)req->body.data, req->body.len);

  if (ob.oom) {
    ccol__mem_free(mp, ob.buf);
    return ccol_not_enough_memory;
  }
  *out_buf = ob.buf;
  *out_len = ob.len;
  if (out_presence) *out_presence = hp;
  return ccol_success;
}

// chttp1_parser integration
//
// A body sink that throws the bytes away, for a redirect response whose body
// nobody reads. Gives len.
static size_t sink_discard(const void *data, size_t len, void *ctx) {
  (void)data;
  (void)ctx;
  return len;
}

// The buffered body sink, which collects the body of a non-streaming request
// into the chttp_bodybuf in ctx. It gives a short count when the size would
// overflow, growth fails, or the cap of chttpcli_set_max_response_body_size is
// passed (whatever the framing), which stops the parse.
static size_t sink_buffered(const void *data, size_t len, void *ctx) {
  chttp_bodybuf *bb = (chttp_bodybuf *)ctx;
  if (len == 0) return 0;
  if (len > SIZE_MAX - bb->len || bb->len + len > SIZE_MAX - 1) {
    bb->oom = true;
    return 0;
  }
  if (bb->max_size > 0 && bb->len + len > bb->max_size) {
    bb->too_large = true;
    return 0;
  }
  if (bb->len + len + 1 > bb->cap) {
    size_t nc = chttp_grow_capacity(bb->cap, bb->len + len + 1, 4096);
    char *nb = (char *)ccol__mem_realloc(bb->mp, bb->buf, nc);
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
// Let a test drive sink_buffered: Its overflow guard with a made-up length, and
// a growth from a capacity whose doubling would pass SIZE_MAX.
bool chttp__sink_buffered_overflow_guard_for_tests(ccol_memmgmt_procs *mp,
                                                   size_t fake_len) {
  chttp_bodybuf bb = {0};
  bb.mp = mp;
  bb.len = fake_len;
  char probe[8] = {0};
  size_t n = sink_buffered(probe, sizeof(probe), &bb);
  bool rejected = (n != sizeof(probe)) && bb.oom && !bb.too_large;
  ccol__mem_free(mp, bb.buf);
  return rejected;
}

bool chttp__sink_buffered_cap_overflow_for_tests(ccol_memmgmt_procs *mp,
                                                 size_t fake_cap) {
  chttp_bodybuf bb = {0};
  bb.mp = mp;
  bb.len = fake_cap - 1;
  bb.cap = fake_cap;
  char probe[8] = {0};
  size_t n = sink_buffered(probe, sizeof(probe), &bb);
  return (n == 0) && bb.oom && !bb.too_large;
}
#endif

#ifdef RUNNING_UNIT_TESTS
// How many field record headers the code below has read; a test keeps it at or
// below the record count, so nothing rescans the block.
static _Atomic unsigned long g_field_rec_reads_for_tests = 0;
// Gives g_field_rec_reads_for_tests, so a test can check that no operation
// rescans the field lines.
unsigned long chttpcli__field_rec_reads_for_tests(void) {
  return atomic_load_explicit(&g_field_rec_reads_for_tests,
                              memory_order_relaxed);
}
#endif

// Read a field record (its header at off, its name and its value) and give the
// padded size of a record, for the field-lines code below.
static inline chttp_field_rec field_rec_at(const chttp_field_lines *lines,
                                           uint32_t off) {
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&g_field_rec_reads_for_tests, 1,
                            memory_order_relaxed);
#endif
  chttp_field_rec r;
  memcpy(&r, lines->data + off, sizeof(r));
  return r;
}

static inline const char *field_rec_name(const chttp_field_lines *lines,
                                         uint32_t off) {
  return lines->data + off + sizeof(chttp_field_rec);
}

static inline const char *field_rec_value(const chttp_field_lines *lines,
                                          uint32_t off,
                                          const chttp_field_rec *r) {
  return field_rec_name(lines, off) + r->name_len + 1;
}

static inline size_t field_rec_size(size_t name_len, size_t value_len) {
  size_t n = sizeof(chttp_field_rec) + name_len + 1 + value_len + 1;
  return (n + 3) & ~(size_t)3;
}

// Frees a field-lines block, when a response or a parse context goes away.
static void field_lines_free(ccol_memmgmt_procs *mp, chttp_field_lines *lines) {
  if (!lines) return;
  ccol__mem_free(mp, lines->index);
  ccol__mem_free(mp, lines);
}

// Picks the index slot of a lower-case name for the field-lines table: FNV-1a
// over it, then a multiply whose high bits pick the slot, so every byte of the
// name counts.
static inline size_t field_name_slot(const char *name, size_t name_len,
                                     size_t index_cap) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < name_len; i++) {
    h ^= (unsigned char)name[i];
    h *= 1099511628211ULL;
  }
  h *= 0x9E3779B97F4A7C15ULL;
  return (size_t)(h >> 32) & (index_cap - 1);
}

// Finds the index slot of a lower-case name, for adding and reading
// occurrences. Gives NULL when the name has no records.
static chttp_field_slot *field_lines_find(const chttp_field_lines *lines,
                                          const char *name, size_t name_len) {
  if (!lines || !lines->index) return NULL;
  size_t mask = lines->index_cap - 1;
  for (size_t i = field_name_slot(name, name_len, lines->index_cap);;
       i = (i + 1) & mask) {
    chttp_field_slot *slot = &lines->index[i];
    if (!slot->head) return NULL;
    uint32_t off = slot->head - 1;
    chttp_field_rec r = field_rec_at(lines, off);
    if (r.name_len == name_len &&
        memcmp(field_rec_name(lines, off), name, name_len) == 0)
      return slot;
  }
}

// Makes room for need more record bytes and one more name in the index (kept at
// most half full), before field_lines_put. A failed allocation gives false and
// leaves every record and slot as it was.
static bool field_lines_reserve(ccol_memmgmt_procs *mp,
                                chttp_field_lines **linesp, size_t need) {
  chttp_field_lines *lines = *linesp;
  size_t len = lines ? lines->len : 0;
  size_t cap = lines ? lines->cap : 0;
  if (cap - len < need) {
    size_t new_cap = cap ? cap * 2 : 256;
    while (new_cap - len < need) new_cap *= 2;
    chttp_field_lines *grown = (chttp_field_lines *)ccol__mem_realloc(
        mp, lines, sizeof(chttp_field_lines) + new_cap);
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
  if ((lines->names + 1) * 2 > lines->index_cap) {
    size_t new_icap = lines->index_cap ? lines->index_cap * 2 : 16;
    chttp_field_slot *idx = (chttp_field_slot *)ccol__mem_calloc(
        mp, new_icap, sizeof(chttp_field_slot));
    if (!idx) return false;
    for (size_t i = 0; i < lines->index_cap; i++) {
      chttp_field_slot old = lines->index[i];
      if (!old.head) continue;
      chttp_field_rec r = field_rec_at(lines, old.head - 1);
      size_t j = field_name_slot(field_rec_name(lines, old.head - 1),
                                 r.name_len, new_icap);
      while (idx[j].head) j = (j + 1) & (new_icap - 1);
      idx[j] = old;
    }
    ccol__mem_free(mp, lines->index);
    lines->index = idx;
    lines->index_cap = new_icap;
  }
  return true;
}

// Appends one record into room that field_lines_reserve made, and gives its
// offset for the chain.
static uint32_t field_lines_put(chttp_field_lines *lines, const char *name,
                                size_t name_len, const char *value,
                                size_t value_len) {
  uint32_t off = (uint32_t)lines->len;
  chttp_field_rec r = {.name_len = (uint32_t)name_len,
                       .value_len = (uint32_t)value_len,
                       .next = 0};
  char *dst = lines->data + off;
  memcpy(dst, &r, sizeof(r));
  dst += sizeof(r);
  memcpy(dst, name, name_len);
  dst[name_len] = '\0';
  memcpy(dst + name_len + 1, value, value_len);
  dst[name_len + 1 + value_len] = '\0';
  lines->len += field_rec_size(name_len, value_len);
  return off;
}

// Records one more occurrence of a lower-case name; for a name seen for the
// first time, first_value (the map's value) is recorded first. A failed
// allocation gives false and leaves *linesp untouched. Out of line, since
// repeated names are rare.
__attribute__((noinline, cold)) static bool field_lines_add(
    ccol_memmgmt_procs *mp, chttp_field_lines **linesp, const char *name,
    size_t name_len, const char *first_value, size_t first_len,
    const char *value, size_t value_len) {
  chttp_field_slot *slot = field_lines_find(*linesp, name, name_len);
  size_t need = field_rec_size(name_len, value_len);
  if (!slot) need += field_rec_size(name_len, first_len);
  if (!field_lines_reserve(mp, linesp, need)) return false;
  chttp_field_lines *lines = *linesp;
  if (!slot) {
    uint32_t head =
        field_lines_put(lines, name, name_len, first_value, first_len);
    size_t j = field_name_slot(name, name_len, lines->index_cap);
    while (lines->index[j].head) j = (j + 1) & (lines->index_cap - 1);
    slot = &lines->index[j];
    slot->head = head + 1;
    slot->tail = head + 1;
    lines->names++;
  } else {
    slot = field_lines_find(lines, name, name_len);
  }
  uint32_t off = field_lines_put(lines, name, name_len, value, value_len);
  uint32_t tail = slot->tail - 1;
  chttp_field_rec tr = field_rec_at(lines, tail);
  tr.next = off + 1;
  memcpy(lines->data + tail, &tr, sizeof(tr));
  slot->tail = off + 1;
  return true;
}

// Replaces the map value of every repeated name with all occurrences joined by
// ", " (RFC 9110 section 5.3), once per response. Set-Cookie keeps its first
// occurrence. False means an allocation failed.
static bool field_lines_combine(chttp_parse_ctx *ctx) {
  const chttp_field_lines *lines = ctx->field_lines;
  for (size_t i = 0; i < lines->index_cap; i++) {
    const chttp_field_slot *slot = &lines->index[i];
    if (!slot->head) continue;
    uint32_t off = slot->head - 1;
    chttp_field_rec r = field_rec_at(lines, off);
    const char *name = field_rec_name(lines, off);
    if (r.name_len == sizeof("set-cookie") - 1 &&
        memcmp(name, "set-cookie", r.name_len) == 0)
      continue;
    size_t total = 0;
    for (uint32_t o = slot->head; o;) {
      chttp_field_rec cr = field_rec_at(lines, o - 1);
      total += cr.value_len + 2;
      o = cr.next;
    }
    char *combined = (char *)ccol__mem_alloc(ctx->mp, total - 1);
    if (!combined) return false;
    size_t pos = 0;
    for (uint32_t o = slot->head; o;) {
      chttp_field_rec cr = field_rec_at(lines, o - 1);
      if (o != slot->head) {
        combined[pos++] = ',';
        combined[pos++] = ' ';
      }
      memcpy(combined + pos, field_rec_value(lines, o - 1, &cr), cr.value_len);
      pos += cr.value_len;
      o = cr.next;
    }
    combined[pos] = '\0';
    cmap_pair kp = {.ptr = (void *)name, .size = r.name_len + 1};
    cmap_pair vp = {.ptr = combined, .size = pos + 1};
    ccol_retval rv = chashmap_insert_elem(ctx->headers, &kp, &vp);
    ccol__mem_free(ctx->mp, combined);
    if (rv != ccol_key_already_present && rv != ccol_success) return false;
  }
  return true;
}

// Stores one response header line (the parser's header callback; trailers have
// no callback). The first occurrence of a name goes into the map, later ones
// into ctx->field_lines. Location is captured before any combining, so a
// redirect follows the first one.
static int http1_on_header(chttp1_parser *p, const char *name, size_t name_len,
                           const char *value, size_t value_len) {
  chttp_parse_ctx *ctx = (chttp_parse_ctx *)p->data;

  char stack_buf[CHTTP1_MAX_LINE_LEN + 1];
  size_t need = name_len + 1 + value_len + 1;
  char *buf = stack_buf;
  if (need > sizeof(stack_buf)) {
    buf = (char *)ccol__mem_alloc(ctx->mp, need);
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
  ccol_retval rv = ccol_chmap_insert_or_get_elem(ctx->headers, &kp, &vp, &slot);
  if (rv != ccol_success &&
      (rv != ccol_key_already_present ||
       !field_lines_add(ctx->mp, &ctx->field_lines, lower_name, name_len,
                        (const char *)slot.ptr, slot.size - 1, value_copy,
                        value_len))) {
    ctx->error = true;
    ret = 1;
  }
  if (buf != stack_buf) ccol__mem_free(ctx->mp, buf);
  return ret;
}

// Checks the response once its header block is complete, before any body byte
// reaches the sink. It fails a 101 (we never send Upgrade) and a final response
// with a transfer coding other than chunked (the parser only decodes chunked),
// decides whether this hop redirects (ctx->will_redirect, ctx->location),
// refuses a declared body over the size cap, and combines repeated fields.
static int http1_on_headers_complete(chttp1_parser *p) {
  chttp_parse_ctx *ctx = (chttp_parse_ctx *)p->data;
  ctx->status_code = p->status_code;

  if (p->status_code == 101) return -1;

  if (p->status_code >= 200 && chttp1_has_other_transfer_coding(p)) return -1;

  bool is_redirect_status = ctx->status_code == 301 ||
                            ctx->status_code == 302 ||
                            ctx->status_code == 303 ||
                            ctx->status_code == 307 || ctx->status_code == 308;
  if (is_redirect_status) {
    cmap_pair kp = {.ptr = (void *)"location", .size = sizeof("location")};
    cmap_pair vp;
    if (chashmap_get_elem_ref(ctx->headers, &kp, &vp) == ccol_success &&
        ((const char *)vp.ptr)[0] != '\0') {
      ctx->location = ccol_strdup(ctx->mp, (const char *)vp.ptr);
      if (!ctx->location) {
        ctx->error = true;
        return -1;
      }
      ctx->will_redirect = true;
    }
  }

  if (ctx->field_lines && !field_lines_combine(ctx)) {
    ctx->error = true;
    return -1;
  }

  bool is_informational_status =
      ctx->status_code >= 100 && ctx->status_code < 200;
  bool is_always_bodyless_status = is_informational_status ||
                                   ctx->status_code == 204 ||
                                   ctx->status_code == 304;
  if (!ctx->will_redirect && !ctx->is_head_request &&
      !is_always_bodyless_status && ctx->requested_sink_fn == sink_buffered) {
    chttp_bodybuf *bb = (chttp_bodybuf *)ctx->requested_sink_ctx;
    if (bb->max_size > 0 && chttp1_has_content_length(p) &&
        chttp1_declared_content_length(p) > (uint64_t)bb->max_size) {
      ctx->too_large = true;
      return -1;
    }
  }

  ctx->sink_fn = ctx->will_redirect ? sink_discard : ctx->requested_sink_fn;
  ctx->sink_ctx = ctx->will_redirect ? NULL : ctx->requested_sink_ctx;

  return ctx->is_head_request ? 1 : 0;
}

// Passes body bytes to the sink chosen for this parse (the parser's body
// callback). A short count becomes an allocation failure, a cap breach or an
// abort by the caller's write_fn, recorded in ctx so the caller reports the
// right code.
static int http1_on_body(chttp1_parser *p, const char *at, size_t len) {
  chttp_parse_ctx *ctx = (chttp_parse_ctx *)p->data;
  size_t n = ctx->sink_fn ? ctx->sink_fn(at, len, ctx->sink_ctx) : len;
  if (n != len) {
    if (ctx->sink_fn == sink_buffered) {
      chttp_bodybuf *bb = (chttp_bodybuf *)ctx->sink_ctx;
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

// Marks the response complete (the parser's message-complete callback), so the
// read loop knows the message ended.
static int http1_on_message_complete(chttp1_parser *p) {
  chttp_parse_ctx *ctx = (chttp_parse_ctx *)p->data;
  ctx->message_complete = true;
  return 0;
}

// The parser settings shared by every response parse, built once.
static struct {
  chttp1_settings settings;
  ccol__once_flag once;
} client_http1_settings_bundler = {0};

// Fills the settings above. on_trailer stays NULL, so trailer fields are
// dropped.
static void init_chttp1_settings(void) {
  chttp1_settings_init(&client_http1_settings_bundler.settings);
  client_http1_settings_bundler.settings.on_header = http1_on_header;
  client_http1_settings_bundler.settings.on_headers_complete =
      http1_on_headers_complete;
  client_http1_settings_bundler.settings.on_body = http1_on_body;
  client_http1_settings_bundler.settings.on_message_complete =
      http1_on_message_complete;
}

// Frees the header map, the field lines and the redirect location of ctx, for
// the reset and the error paths of a parse.
static void parse_ctx_free_fields(chttp_parse_ctx *ctx) {
  ccol__mem_free(ctx->mp, ctx->location);
  if (ctx->headers) chashmap__destroy(ctx->headers);
  field_lines_free(ctx->mp, ctx->field_lines);
  ctx->location = NULL;
  ctx->headers = NULL;
  ctx->field_lines = NULL;
}

// Resets ctx for the final response that follows a "100 Continue" on the same
// connection, so the interim response's headers never leak into it. Fields that
// belong to the request stay.
static ccol_retval parse_ctx_reset_for_continue(chttp_parse_ctx *ctx) {
  ccol__mem_free(ctx->mp, ctx->location);
  ctx->location = NULL;
  if (ctx->headers) chashmap__destroy(ctx->headers);
  field_lines_free(ctx->mp, ctx->field_lines);
  ctx->field_lines = NULL;
  char *herr = NULL;
  ctx->headers =
      chashmap_create_full(CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                           ccol_string, ctx->mp, NULL, NULL, &herr);
  ctx->will_redirect = false;
  ctx->message_complete = false;
  ctx->trailing_garbage = false;
  ctx->error = false;
  ctx->aborted = false;
  ctx->too_large = false;
  ctx->status_code = 0;
  ctx->sink_fn = NULL;
  ctx->sink_ctx = NULL;
  return ctx->headers ? ccol_success : ccol_not_enough_memory;
}

#ifdef RUNNING_UNIT_TESTS
// Counters of Tier 1 reads and of the reads that had to wait for the socket,
// for the test that a large body isn't read with a wait per read.
static unsigned long chttp_tier1_reads;
static unsigned long chttp_tier1_read_waits;
// Give the Tier 1 read and read-wait counters, so a test can check that a large
// body isn't read with a wait per read.
unsigned long chttp__tier1_reads_for_tests(void) {
  return __atomic_load_n(&chttp_tier1_reads, __ATOMIC_RELAXED);
}
unsigned long chttp__tier1_read_waits_for_tests(void) {
  return __atomic_load_n(&chttp_tier1_read_waits, __ATOMIC_RELAXED);
}
// Counts the Tier 1 hops that open a 100-continue window.
static unsigned long chttp_tier1_continue_waits;
// Gives chttp_tier1_continue_waits, so a test can check that a request which
// must not wait never opens a 100-continue window.
unsigned long chttp__tier1_continue_waits_for_tests(void) {
  return __atomic_load_n(&chttp_tier1_continue_waits, __ATOMIC_RELAXED);
}
#endif

// Reads and parses one message from conn into ctx. carry_in goes to the parser
// before the socket. first_byte bounds only the wait for the first byte (the
// 100-continue window); once a byte has arrived only overall applies. After a
// read that made progress we read again at once, and wait only after one that
// blocked. Bytes past the message come back in *leftover_out from mp;
// *keep_alive_out is the parser's answer.
static ccol_retval chttp_read_message_with(
    chttp1_parser *parser, chttp_conn *conn, chttp_parse_ctx *pctx,
    chttp_deadline *overall, chttp_deadline *first_byte, const char *carry_in,
    size_t carry_in_len, bool *keep_alive_out, bool *any_bytes_read_out,
    char **leftover_out, size_t *leftover_len_out) {
  *leftover_out = NULL;
  *leftover_len_out = 0;
  bool started = carry_in_len > 0;

  if (carry_in_len > 0) {
    *any_bytes_read_out = true;
    chttp1_errno err = chttp1_parser_execute(parser, carry_in, carry_in_len);
    if (err == CHTTP1_PAUSED) {
      size_t consumed = chttp1_parser_consumed(parser);
      *keep_alive_out = chttp1_should_keep_alive(parser);
      if (consumed < carry_in_len) {
        size_t rem = carry_in_len - consumed;
        char *lb = (char *)ccol__mem_alloc(pctx->mp, rem);
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
  }

  char buf[8192];
  short want_events = POLLIN;
  bool read_blocked = false;
  bool wait_first = true;
  for (;;) {
    int wait_ms;
    chttp_deadline *dl = (started || !first_byte) ? overall : first_byte;
    if (read_deadline_passed(dl, conn->fd) ||
        !deadline_remaining_ms(dl, &wait_ms) ||
        !conn_inactivity_left_ms(conn, &wait_ms))
      return ccol_timed_out;
    bool buffered = conn->tls && want_events == POLLIN && !read_blocked &&
                    ctls_conn_has_pending_input(conn->tls);
    if (wait_first && !buffered) {
#ifdef RUNNING_UNIT_TESTS
      __atomic_fetch_add(&chttp_tier1_read_waits, 1, __ATOMIC_RELAXED);
#endif
      ccol_retval prv =
          conn_wait_io(conn, want_events, wait_ms, ccol_http_transfer_aborted);
      if (prv == ccol_timed_out) continue;
      if (prv != ccol_success) return prv;
    }

#ifdef RUNNING_UNIT_TESTS
    __atomic_fetch_add(&chttp_tier1_reads, 1, __ATOMIC_RELAXED);
#endif
    ssize_t n = conn_read(conn, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EWOULDBLOCK) {
        read_blocked = true;
        wait_first = true;
        if (conn->tls)
          want_events = conn_tls_wants_write(conn->tls) ? POLLOUT : POLLIN;
        continue;
      }
      return ccol_http_transfer_aborted;
    }
    read_blocked = false;
    wait_first = false;
    want_events = POLLIN;
    if (n > 0) {
      *any_bytes_read_out = true;
      started = true;
    }
    if (n == 0) {
      chttp1_errno fe = chttp1_parser_finish(parser);
      if ((fe != CHTTP1_OK && fe != CHTTP1_PAUSED) || !pctx->message_complete)
        return ccol_http_transfer_aborted;
      *keep_alive_out = false;
      return ccol_success;
    }

    chttp1_errno err = chttp1_parser_execute(parser, buf, (size_t)n);
    if (err == CHTTP1_PAUSED) {
      size_t consumed = chttp1_parser_consumed(parser);
      *keep_alive_out = chttp1_should_keep_alive(parser);
      if (consumed < (size_t)n) {
        size_t rem = (size_t)n - consumed;
        char *lb = (char *)ccol__mem_alloc(pctx->mp, rem);
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
  }
}

// Reads one message from conn for Tier 1, through chttp_read_message_with, with
// a parser set up here. It enables the 64 KiB response line spill and releases
// it on every path, so no caller can leak it.
static ccol_retval chttp_read_message(
    chttp_conn *conn, chttp_parse_ctx *pctx, chttp_deadline *overall,
    chttp_deadline *first_byte, const char *carry_in, size_t carry_in_len,
    bool *keep_alive_out, bool *any_bytes_read_out, char **leftover_out,
    size_t *leftover_len_out) {
  ccol__call_once(client_http1_settings_bundler.once, init_chttp1_settings);

  chttp1_parser parser;
  chttp1_parser_init(&parser, &client_http1_settings_bundler.settings);
  parser.data = pctx;
  (void)chttp1_parser_enable_line_spill(&parser, pctx->mp);
  ccol_retval rv = chttp_read_message_with(
      &parser, conn, pctx, overall, first_byte, carry_in, carry_in_len,
      keep_alive_out, any_bytes_read_out, leftover_out, leftover_len_out);
  chttp1_parser_release(&parser);
  return rv;
}

// Reads messages like chttp_read_message but skips every interim 1xx other than
// stop_at_status (0 skips them all), so a 1xx the server sends on its own never
// reads as the response. It stops after CHTTP_MAX_INTERIM_RESPONSES skipped
// messages; leftover bytes of a skipped message feed the next one.
static ccol_retval chttp_read_message_loop(
    chttp_conn *conn, chttp_parse_ctx *pctx, chttp_deadline *overall,
    chttp_deadline *first_byte, const char *carry_in, size_t carry_in_len,
    int stop_at_status, size_t discarded_before, bool *keep_alive_out,
    bool *any_bytes_read_out, char **leftover_out, size_t *leftover_len_out) {
  char *cur_carry = NULL;
  size_t cur_carry_len = 0;
  if (carry_in_len > 0) {
    cur_carry = (char *)ccol__mem_alloc(pctx->mp, carry_in_len);
    if (!cur_carry) return ccol_not_enough_memory;
    memcpy(cur_carry, carry_in, carry_in_len);
    cur_carry_len = carry_in_len;
  }

  for (size_t n_discarded = discarded_before;;) {
    char *leftover = NULL;
    size_t leftover_len = 0;
    ccol_retval rv = chttp_read_message(
        conn, pctx, overall, first_byte, cur_carry, cur_carry_len,
        keep_alive_out, any_bytes_read_out, &leftover, &leftover_len);
    ccol__mem_free(pctx->mp, cur_carry);
    cur_carry = NULL;
    cur_carry_len = 0;
    if (rv != ccol_success) {
      ccol__mem_free(pctx->mp, leftover);
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
      ccol__mem_free(pctx->mp, leftover);
      return ccol_http_transfer_aborted;
    }
    n_discarded++;

    ccol_retval rrv = parse_ctx_reset_for_continue(pctx);
    if (rrv != ccol_success) {
      ccol__mem_free(pctx->mp, leftover);
      return rrv;
    }
    cur_carry = leftover;
    cur_carry_len = leftover_len;
  }
}

// TLS context management
//
// Tells whether path names a regular file we can read, so a rebuild can mark
// the context unusable instead of failing when a configured file isn't there
// yet.
static bool file_readable(const char *path) {
  return path && access(path, R_OK) == 0;
}

// Builds cli->tls_ctx from cli->tls, at create, from chttpcli_set_tls, and at a
// request after a rebuild that ran out of memory. The generation is bumped
// first on every path. A missing or unreadable file only marks the context
// unusable (requests then fail with ccol_http_tls_cert_load_failed), because
// TLS may be configured before its files exist. An allocation failure is
// returned as ccol_not_enough_memory. The caller holds cli->lock.
static ccol_retval rebuild_tls_ctx_body_locked(struct chttpclient *cli) {
  cli->tls_generation++;
  if (cli->tls_ctx) {
    ctls_ctx_release(cli->tls_ctx);
    cli->tls_ctx = NULL;
  }
  cli->tls_ctx_usable = false;

  bool have_cert_pair = cli->tls.cert_path && cli->tls.key_path;
  if (have_cert_pair && (!file_readable(cli->tls.cert_path) ||
                         !file_readable(cli->tls.key_path))) {
    return ccol_success;
  }
  if (cli->tls.ca_bundle_path && !file_readable(cli->tls.ca_bundle_path)) {
    return ccol_success;
  }

  ctls_ctx *ctx = ctls_ctx_new_mp(cli->m_procs, NULL);
  if (!ctx) return ccol_not_enough_memory;

  if (have_cert_pair) {
    ccol_retval crv = ctls_ctx_cert_add(ctx, NULL, cli->tls.cert_path,
                                        cli->tls.key_path, NULL, NULL);
    if (crv != ccol_success) {
      ctls_ctx_release(ctx);
      return crv == ccol_not_enough_memory ? crv : ccol_success;
    }
  }

  if (cli->tls.ca_bundle_path) {
    ccol_retval trv = ctls_ctx_trust(ctx, cli->tls.ca_bundle_path, NULL);
    if (trv != ccol_success) {
      ctls_ctx_release(ctx);
      return trv == ccol_not_enough_memory ? trv : ccol_success;
    }
  } else if (!cli->tls.insecure_skip_verify) {
    ccol_retval srv = ctls_ctx_trust_system(ctx);
    if (srv != ccol_success) {
      ctls_ctx_release(ctx);
      return srv == ccol_not_enough_memory ? srv : ccol_success;
    }
  }

  cli->tls_ctx = ctx;
  cli->tls_ctx_usable = true;
  return ccol_success;
}

// Rebuilds cli->tls_ctx for create, chttpcli_set_tls and the retry after an
// out-of-memory rebuild, and records in cli->tls_ctx_oom whether it ran out of
// memory. Gives the rebuild's code. The caller holds cli->lock.
static ccol_retval rebuild_tls_ctx_locked(struct chttpclient *cli) {
  ccol_retval rv = rebuild_tls_ctx_body_locked(cli);
  cli->tls_ctx_oom = rv == ccol_not_enough_memory;
  return rv;
}

// Tries the rebuild again when the last one ran out of memory, so a client
// recovers once memory comes back. The caller holds cli->lock.
static void tls_ctx_retry_if_oom_locked(struct chttpclient *cli) {
  if (!cli->tls_ctx_usable && cli->tls_ctx_oom)
    (void)rebuild_tls_ctx_locked(cli);
}

// Gives the code a request that needs TLS reports when the client has no usable
// context: ccol_not_enough_memory when oom is set, else
// ccol_http_tls_cert_load_failed. Every tier uses it so they agree.
static ccol_retval tls_ctx_unusable_code(bool oom) {
  return oom ? ccol_not_enough_memory : ccol_http_tls_cert_load_failed;
}

// The client engine
//
// One ceventloop reactor for the whole process, shared by every client, and
// independent of chttpserver's. It starts with the first async request and is
// reaped on its own thread once the last user goes away. Everything here is
// guarded by mutex, except the atomics read without it. The installed logger is
// kept across stop and start; the reaper closes only the fallback logger.
static struct {
  ceventloop reactor;
  size_t reactor_refs;
  ccol__mutex mutex;
  ccol__cond_var stopped_cv;
  ccol__once_flag once;
  bool stopping;
  ccol__thread_id reaper_thread;
  bool reaper_joinable;
  ccol_memmgmt_procs mprocs_storage;
  ccol_memmgmt_procs *mprocs;

  size_t num_reactor_threads;
  size_t last_resolved_num_reactor_threads;

  clog logger;

  clog user_logger;

  cthrpool dns_pool;

  size_t chains_live;
  bool exit_waiting;

  ccol__once_flag exit_hook_once;
#if CCOL_FORK_SAFETY_REQUIRED
  ccol__once_flag atfork_once;
#endif
} cli_engine_bundler = {0};

// The process that built the engine or installed the exit hook; client_at_exit
// acts only there, since a fork child inherits the state but not the threads.
// Atomic because the exit hook reads it before taking any lock.
static _Atomic pid_t g_client_engine_pid = 0;

// Async requests from any client whose future isn't fulfilled yet. A zero
// proves to client_at_exit that exit() isn't called from a write_fn and that no
// request is in flight.
static _Atomic size_t g_async_requests_pending = 0;
// Async requests the sweep fulfilled while their hop is still alive (stuck in a
// name lookup, or waiting for a dispatch that never comes). The exit handler
// counts them as in flight.
static _Atomic size_t g_async_hops_abandoned = 0;

static ccol_retval client_deadline_sweep_start(void);
static void client_deadline_sweep_stop_and_join(void);
static void client_exit_hook_install(void);
static void default_client_destroy_if_idle(void);

#if CCOL_FORK_SAFETY_REQUIRED
static void chttpcli_atfork_prepare(void);
static void chttpcli_atfork_parent(void);
static void chttpcli_atfork_child(void);
#endif

#if CCOL_FORK_SAFETY_REQUIRED
// Registers this module's fork handlers, once. ceventloop, cthrpool and clog
// register first, so our prepare handler runs before theirs.
static void client_atfork_install(void) {
  cthreadcomm__ensure_atfork_registered_before_caller();
  cthrpool__ensure_atfork_registered_before_caller();
  clog__ensure_atfork_registered_before_caller();
  ccol__at_fork(chttpcli_atfork_prepare, chttpcli_atfork_parent,
                chttpcli_atfork_child);
}
#endif

#if defined(CCOL__ATFORK_AT_LOAD) && CCOL_FORK_SAFETY_REQUIRED
// Registers the fork handlers at load on systems that need it; see
// CCOL__ATFORK_AT_LOAD in common.h.
__attribute__((constructor)) static void client_atfork_at_load(void) {
  ccol__call_once(cli_engine_bundler.atfork_once, client_atfork_install);
}
#endif

// Set first thing in client_engine_globals_init, so the destructor knows
// whether the engine globals were ever initialized.
static _Atomic bool g_engine_globals_entered = false;

// Initializes the engine mutex and condition variables, once, before the first
// engine use, and sets g_engine_globals_entered first. SIGPIPE is left to the
// application; every write of this module avoids it.
static void client_engine_globals_init(void) {
  atomic_store(&g_engine_globals_entered, true);
  if (ccol__mutex_init(cli_engine_bundler.mutex) != 0)
    ccol__fatal_err("chttpclient engine: failed to initialize mutex");
  if (ccol__cond_var_init(cli_engine_bundler.stopped_cv) != 0)
    ccol__fatal_err(
        "chttpclient engine: failed to initialize condition variable");
}

// Logs fmt and its arguments (as for clog_info) through the engine logger, for
// diagnostics from engine threads. The pointer is read and used inside one
// critical section of the engine mutex, because chttpcli_set_engine_logger
// closes the old logger right after it swaps it. Expands to a statement.
#define CLIENT_ENGINE_LOG_INFO(fmt, ...)                                  \
  do {                                                                    \
    ccol__call_once(cli_engine_bundler.once, client_engine_globals_init); \
    ccol__mutex_lock(cli_engine_bundler.mutex);                           \
    if (cli_engine_bundler.logger)                                        \
      clog_info(cli_engine_bundler.logger, fmt, ##__VA_ARGS__);           \
    ccol__mutex_unlock(cli_engine_bundler.mutex);                         \
  } while (0)

#ifdef RUNNING_UNIT_TESTS
// Reaper threads created and joined; they match once the engine is quiescent.
static _Atomic unsigned g_reaper_created_for_tests = 0;
static _Atomic unsigned g_reaper_joined_for_tests = 0;
#endif

// Joins a reaper that has finished, so its handle is never overwritten
// unjoined. The caller holds the engine mutex.
static void client_engine_join_reaper_if_needed_locked(void) {
  if (cli_engine_bundler.reaper_joinable) {
    ccol__thread_join(cli_engine_bundler.reaper_thread);
    cli_engine_bundler.reaper_joinable = false;
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add(&g_reaper_joined_for_tests, 1u);
#endif
  }
}

// True while the engine runs with no reference because no thread could be had
// for its teardown. The sweep reads it each tick, without the engine lock, and
// retries the reap.
static _Atomic bool g_engine_reap_abandoned = false;

// In a fork child, forgets the inherited reaper handle, stopping flag and
// unreferenced reactor, all of which name parent threads; joining such a handle
// could join an unrelated thread that reuses its descriptor. It checks the pid,
// so it holds without the fork handlers too. The caller holds the engine mutex.
static void client_engine_forget_inherited_locked(void) {
  if (!cli_engine_bundler.stopping && !cli_engine_bundler.reaper_joinable &&
      !(cli_engine_bundler.reactor && cli_engine_bundler.reactor_refs == 0))
    return;
  if (atomic_load(&g_client_engine_pid) == getpid()) return;
  clog inherited_logger = cli_engine_bundler.logger;
  cli_engine_bundler.logger = CLOG_NULL;
  if (inherited_logger && inherited_logger != cli_engine_bundler.user_logger)
    clog_close(inherited_logger);
  cli_engine_bundler.reactor = CEVENTLOOP_NULL;
  cli_engine_bundler.dns_pool = CTHRPOOL_NULL;
  cli_engine_bundler.reactor_refs = 0;
  cli_engine_bundler.stopping = false;
  cli_engine_bundler.reaper_joinable = false;
  cli_engine_bundler.chains_live = 0;
  cli_engine_bundler.exit_waiting = false;
  atomic_store(&g_async_requests_pending, 0);
  atomic_store(&g_async_hops_abandoned, 0);
  atomic_store(&g_engine_reap_abandoned, false);
}

#ifdef RUNNING_UNIT_TESTS
// While non-zero, the next reaper holds the engine mutex this many ms in its
// final critical section and sets the entered flag, so a test can race it.
static _Atomic int g_reaper_final_hold_ms_for_tests = 0;
static _Atomic bool g_reaper_final_hold_entered_for_tests = false;

// Runs the reaper's final-section hold, once, so a test can race the end of a
// teardown.
static void reaper_final_hold_for_tests(void) {
  int ms = atomic_exchange(&g_reaper_final_hold_ms_for_tests, 0);
  if (ms <= 0) return;
  atomic_store(&g_reaper_final_hold_entered_for_tests, true);
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

// While non-zero, the next reaper waits this many ms after it closes the
// fallback logger, the last thing it does, holding no lock. Fires once.
static _Atomic int g_reaper_tail_hold_ms_for_tests = 0;
static _Atomic bool g_reaper_tail_hold_entered_for_tests = false;
static _Atomic bool g_reaper_tail_hold_left_for_tests = false;

// Runs the reaper's tail hold, once, so a test can fork while the reaper
// finishes after clearing stopping.
static void reaper_tail_hold_for_tests(void) {
  int ms = atomic_exchange(&g_reaper_tail_hold_ms_for_tests, 0);
  if (ms <= 0) return;
  atomic_store(&g_reaper_tail_hold_entered_for_tests, true);
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  atomic_store(&g_reaper_tail_hold_left_for_tests, true);
}
#endif

// Tears the engine down on the reaper thread. It stops the deadline sweep first
// (a tick may use the reactor and the DNS pool), then destroys the reactor and
// the DNS pool outside the lock, clears stopping last so a waiting acquirer can
// go on, and closes the fallback logger. It never runs on the thread that
// dropped the last reference, which is often an engine thread.
static void *client_engine_reaper_fn(void *arg) {
  (void)arg;
  ceventloop loop_to_destroy;
  cthrpool dns_pool_to_destroy;
  ccol__mutex_lock(cli_engine_bundler.mutex);
  loop_to_destroy = cli_engine_bundler.reactor;
  dns_pool_to_destroy = cli_engine_bundler.dns_pool;
  ccol__mutex_unlock(cli_engine_bundler.mutex);

  client_deadline_sweep_stop_and_join();
  if (loop_to_destroy) ceventloop_destroy(loop_to_destroy);
  if (dns_pool_to_destroy) cthrpool_destroy(dns_pool_to_destroy);

  ccol__mutex_lock(cli_engine_bundler.mutex);
#ifdef RUNNING_UNIT_TESTS
  reaper_final_hold_for_tests();
#endif
  cli_engine_bundler.dns_pool = CTHRPOOL_NULL;
  cli_engine_bundler.reactor = CEVENTLOOP_NULL;
  cli_engine_bundler.stopping = false;
  if (cli_engine_bundler.logger)
    clog_info(cli_engine_bundler.logger,
              "The http client reactor engine has been destroyed");
  clog old_logger = cli_engine_bundler.logger != cli_engine_bundler.user_logger
                        ? cli_engine_bundler.logger
                        : CLOG_NULL;
  cli_engine_bundler.logger = CLOG_NULL;
  ccol__cond_var_broadcast(cli_engine_bundler.stopped_cv);
  ccol__mutex_unlock(cli_engine_bundler.mutex);
  if (old_logger) clog_close(old_logger);
#ifdef RUNNING_UNIT_TESTS
  reaper_tail_hold_for_tests();
#endif
  return NULL;
}

// Extra attempts and the pause between them when pthread_create fails (EAGAIN
// under a thread ceiling clears quickly). The total stays small because this
// may run on a dispatch thread.
#define CHTTP_REAPER_SPAWN_RETRIES 5
#define CHTTP_REAPER_SPAWN_RETRY_NS (1000L * 1000L)

#ifdef RUNNING_UNIT_TESTS
// While set, client_engine_spawn_reaper treats every thread creation as failed,
// so a test reaches the give-up path; the counter says how often it gave up.
static _Atomic bool g_force_reaper_spawn_fail_for_tests = false;
static _Atomic unsigned g_reaper_abandoned_count_for_tests = 0;

// -1 until a shutdown of an abandoned ctx has run, then 1 if it ran with the
// ctx's idle_lock held and 0 if not. Only 1 is safe: After the unlock a
// dispatch can close the fd and the number can name another socket.
static _Atomic int g_abandon_shutdown_under_lock_for_tests = -1;

// While set, the next reaper is held between its creation and the publish of
// its handle until a test opens the gate (10 s at most), so a second spawn can
// start in that window. Fires once.
static _Atomic bool g_reaper_spawn_hold_for_tests = false;
static _Atomic bool g_reaper_spawn_held_for_tests = false;
static _Atomic bool g_reaper_spawn_gate_for_tests = false;

// Runs the reaper spawn hold, so a test can start a second spawn while the
// first one's handle isn't published yet.
static void reaper_spawn_hook_for_tests(void) {
  if (!atomic_exchange(&g_reaper_spawn_hold_for_tests, false)) return;
  atomic_store(&g_reaper_spawn_held_for_tests, true);
  for (int i = 0; i < 10000 && !atomic_load(&g_reaper_spawn_gate_for_tests);
       ++i) {
    struct timespec ts = {0, 1000000L};
    nanosleep(&ts, NULL);
  }
}
#endif

// Hands the engine teardown to a new reaper thread, with a few retries, when
// the last reference goes away. The teardown can't run inline: On a dispatch
// thread or a DNS worker it would join itself. When no thread can be had, the
// engine stays up with no reference and g_engine_reap_abandoned set, and the
// sweep retries. The handle (cli_engine_bundler.reaper) is published in the
// critical section that creates the thread. The caller holds the engine mutex.
static void client_engine_spawn_reaper(void) {
  ccol__thread_id reaper;
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
    ccol__mutex_lock(cli_engine_bundler.mutex);
    rc = ccol__thread_create(reaper, client_engine_reaper_fn, NULL);
    if (rc == 0) {
#ifdef RUNNING_UNIT_TESTS
      atomic_fetch_add(&g_reaper_created_for_tests, 1u);
      reaper_spawn_hook_for_tests();
#endif
      client_engine_join_reaper_if_needed_locked();
      cli_engine_bundler.reaper_thread = reaper;
      cli_engine_bundler.reaper_joinable = true;
      atomic_store(&g_engine_reap_abandoned, false);
    }
    ccol__mutex_unlock(cli_engine_bundler.mutex);
    if (rc == 0) break;
  }
  if (rc != 0) {
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add(&g_reaper_abandoned_count_for_tests, 1u);
#endif
    ccol__mutex_lock(cli_engine_bundler.mutex);
    cli_engine_bundler.stopping = false;
    bool first_give_up = !atomic_exchange(&g_engine_reap_abandoned, true);
    ccol__cond_var_broadcast(cli_engine_bundler.stopped_cv);
    ccol__mutex_unlock(cli_engine_bundler.mutex);
    if (first_give_up)
      CLIENT_ENGINE_LOG_INFO(
          "The http client reactor engine could not be reaped (no thread "
          "available); the deadline sweep retries the teardown");
    return;
  }
}

// Retries the teardown of an engine left with no reference. Only the sweep
// calls it, once a tick, holding no lock.
static void client_engine_retry_abandoned_reap(void) {
  if (!atomic_load_explicit(&g_engine_reap_abandoned, memory_order_relaxed))
    return;
  bool reap = false;
  ccol__mutex_lock(cli_engine_bundler.mutex);
  if (atomic_load(&g_engine_reap_abandoned) && cli_engine_bundler.reactor &&
      cli_engine_bundler.reactor_refs == 0 && !cli_engine_bundler.stopping) {
    cli_engine_bundler.stopping = true;
    reap = true;
  }
  ccol__mutex_unlock(cli_engine_bundler.mutex);
  if (reap) client_engine_spawn_reaper();
}

// Takes a reference to the engine. The first one creates the reactor, the DNS
// pool and the deadline sweep, and registers the exit hook after OpenSSL's
// cleanup so the hook runs first. A failed step clears its fields under the
// mutex and destroys outside it. for_chain also counts the reference in
// chains_live and the request in g_async_requests_pending. Every success pairs
// with one release.
static ccol_retval client_engine_acquire_impl(bool for_chain) {
  ccol__call_once(cli_engine_bundler.once, client_engine_globals_init);
  ccol__call_once(cli_engine_bundler.exit_hook_once, client_exit_hook_install);
#if CCOL_FORK_SAFETY_REQUIRED
  ccol__call_once(cli_engine_bundler.atfork_once, client_atfork_install);
#endif
  ccol__mutex_lock(cli_engine_bundler.mutex);
  client_engine_forget_inherited_locked();
  while (cli_engine_bundler.stopping)
    ccol__cond_var_wait(cli_engine_bundler.stopped_cv,
                        cli_engine_bundler.mutex);
  client_engine_join_reaper_if_needed_locked();

  if (!cli_engine_bundler.reactor) {
    atomic_store(&g_client_engine_pid, getpid());
    size_t nthreads = cli_engine_bundler.num_reactor_threads;
    if (nthreads == 0) {
      long cpus = sysconf(_SC_NPROCESSORS_ONLN);
      nthreads = (cpus > 0) ? (size_t)cpus : 1;
    }
    cli_engine_bundler.last_resolved_num_reactor_threads = nthreads;
    char *err = NULL;
    cli_engine_bundler.reactor =
        ceventloop_create_mp(256, 4, nthreads, cli_engine_bundler.mprocs, &err);
    if (!cli_engine_bundler.reactor) {
      ccol__mutex_unlock(cli_engine_bundler.mutex);
      return ccol_not_enough_memory;
    }

    char *dns_err = NULL;
    cli_engine_bundler.dns_pool =
        cthrpool_create_mp(nthreads, 0, cli_engine_bundler.mprocs, &dns_err);
    if (!cli_engine_bundler.dns_pool) {
      ceventloop stale_reactor = cli_engine_bundler.reactor;
      cli_engine_bundler.reactor = CEVENTLOOP_NULL;
      ccol__mutex_unlock(cli_engine_bundler.mutex);
      ceventloop__destroy(stale_reactor);
      return ccol_not_enough_memory;
    }

    if (client_deadline_sweep_start() != ccol_success) {
      cthrpool stale_dns_pool = cli_engine_bundler.dns_pool;
      ceventloop stale_reactor = cli_engine_bundler.reactor;
      cli_engine_bundler.dns_pool = CTHRPOOL_NULL;
      cli_engine_bundler.reactor = CEVENTLOOP_NULL;
      ccol__mutex_unlock(cli_engine_bundler.mutex);
      cthrpool__destroy(stale_dns_pool);
      ceventloop__destroy(stale_reactor);
      return ccol_unexpected_failure;
    }

    if (!cli_engine_bundler.logger && cli_engine_bundler.user_logger)
      cli_engine_bundler.logger = cli_engine_bundler.user_logger;
    if (!cli_engine_bundler.logger) {
      cli_engine_bundler.logger = clog_create_from_fd_mp(
          2, CLOG_FATAL, NULL, cli_engine_bundler.mprocs);
      if (!cli_engine_bundler.logger) {
        cli_engine_bundler.stopping = true;
        ccol__mutex_unlock(cli_engine_bundler.mutex);
        client_engine_spawn_reaper();
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
    if (cli_engine_bundler.exit_waiting)
      ccol__cond_var_broadcast(cli_engine_bundler.stopped_cv);
  }
  ccol__mutex_unlock(cli_engine_bundler.mutex);
  return ccol_success;
}

// Takes an engine reference that isn't tied to a chain, for an idle pooled
// connection and for the tests.
static ccol_retval client_engine_acquire(void) {
  return client_engine_acquire_impl(false);
}

// Takes the engine reference of an async chain, which also counts the request
// in g_async_requests_pending so client_at_exit knows one is in flight. Pairs
// with client_engine_release_chain.
static ccol_retval client_engine_acquire_chain(void) {
  return client_engine_acquire_impl(true);
}

// Takes a request off g_async_requests_pending right before its future is
// fulfilled, so whoever sees the future ready also sees the count without it.
static inline void client_async_request_settled(void) {
  atomic_fetch_sub_explicit(&g_async_requests_pending, 1, memory_order_release);
}

// Drops a reference. The last one hands the teardown to a reaper thread, since
// the caller is often a reactor thread that must not join the sweep or drain
// the DNS pool.
static void client_engine_release_impl(bool for_chain) {
  bool should_reap = false;
  ccol__mutex_lock(cli_engine_bundler.mutex);
  if (for_chain && cli_engine_bundler.chains_live > 0 &&
      --cli_engine_bundler.chains_live == 0 && cli_engine_bundler.exit_waiting)
    ccol__cond_var_broadcast(cli_engine_bundler.stopped_cv);
  if (cli_engine_bundler.reactor_refs > 0) cli_engine_bundler.reactor_refs--;
  if (cli_engine_bundler.reactor_refs == 0 && cli_engine_bundler.reactor) {
    cli_engine_bundler.stopping = true;
    should_reap = true;
  }
  ccol__mutex_unlock(cli_engine_bundler.mutex);
  if (should_reap) client_engine_spawn_reaper();
}

// Drops an engine reference that client_engine_acquire took.
static void client_engine_release(void) { client_engine_release_impl(false); }

// Drops the reference of client_engine_acquire_chain, when its chain is freed.
static void client_engine_release_chain(void) {
  client_engine_release_impl(true);
}

#ifdef RUNNING_UNIT_TESTS
// Waits until a reaper in progress has finished, so a test can wait for a
// teardown it caused. Returns at once when nothing is stopping.
static void client_engine_wait_for_quiescence(void) {
  ccol__call_once(cli_engine_bundler.once, client_engine_globals_init);
  ccol__mutex_lock(cli_engine_bundler.mutex);
  client_engine_forget_inherited_locked();
  while (cli_engine_bundler.stopping)
    ccol__cond_var_wait(cli_engine_bundler.stopped_cv,
                        cli_engine_bundler.mutex);
  client_engine_join_reaper_if_needed_locked();
  ccol__mutex_unlock(cli_engine_bundler.mutex);
}
#endif

// Waits while the engine runs or stops, and joins the reaper. A reaper that
// couldn't start leaves an unreferenced reactor that the sweep tears down, and
// this waits for that too. Fatal on an engine thread, which would wait for
// itself.
void chttpcli_engine_wait(void) {
  if (chttpcli_runs_on_engine_thread())
    ccol__fatal_err(
        "chttpcli_engine_wait: called on a thread of the async engine (a "
        "write_fn of chttpcli_do_async_streaming or "
        "chttpcli_do_pooled_streaming), which would wait for its own "
        "thread; call it from another thread");
  ccol__call_once(cli_engine_bundler.once, client_engine_globals_init);
  ccol__mutex_lock(cli_engine_bundler.mutex);
  client_engine_forget_inherited_locked();
  while (cli_engine_bundler.reactor || cli_engine_bundler.stopping)
    ccol__cond_var_wait(cli_engine_bundler.stopped_cv,
                        cli_engine_bundler.mutex);
  client_engine_join_reaper_if_needed_locked();
  ccol__mutex_unlock(cli_engine_bundler.mutex);
}

// Stops the client engine at exit (the atexit handler). It destroys the default
// client and tears the engine down before OpenSSL's cleanup, so no engine
// thread frees OpenSSL's per-thread state after it. It stands aside in a fork
// child, while any async request is pending and while an abandoned hop exists,
// so an exit() from a write_fn can't deadlock.
static void client_at_exit(void) {
  if (atomic_load(&g_client_engine_pid) != getpid()) return;
  if (atomic_load_explicit(&g_async_requests_pending, memory_order_acquire) ||
      atomic_load_explicit(&g_async_hops_abandoned, memory_order_acquire))
    return;
  ccol__call_once(cli_engine_bundler.once, client_engine_globals_init);

  ccol__mutex_lock(cli_engine_bundler.mutex);
  cli_engine_bundler.exit_waiting = true;
  while (cli_engine_bundler.chains_live > 0 &&
         atomic_load(&g_async_requests_pending) == 0 &&
         atomic_load(&g_async_hops_abandoned) == 0)
    ccol__cond_var_wait(cli_engine_bundler.stopped_cv,
                        cli_engine_bundler.mutex);
  cli_engine_bundler.exit_waiting = false;
  bool in_flight = cli_engine_bundler.chains_live > 0;
  ccol__mutex_unlock(cli_engine_bundler.mutex);
  if (in_flight) return;

  default_client_destroy_if_idle();

  ccol__mutex_lock(cli_engine_bundler.mutex);
  while (cli_engine_bundler.stopping)
    ccol__cond_var_wait(cli_engine_bundler.stopped_cv,
                        cli_engine_bundler.mutex);
  client_engine_join_reaper_if_needed_locked();
  bool reap_here =
      cli_engine_bundler.reactor && cli_engine_bundler.reactor_refs == 0;
  if (reap_here) {
    cli_engine_bundler.stopping = true;
    atomic_store(&g_engine_reap_abandoned, false);
  }
  ccol__mutex_unlock(cli_engine_bundler.mutex);
  if (reap_here) (void)client_engine_reaper_fn(NULL);
}

// Calls OPENSSL_init_ssl first (which registers OpenSSL's cleanup) and then
// registers client_at_exit, once, so ours runs first.
static void client_exit_hook_install(void) {
  atomic_store(&g_client_engine_pid, getpid());
  (void)OPENSSL_init_ssl(0, NULL);
  (void)atexit(client_at_exit);
}

// Installs a logger for engine diagnostics (a handle derived from cl), or
// releases it for CLOG_NULL, so the application decides where engine messages
// go. The logger is kept across engine stops and starts. A release waits for a
// full stop and is ccol_not_permitted while the engine runs or stops. A closed
// or stale handle is ccol_invalid_args and the installed logger stays. Takes
// the engine mutex.
ccol_retval chttpcli_set_engine_logger(clog cl) {
  if (!cl) {
    ccol__call_once(cli_engine_bundler.once, client_engine_globals_init);
    ccol__mutex_lock(cli_engine_bundler.mutex);
    client_engine_forget_inherited_locked();
    if (cli_engine_bundler.reactor || cli_engine_bundler.stopping) {
      ccol__mutex_unlock(cli_engine_bundler.mutex);
      return ccol_not_permitted;
    }
    clog old_user = cli_engine_bundler.user_logger;
    cli_engine_bundler.user_logger = CLOG_NULL;
    ccol__mutex_unlock(cli_engine_bundler.mutex);
    if (old_user) clog_close(old_user);
    return ccol_success;
  }
  ccol_retval derive_rv = ccol_success;
  clog derived = clog__derive_checked(cl, &derive_rv);
  if (!derived) return derive_rv;
  clog_set_field(derived, "component", "http-client-engine");
  ccol__call_once(cli_engine_bundler.once, client_engine_globals_init);
  ccol__mutex_lock(cli_engine_bundler.mutex);
  clog old_user = cli_engine_bundler.user_logger;
  clog old_active = cli_engine_bundler.logger;
  cli_engine_bundler.user_logger = derived;
  if (old_active) cli_engine_bundler.logger = derived;
  ccol__mutex_unlock(cli_engine_bundler.mutex);
  if (old_active && old_active != old_user) clog_close(old_active);
  if (old_user) clog_close(old_user);
  return ccol_success;
}

// At exit or unload, closes the installed engine logger, but only when the
// engine is fully stopped. It returns at once when the engine globals were
// never initialized, since initializing them here would leave resources no
// destructor frees.
__attribute__((destructor)) static void cleanup_engine_user_logger(void) {
  if (!atomic_load(&g_engine_globals_entered)) return;
  ccol__call_once(cli_engine_bundler.once, client_engine_globals_init);
  ccol__mutex_lock(cli_engine_bundler.mutex);
  clog user = CLOG_NULL;
  if (!cli_engine_bundler.reactor && !cli_engine_bundler.stopping &&
      cli_engine_bundler.logger != cli_engine_bundler.user_logger) {
    user = cli_engine_bundler.user_logger;
    cli_engine_bundler.user_logger = CLOG_NULL;
  }
  ccol__mutex_unlock(cli_engine_bundler.mutex);
  if (user) clog_close(user);
}

// Sets the allocator of the engine's own objects, so an application can route
// them through its allocator. ccol_not_permitted while the engine runs.
ccol_retval chttpcli_set_engine_mem_mgmt_procs(ccol_memmgmt_procs *mp) {
  if (mp && (!mp->malloc || !mp->free || !mp->calloc || !mp->realloc))
    return ccol_invalid_args;
  ccol__call_once(cli_engine_bundler.once, client_engine_globals_init);
  ccol__mutex_lock(cli_engine_bundler.mutex);
  client_engine_forget_inherited_locked();
  if (cli_engine_bundler.reactor) {
    ccol__mutex_unlock(cli_engine_bundler.mutex);
    return ccol_not_permitted;
  }
  if (mp) {
    cli_engine_bundler.mprocs_storage = *mp;
    cli_engine_bundler.mprocs = &cli_engine_bundler.mprocs_storage;
  } else {
    cli_engine_bundler.mprocs = NULL;
  }
  ccol__mutex_unlock(cli_engine_bundler.mutex);
  return ccol_success;
}

// Sets the reactor thread count of the next engine (0 picks the CPU count), so
// an application can size the engine. ccol_not_permitted while the engine runs.
ccol_retval chttpcli_set_engine_num_reactor_threads(size_t num_threads) {
  ccol__call_once(cli_engine_bundler.once, client_engine_globals_init);
  ccol__mutex_lock(cli_engine_bundler.mutex);
  client_engine_forget_inherited_locked();
  if (cli_engine_bundler.reactor) {
    ccol__mutex_unlock(cli_engine_bundler.mutex);
    return ccol_not_permitted;
  }
  cli_engine_bundler.num_reactor_threads = num_threads;
  ccol__mutex_unlock(cli_engine_bundler.mutex);
  return ccol_success;
}

// Async connection state machine
//
// Tier 2 and Tier 3 run each hop as a non-blocking state machine on the client
// engine, with the same ctls API and parser as Tier 1. One reactor registration
// per connection carries every step, and redirects sit on top of it as a chain
// of hops.
typedef enum {
  CHTTP_ASYNC_CONNECTING,
  CHTTP_ASYNC_TLS_HANDSHAKING,
  CHTTP_ASYNC_WRITING,
  CHTTP_ASYNC_AWAITING_CONTINUE,
  CHTTP_ASYNC_READING,
  CHTTP_ASYNC_IDLE,
} chttp_async_state;

// The shared state of one async request, one per redirect chain (not per hop).
// Every hop holds a reference; lock guards fulfilled, result and refcount,
// because an old hop's teardown can run while the next hop's dispatch runs. The
// fields written per hop (carried_auth, redirect_drops, the body fields) need
// no lock, since only the thread that parsed a hop's response submits the next
// hop.
typedef struct {
  ccol_memmgmt_procs *mp;
  struct chttpclient *cli;
  cthrpool_future *future;

  // The caller's streaming sink (NULL for a buffered request). sink_closed and
  // in_sink make sure no write_fn call starts after the future is fulfilled.
  chttpcli_write_fn write_fn;
  void *write_ctx;
  _Atomic bool sink_closed;
  _Atomic bool in_sink;

  // The request as submitted: Headers, body and content type. body_dropped is
  // set once a redirect turns the request into a bodiless GET.
  chmap req_headers;
  void *body_data;
  size_t body_len;
  char *body_content_type;
  bool body_dropped;

  // The URL's Authorization value, carried across hops on the same origin and
  // dropped for good once the origin changes, and the origin it was made for.
  char *carried_auth;
  char *carried_auth_origin;

  // The origin of hop 0, and the CHTTP_REDIRECT_DROP_* flags of the current hop
  // judged against it.
  char *initial_origin_key;
  unsigned redirect_drops;

  // A snapshot of the client's TLS context and settings, taken once when the
  // request starts.
  ctls_ctx *tls_ctx;
  bool tls_ctx_usable;
  bool tls_ctx_oom;
  bool verify_host;
  uint64_t tls_generation;

  // Timeouts and options of the request, copied at submit.
  uint64_t connect_timeout_us;
  chttp_deadline overall_deadline;
  size_t max_response_body_size;
  bool prevent_tls_downgrade_on_redirect;
  bool expect_continue;

  // Guards fulfilled, result and refcount. result is allocated ahead, so the
  // fulfilment allocates nothing.
  ccol__mutex lock;
  bool fulfilled;
  chttpcli_async_result *result;
  int refcount;
  // The inactivity timeout and when write_fn last returned, which the sweep
  // compares with the connection's last I/O.
  uint64_t inactivity_timeout_us;
  _Atomic uint64_t sink_returned_ns;
} chttp_async_chain;

static void async_fulfill_chain(chttp_async_chain *chain, ccol_retval rv,
                                chttpcli_response *resp);

// Passes body bytes of a streaming Tier 2 or Tier 3 request to the caller's
// write_fn (the chain's sink), with a frame of the client pushed so a destroy
// from inside write_fn is fatal instead of a wait for itself. It never calls
// write_fn after the sweep closed the sink (sink_closed, in_sink); a call it
// refuses fulfils ccol_timed_out itself. Gives the count write_fn gave, or a
// short count to stop.
static size_t sink_async_user(const void *data, size_t len, void *ctx) {
  chttp_async_chain *chain = (chttp_async_chain *)ctx;
  atomic_store(&chain->in_sink, true);
  if (atomic_load(&chain->sink_closed)) {
    atomic_store(&chain->in_sink, false);
    async_fulfill_chain(chain, ccol_timed_out, NULL);
    return 0;
  }
  chttpcli_call_frame frame;
  chttpcli_call_push(&frame, chain->cli, true);
  size_t n = chain->write_fn(data, len, chain->write_ctx);
  chttpcli_call_pop(&frame);
  atomic_store(&chain->sink_returned_ns, monotonic_now_ns());
  atomic_store(&chain->in_sink, false);
  return n;
}

// Closes a streaming chain's sink and waits until no write_fn call runs, so the
// future is fulfilled with no call after it.
static void async_sink_close(chttp_async_chain *chain) {
  atomic_store(&chain->sink_closed, true);
  while (atomic_load(&chain->in_sink)) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
    nanosleep(&ts, NULL);
  }
}

// See the async Happy Eyeballs section.
typedef struct chttp_connect_race_s chttp_connect_race;

// The values of ctx->flip_watch. ON_INPUT: The send waits for room, and a
// response arriving turns the registration to reading. ON_ROOM: A paused duplex
// send waits for input, and room turns it to writing. See async_upload_pause.
#define CHTTP_FLIP_NONE 0
#define CHTTP_FLIP_ON_INPUT 1
#define CHTTP_FLIP_ON_ROOM 2

// One async connection and the hop it serves. It's tagged so the deadline list
// links can point at it. A connection outlives its chain when it goes back to
// the idle pool (state CHTTP_ASYNC_IDLE, chain NULL).
typedef struct chttp_async_ctx_s {
  ccol_memmgmt_procs *mp;
  struct chttpclient *cli;
  chttp_async_chain *_Atomic chain;
  // The hop index and the method this hop's bytes were built with.
  int hop;
  chttp_method cur_method;
  // The state, the fd and the reactor registration. reg is atomic because
  // ceventloop_add publishes it before the call returns. room_reg exists only
  // while a paused duplex send waits for room; it shares reg's dispatch lock.
  _Atomic chttp_async_state state;
  _Atomic int fd;
  _Atomic(ceventloop_reg) reg;
  _Atomic(ceventloop_reg) room_reg;

  // The target of this connection, the candidate addresses and the Happy
  // Eyeballs race in progress.
  bool is_unix;
  char *unix_socket_path;
  char *host;
  uint16_t port;
  chttp_addr_list *addrs;
  _Atomic(chttp_connect_race *) race;
  bool is_ipv6;
  char *path_and_query;
  char *origin_key;
  // The retry-once and pooling bookkeeping: Whether the connection came from
  // the pool, whether any byte was read, and the interim responses skipped.
  bool reused;
  bool any_bytes_read;
  size_t interim_responses_seen;
  struct timespec last_used;
  // Duplex upload state: A paused send, the direction the sweep watches
  // (CHTTP_FLIP_*), a response that began while sending, and when to look for
  // input next.
  bool upload_paused;
  _Atomic unsigned char flip_watch;
  bool answered_early;
  bool duplex;
  size_t upload_check_at;
  // Set when the IDLE dispatch's own read took input, so a later take sends the
  // hop to the retry path; reuse_checked and tls_read_blocked go with the probe
  // of a reused connection.
  bool idle_tainted;
  bool reuse_checked;
  bool tls_read_blocked;
  bool resp_msg_started;
  // The serialized request and how much of it went out.
  char *wire;
  size_t wire_len;
  size_t wire_sent;

  bool is_https;
  bool verify_host;
  uint64_t tls_generation;
#ifdef RUNNING_UNIT_TESTS
  bool idle_lock_held_for_tests;
#endif
  // hop_completed settles which dispatch finishes the hop, since a spurious
  // extra dispatch would run the redirect hand-off twice. pending_app_teardown
  // marks a teardown that an application-thread path left to the dispatch.
  _Atomic bool hop_completed;
  _Atomic bool pending_app_teardown;
  ctls_conn *tls;

  // Guards state and chain together while the connection moves between idle and
  // active.
  ccol__mutex idle_lock;

  // The deadlines. deadline_lock is a leaf lock that makes the sweep's snapshot
  // and verdict one step against the idle pool's offer and take.
  chttp_deadline connect_deadline;
  ccol__mutex deadline_lock;
  chttp_deadline overall_deadline;
  // abandoned: The sweep fulfilled this live hop's future. timed_out: The sweep
  // marked it and shut its fd down. Both are written without a lock.
  _Atomic bool abandoned;
  _Atomic bool timed_out;
  bool deadline_registered;
  struct chttp_async_ctx_s *deadline_prev, *deadline_next;

  // The response parse of this hop.
  chttp1_parser parser;
  chttp_parse_ctx pctx;
  chttp_bodybuf bb;

  // "Expect: 100-continue" state. continue_decided lets exactly one of the "100
  // seen" and the timeout branches send the body; continue_msg_started ends the
  // window once a message's first byte arrived.
  bool want_100_continue;
  size_t header_len;
  _Atomic bool continue_decided;
  _Atomic bool continue_msg_started;
  chttp_deadline continue_deadline;
  // retry_unsafe: The body went out after a "100 Continue", so it is never sent
  // again. replayable and request_close come from the request.
  bool retry_unsafe;
  bool replayable;
  bool request_close;
  // Bytes that arrived with the "100 Continue" line, fed to the next parse.
  char *continue_carry;
  size_t continue_carry_len;
  // The inactivity timeout and the last instant a byte moved (0 at hop start).
  uint64_t inactivity_us;
  _Atomic uint64_t last_io_ns;
} chttp_async_ctx;

// Async deadline sweep
//
// ceventloop has no timers, so one sweep thread enforces the connect, request,
// 100-continue and inactivity deadlines of Tier 2 and Tier 3 and starts the
// next Happy Eyeballs attempt. It shuts an expired connection's fd down under
// its stripe lock, and the dispatch that follows ends the hop. The registry is
// striped by ctx address, so registering costs one stripe lock.
#define CHTTP_DEADLINE_SWEEP_STRIPES 16

// One stripe of the registry: A lock and a doubly linked list of ctxs.
typedef struct {
  ccol__mutex mutex;
  chttp_async_ctx *head;
} chttp_deadline_stripe;

// The sweep thread and its registry. mutex and cv guard only the thread's
// start, stop and sleep; the stripes guard the lists.
static struct {
  ccol__mutex mutex;
  ccol__cond_var cond_var;
  ccol__once_flag once;
  chttp_deadline_stripe stripes[CHTTP_DEADLINE_SWEEP_STRIPES];
  ccol__thread_id thread;
  bool stop;
} client_deadline_bundle = {0};

// The earliest future instant (CLOCK_MONOTONIC ns) at which the sweep has work.
// Each tick sets it to UINT64_MAX before its walk and lowers it to what the
// walk finds, and a thread setting a deadline lowers it too, so no deadline is
// lost.
static _Atomic uint64_t g_sweep_next_ns = UINT64_MAX;

// When the sleeping sweep plans to wake, or 0 while it is awake. A deadline
// earlier than this wakes it; all four accesses are sequentially consistent, so
// either the sweep sees the new deadline or the setter sees the planned wake.
static _Atomic uint64_t g_sweep_planned_ns = 0;

// True on the sweep thread, which plans its own wait and holds a stripe lock
// under which the bundle mutex must not be taken.
static __thread bool t_on_sweep_thread = false;

#ifdef RUNNING_UNIT_TESTS
// The number of sweep ticks the process has run.
static atomic_ulong g_sweep_ticks_for_tests;

// Gives g_sweep_ticks_for_tests, so a test can count sweep walks.
unsigned long chttp__sweep_ticks_for_tests(void) {
  return atomic_load(&g_sweep_ticks_for_tests);
}

// The instant the sweep planned the current walk for; only the sweep thread
// uses it.
static uint64_t g_sweep_walk_plan_ns_for_tests;

// For each kind of timed work (0: An expired deadline, 1: A Happy Eyeballs
// start), how often a walk acted on it and the largest gap between the walk's
// planned instant and the due instant, so a test can check the plan on a loaded
// machine.
static atomic_ulong g_sweep_due_events_for_tests[2];
static _Atomic int64_t g_sweep_due_max_late_ns_for_tests[2];

// Records one piece of due work for the counters above.
static void sweep_note_due_for_tests(int kind, uint64_t due_ns) {
  int64_t late = (int64_t)g_sweep_walk_plan_ns_for_tests - (int64_t)due_ns;
  atomic_fetch_add(&g_sweep_due_events_for_tests[kind], 1);
  int64_t cur = atomic_load(&g_sweep_due_max_late_ns_for_tests[kind]);
  while (late > cur &&
         !atomic_compare_exchange_weak(&g_sweep_due_max_late_ns_for_tests[kind],
                                       &cur, late)) {
  }
}

// Resets the due-work counts and maxima, so a test measures only its own
// requests.
void chttp__sweep_due_reset_for_tests(void) {
  for (int k = 0; k < 2; k++) {
    atomic_store(&g_sweep_due_events_for_tests[k], 0);
    atomic_store(&g_sweep_due_max_late_ns_for_tests[k], INT64_MIN);
  }
}

// The count for kind since the reset, and in *max_late_ns its largest lateness
// (INT64_MIN with a count of 0).
unsigned long chttp__sweep_due_for_tests(int kind, int64_t *max_late_ns) {
  *max_late_ns = atomic_load(&g_sweep_due_max_late_ns_for_tests[kind]);
  return atomic_load(&g_sweep_due_events_for_tests[kind]);
}
#endif

// Gives the registry stripe of ctx, so register and unregister lock the same
// stripe without storing it: The pointer is folded to 32 bits and hashed with a
// multiply read from the high bits.
static size_t deadline_stripe_index_for_ctx(const chttp_async_ctx *ctx) {
  uintptr_t p = (uintptr_t)ctx;
  uint32_t folded;
#if UINTPTR_MAX > 0xFFFFFFFFu
  folded = (uint32_t)p ^ (uint32_t)(p >> 32);
#else
  folded = (uint32_t)p;
#endif
  uint32_t h = folded * 2654435761u;
  return (size_t)h % CHTTP_DEADLINE_SWEEP_STRIPES;
}

// Initializes the sweep lock, its CLOCK_MONOTONIC condition variable (the clock
// of every deadline here) and the stripe locks, once.
static void client_deadline_init_globals(void) {
  if (ccol__mutex_init(client_deadline_bundle.mutex) != 0)
    ccol__fatal_err("chttpclient deadline sweep: failed to initialize mutex");
  for (size_t i = 0; i < CHTTP_DEADLINE_SWEEP_STRIPES; i++) {
    if (ccol__mutex_init(client_deadline_bundle.stripes[i].mutex) != 0)
      ccol__fatal_err(
          "chttpclient deadline sweep: failed to initialize stripe mutex");
  }
  if (ccol_cond_var_init_monotonic(&client_deadline_bundle.cond_var) != 0)
    ccol__fatal_err(
        "chttpclient deadline sweep: failed to initialize condition "
        "variable");
}

// The longest the sweep sleeps between walks. It also paces the work that has
// no deadline of its own (flipping a stalled send, retrying a reap).
#define CHTTP_DEADLINE_SWEEP_INTERVAL_MS 100
// The most expired connections one tick shuts down; the rest wait for the next
// tick, which follows at once.
#define CHTTP_DEADLINE_SWEEP_BATCH 256

// Adds ctx to the registry unless it's there. It takes only ctx's stripe lock,
// so a caller may hold an idle_lock.
static void client_deadline_register(chttp_async_ctx *ctx) {
  ccol__call_once(client_deadline_bundle.once, client_deadline_init_globals);
  chttp_deadline_stripe *stripe =
      &client_deadline_bundle.stripes[deadline_stripe_index_for_ctx(ctx)];
  ccol__mutex_lock(stripe->mutex);
  if (!ctx->deadline_registered) {
    ctx->deadline_prev = NULL;
    ctx->deadline_next = stripe->head;
    if (stripe->head) stripe->head->deadline_prev = ctx;
    stripe->head = ctx;
    ctx->deadline_registered = true;
  }
  ccol__mutex_unlock(stripe->mutex);
}

// Converts t to nanoseconds, for the sweep's arithmetic.
static uint64_t timespec_to_ns(const struct timespec *t) {
  if (t->tv_sec < 0) return 0;
  if ((uint64_t)t->tv_sec >= UINT64_MAX / 1000000000u) return UINT64_MAX - 1;
  return (uint64_t)t->tv_sec * 1000000000u + (uint64_t)t->tv_nsec;
}

// Gives CLOCK_MONOTONIC now in nanoseconds, the clock of every deadline.
static uint64_t monotonic_now_ns(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return timespec_to_ns(&now);
}

// Tells the sweep that it has work at at_ns: Lowers g_sweep_next_ns, and wakes
// the sleeping sweep to replan when that's earlier than g_sweep_planned_ns
// (never from the sweep thread itself). Every path that sets a deadline calls
// it; the common case costs one atomic load.
static void client_deadline_note_ns(uint64_t at_ns) {
  uint64_t cur = atomic_load(&g_sweep_next_ns);
  while (at_ns < cur) {
    if (atomic_compare_exchange_weak(&g_sweep_next_ns, &cur, at_ns)) {
      if (t_on_sweep_thread) return;
      if (at_ns >= atomic_load(&g_sweep_planned_ns)) return;
      ccol__call_once(client_deadline_bundle.once,
                      client_deadline_init_globals);
      ccol__mutex_lock(client_deadline_bundle.mutex);
      ccol__cond_var_signal(client_deadline_bundle.cond_var);
      ccol__mutex_unlock(client_deadline_bundle.mutex);
      return;
    }
  }
}

// Tells the sweep about deadline d when it's active (through
// client_deadline_note_ns), for the paths that set a chttp_deadline.
static void client_deadline_note(const chttp_deadline *d) {
  if (d->active) client_deadline_note_ns(timespec_to_ns(&d->deadline));
}

// Removes ctx from the registry. It runs once, first thing in async_ctx_free;
// the stripe is a function of the address, so nothing remembers it.
static void client_deadline_unregister(chttp_async_ctx *ctx) {
  ccol__call_once(client_deadline_bundle.once, client_deadline_init_globals);
  chttp_deadline_stripe *stripe =
      &client_deadline_bundle.stripes[deadline_stripe_index_for_ctx(ctx)];
  ccol__mutex_lock(stripe->mutex);
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
  ccol__mutex_unlock(stripe->mutex);
}

#ifdef RUNNING_UNIT_TESTS
// Test hooks for the race between the sweep's verdict and the idle-pool offer:
// a forced verdict for one ctx, a park at a given phase until released, and
// counters around the offer's critical section.
static _Atomic bool g_sweep_race_armed_for_tests = false;
static void *_Atomic g_sweep_race_forced_ctx_for_tests = NULL;
static _Atomic int g_sweep_race_parked_for_tests = 0;
static _Atomic int g_sweep_race_release_for_tests = 0;
static _Atomic unsigned g_offer_entered_for_tests = 0;
static _Atomic unsigned g_offer_done_for_tests = 0;

// Gives the forced verdict for node once, when a test armed one, so the race
// test decides which ctx expires.
static bool sweep_race_take_forced_verdict_for_tests(void *node) {
  if (!atomic_exchange(&g_sweep_race_armed_for_tests, false)) return false;
  atomic_store(&g_sweep_race_forced_ctx_for_tests, node);
  return true;
}

// Parks the sweep at phase until a test releases it (bounded), so the test can
// run the offer path inside the sweep's window.
static void sweep_race_park_for_tests(int phase) {
  atomic_store(&g_sweep_race_parked_for_tests, phase);
  struct timespec ts = {0, 1000000};
  for (int i = 0;
       i < 10000 && atomic_load(&g_sweep_race_release_for_tests) < phase; i++)
    nanosleep(&ts, NULL);
  atomic_store(&g_sweep_race_parked_for_tests, 0);
}

// Arm, query, release and disarm the sweep race hooks, and read the offer
// counters, for the tests of the sweep against the idle-pool offer.
void chttp__sweep_race_arm_for_tests(void) {
  atomic_store(&g_sweep_race_forced_ctx_for_tests, NULL);
  atomic_store(&g_sweep_race_release_for_tests, 0);
  atomic_store(&g_sweep_race_parked_for_tests, 0);
  atomic_store(&g_sweep_race_armed_for_tests, true);
}

int chttp__sweep_race_parked_phase_for_tests(void) {
  return atomic_load(&g_sweep_race_parked_for_tests);
}

void chttp__sweep_race_release_for_tests(int phase) {
  atomic_store(&g_sweep_race_release_for_tests, phase);
}

void chttp__sweep_race_disarm_for_tests(void) {
  atomic_store(&g_sweep_race_armed_for_tests, false);
  atomic_store(&g_sweep_race_release_for_tests, 1000);
  atomic_store(&g_sweep_race_forced_ctx_for_tests, NULL);
}

unsigned chttp__offer_entered_count_for_tests(void) {
  return atomic_load(&g_offer_entered_for_tests);
}

unsigned chttp__offer_done_count_for_tests(void) {
  return atomic_load(&g_offer_done_for_tests);
}

// While armed, the next sweep tick that snapshots a ctx awaiting a "100
// Continue" parks right after its deadline_lock section until released (10 s at
// most). A release bumps a generation the arm only records, so a release can't
// be taken back by a later arm. The last counter counts stores of
// continue_deadline.
static _Atomic bool g_continue_sweep_armed_for_tests = false;
static _Atomic bool g_continue_sweep_parked_for_tests = false;
static _Atomic unsigned g_continue_sweep_release_gen_for_tests = 0;
static _Atomic unsigned g_continue_sweep_armed_gen_for_tests = 0;
static _Atomic unsigned g_continue_deadline_stores_for_tests = 0;

// Parks the sweep after its deadline_lock section on an awaiting-continue ctx,
// until released (10 s at most), so a test can race the continue decision.
static void continue_sweep_park_for_tests(void) {
  if (!atomic_exchange(&g_continue_sweep_armed_for_tests, false)) return;
  unsigned armed_gen = atomic_load(&g_continue_sweep_armed_gen_for_tests);
  atomic_store(&g_continue_sweep_parked_for_tests, true);
  struct timespec ts = {0, 1000000};
  for (int i = 0;
       i < 10000 &&
       atomic_load(&g_continue_sweep_release_gen_for_tests) == armed_gen;
       i++)
    nanosleep(&ts, NULL);
  atomic_store(&g_continue_sweep_parked_for_tests, false);
}

// Counts one store of continue_deadline, for the continue race test.
static void continue_deadline_stored_for_tests(void) {
  atomic_fetch_add_explicit(&g_continue_deadline_stores_for_tests, 1,
                            memory_order_relaxed);
}

// Arm, query and release the continue hook, and read the store count, for the
// continue race test.
void chttp__continue_sweep_arm_for_tests(void) {
  atomic_store(&g_continue_sweep_armed_gen_for_tests,
               atomic_load(&g_continue_sweep_release_gen_for_tests));
  atomic_store(&g_continue_sweep_armed_for_tests, true);
}

bool chttp__continue_sweep_parked_for_tests(void) {
  return atomic_load(&g_continue_sweep_parked_for_tests);
}

void chttp__continue_sweep_release_for_tests(void) {
  atomic_store(&g_continue_sweep_armed_for_tests, false);
  atomic_fetch_add(&g_continue_sweep_release_gen_for_tests, 1);
}

unsigned chttp__continue_deadline_stores_for_tests(void) {
  return atomic_load_explicit(&g_continue_deadline_stores_for_tests,
                              memory_order_relaxed);
}

// While armed, the first sweep that meets a connecting ctx with no fd and no
// race waits (bounded by the clock) until the connect task registers its fd and
// then until the connect deadline passes, which puts the deadline between the
// task's check and the sweep's verdict.
static _Atomic bool g_sweep_fd_wait_armed_for_tests = false;
static _Atomic bool g_sweep_fd_wait_held_for_tests = false;

// Holds the sweep on a connecting ctx with no fd until the connect registers
// its fd and the deadline passes, and records how it ended, so a test hits the
// connect that starts as the sweep judges it.
static void sweep_fd_wait_for_tests(chttp_async_ctx *node) {
  atomic_fetch_add(&g_sweep_fd_wait_seen_for_tests, 1);
  if (!atomic_load(&g_sweep_fd_wait_armed_for_tests)) {
    atomic_fetch_add(&g_sweep_fd_wait_not_armed_for_tests, 1);
    return;
  }
  int fd = node->fd;
  chttp_async_state state = node->state;
  bool raced = atomic_load(&node->race) != NULL;
  if (fd >= 0 || state != CHTTP_ASYNC_CONNECTING || raced) {
    if (fd >= 0) atomic_fetch_add(&g_sweep_fd_wait_had_fd_for_tests, 1);
    if (state != CHTTP_ASYNC_CONNECTING)
      atomic_fetch_add(&g_sweep_fd_wait_wrong_state_for_tests, 1);
    if (raced) atomic_fetch_add(&g_sweep_fd_wait_had_race_for_tests, 1);
    atomic_store(&g_sweep_fd_wait_last_fd_for_tests, fd);
    atomic_store(&g_sweep_fd_wait_last_state_for_tests, (int)state);
    return;
  }
  if (!atomic_exchange(&g_sweep_fd_wait_armed_for_tests, false)) return;
  atomic_store(&g_sweep_fd_wait_pending_for_tests, false);
  struct timespec ts = {0, 1000000};
  uint64_t give_up = monotonic_now_ns() + 5000000000ull;
  while (!atomic_load(&node->reg) && monotonic_now_ns() < give_up)
    nanosleep(&ts, NULL);
  if (!atomic_load(&node->reg)) {
    atomic_store(&g_sweep_fd_wait_hold_end_for_tests, 1);
    return;
  }
  give_up = monotonic_now_ns() + 10000000000ull;
  while (monotonic_now_ns() < give_up) {
    int ms;
    ccol__mutex_lock(node->deadline_lock);
    bool passed = !deadline_remaining_ms(&node->connect_deadline, &ms);
    ccol__mutex_unlock(node->deadline_lock);
    if (passed) break;
    nanosleep(&ts, NULL);
  }
  atomic_store(&g_sweep_fd_wait_hold_end_for_tests, 2);
  atomic_store(&g_sweep_fd_wait_held_for_tests, true);
}

// Arms the fd wait hook and resets its trace, for the test of a connect that
// starts as the sweep judges it.
void chttp__sweep_fd_wait_arm_for_tests(void) {
  atomic_store(&g_sweep_fd_wait_seen_for_tests, 0);
  atomic_store(&g_sweep_fd_wait_not_armed_for_tests, 0);
  atomic_store(&g_sweep_fd_wait_had_fd_for_tests, 0);
  atomic_store(&g_sweep_fd_wait_wrong_state_for_tests, 0);
  atomic_store(&g_sweep_fd_wait_had_race_for_tests, 0);
  atomic_store(&g_sweep_fd_wait_last_fd_for_tests, -2);
  atomic_store(&g_sweep_fd_wait_last_state_for_tests, -1);
  atomic_store(&g_sweep_fd_wait_lookup_end_for_tests, 0);
  atomic_store(&g_sweep_fd_wait_hold_end_for_tests, 0);
  atomic_store(&g_sweep_fd_wait_held_for_tests, false);
  atomic_store(&g_sweep_fd_wait_pending_for_tests, true);
  atomic_store(&g_sweep_fd_wait_armed_for_tests, true);
}

// Tells whether the sweep held as described, and disarms the hook, so the test
// knows its scenario really happened.
bool chttp__sweep_fd_wait_held_for_tests(void) {
  atomic_store(&g_sweep_fd_wait_armed_for_tests, false);
  atomic_store(&g_sweep_fd_wait_pending_for_tests, false);
  return atomic_load(&g_sweep_fd_wait_held_for_tests);
}

// Writes the trace of the armed hold into buf, so a failing test can print why.
void chttp__sweep_fd_wait_trace_for_tests(char *buf, size_t size) {
  if (!buf || size == 0) return;
  snprintf(buf, size,
           "ctxs seen %lu, not armed %lu, had an fd %lu, wrong state %lu, "
           "had a race %lu, last turned away fd %d state %d, lookup wait "
           "end %d (1 caught, 2 ran out, 3 not waited), hold end %d (0 not "
           "entered or still waiting, 1 no fd registered within 5 s, 2 "
           "held)",
           atomic_load(&g_sweep_fd_wait_seen_for_tests),
           atomic_load(&g_sweep_fd_wait_not_armed_for_tests),
           atomic_load(&g_sweep_fd_wait_had_fd_for_tests),
           atomic_load(&g_sweep_fd_wait_wrong_state_for_tests),
           atomic_load(&g_sweep_fd_wait_had_race_for_tests),
           atomic_load(&g_sweep_fd_wait_last_fd_for_tests),
           atomic_load(&g_sweep_fd_wait_last_state_for_tests),
           atomic_load(&g_sweep_fd_wait_lookup_end_for_tests),
           atomic_load(&g_sweep_fd_wait_hold_end_for_tests));
}

// Gives g_async_hops_abandoned, so a test can check that abandoned hops are
// counted and released.
size_t chttp__async_hops_abandoned_for_tests(void) {
  return atomic_load(&g_async_hops_abandoned);
}
#endif

static uint64_t async_race_tick(chttp_connect_race *r, bool expired);

static void async_chain_retain(chttp_async_chain *chain);
static void async_chain_release(chttp_async_chain *chain);
static void async_fulfill_chain(chttp_async_chain *chain, ccol_retval rv,
                                chttpcli_response *resp);

// Gives the earlier of next_ns and d's instant when d is active and still
// ahead, so a walk can find the sweep's next wake.
static uint64_t sweep_earlier(uint64_t next_ns, const chttp_deadline *d,
                              bool remaining) {
  if (!remaining || !d->active) return next_ns;
  uint64_t at = timespec_to_ns(&d->deadline);
  return at < next_ns ? at : next_ns;
}

// Runs one sweep pass. It walks the stripes one at a time, holding only that
// stripe's lock. For each ctx it snapshots state and chain, judges the connect,
// request, 100-continue and inactivity deadlines and marks timed_out, all under
// the ctx's deadline_lock, so the verdict is one step against the idle pool's
// offer and take. An expired connection gets its fd shut down (timed_out first,
// so the teardown reports a timeout and repeated ticks are harmless). A hop
// with no fd before its connect, or one marked earlier that never got its
// dispatch, is answered with ccol_timed_out after the stripes are unlocked. The
// pass also starts Happy Eyeballs attempts, flips stalled duplex sends, retries
// an abandoned reap, and lowers g_sweep_next_ns to the next work.
static void client_deadline_sweep_once(void) {
  ccol__call_once(client_deadline_bundle.once, client_deadline_init_globals);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_sweep_ticks_for_tests, 1);
#endif
  size_t n_shutdown = 0;
  uint64_t next_ns = UINT64_MAX;
  chttp_async_chain *abandoned[CHTTP_DEADLINE_SWEEP_BATCH];
  size_t n_abandoned = 0;

  for (size_t s = 0; s < CHTTP_DEADLINE_SWEEP_STRIPES &&
                     n_shutdown < CHTTP_DEADLINE_SWEEP_BATCH;
       s++) {
    chttp_deadline_stripe *stripe = &client_deadline_bundle.stripes[s];
    ccol__mutex_lock(stripe->mutex);
    chttp_async_ctx *node = stripe->head;
    while (node && n_shutdown < CHTTP_DEADLINE_SWEEP_BATCH) {
      chttp_async_ctx *next = node->deadline_next;
#ifdef RUNNING_UNIT_TESTS
      sweep_fd_wait_for_tests(node);
#endif

      bool expired = false;
      ccol__mutex_lock(node->deadline_lock);
      chttp_async_state state = node->state;
      chttp_deadline continue_dl = node->continue_deadline;
      chttp_async_chain *chain_snap = node->chain;
      bool has_chain = (chain_snap != NULL);
      if (node->timed_out) {
        if (has_chain && n_abandoned < CHTTP_DEADLINE_SWEEP_BATCH &&
            !atomic_exchange(&node->abandoned, true)) {
          atomic_fetch_add(&g_async_hops_abandoned, 1);
          async_chain_retain(chain_snap);
          abandoned[n_abandoned++] = chain_snap;
          n_shutdown++;
        }
      } else {
        int ms;
        if (state == CHTTP_ASYNC_CONNECTING ||
            state == CHTTP_ASYNC_TLS_HANDSHAKING) {
          bool left = deadline_remaining_ms(&node->connect_deadline, &ms);
          if (!left) {
            expired = true;
#ifdef RUNNING_UNIT_TESTS
            sweep_note_due_for_tests(
                0, timespec_to_ns(&node->connect_deadline.deadline));
#endif
          }
          next_ns = sweep_earlier(next_ns, &node->connect_deadline, left);
        }
        if (!expired && has_chain) {
          bool left = deadline_remaining_ms(&node->overall_deadline, &ms);
          if (!left) {
            expired = true;
#ifdef RUNNING_UNIT_TESTS
            sweep_note_due_for_tests(
                0, timespec_to_ns(&node->overall_deadline.deadline));
#endif
          }
          next_ns = sweep_earlier(next_ns, &node->overall_deadline, left);
        }
        if (!expired && has_chain && node->inactivity_us != 0 &&
            (state == CHTTP_ASYNC_WRITING ||
             state == CHTTP_ASYNC_AWAITING_CONTINUE ||
             state == CHTTP_ASYNC_READING) &&
            !atomic_load(&chain_snap->in_sink)) {
          uint64_t now_ns = monotonic_now_ns();
          uint64_t last = atomic_load(&node->last_io_ns);
          uint64_t sink = atomic_load(&chain_snap->sink_returned_ns);
          if (sink > last) last = sink;
          if (last == 0) {
            atomic_store(&node->last_io_ns, now_ns);
            last = now_ns;
          }
          uint64_t budget_ns = node->inactivity_us > UINT64_MAX / 1000
                                   ? UINT64_MAX
                                   : node->inactivity_us * 1000;
          uint64_t due =
              last > UINT64_MAX - budget_ns ? UINT64_MAX : last + budget_ns;
          if (now_ns >= due) {
            expired = true;
#ifdef RUNNING_UNIT_TESTS
            sweep_note_due_for_tests(0, due);
#endif
          } else if (due < next_ns) {
            next_ns = due;
          }
        }
        if (!expired && state == CHTTP_ASYNC_AWAITING_CONTINUE &&
            !atomic_load(&node->continue_decided) &&
            !atomic_load(&node->continue_msg_started))
          next_ns = sweep_earlier(next_ns, &continue_dl,
                                  deadline_remaining_ms(&continue_dl, &ms));
#ifdef RUNNING_UNIT_TESTS
        bool forced = !expired && has_chain && state == CHTTP_ASYNC_READING &&
                      sweep_race_take_forced_verdict_for_tests(node);
        if (forced) {
          expired = true;
          sweep_race_park_for_tests(1);
        }
#endif
        if (expired) node->timed_out = true;
      }
      ccol__mutex_unlock(node->deadline_lock);
      int fd = node->fd;
#ifdef RUNNING_UNIT_TESTS
      if (state == CHTTP_ASYNC_AWAITING_CONTINUE)
        continue_sweep_park_for_tests();
#endif

      chttp_connect_race *race =
          (state == CHTTP_ASYNC_CONNECTING) ? atomic_load(&node->race) : NULL;
      if (expired && fd < 0 && !race && has_chain &&
          state == CHTTP_ASYNC_CONNECTING &&
          n_abandoned < CHTTP_DEADLINE_SWEEP_BATCH &&
          !atomic_exchange(&node->abandoned, true)) {
        atomic_fetch_add(&g_async_hops_abandoned, 1);
        async_chain_retain(chain_snap);
        abandoned[n_abandoned++] = chain_snap;
      }
      if (expired) {
#ifdef RUNNING_UNIT_TESTS
        if (atomic_load(&g_sweep_race_forced_ctx_for_tests) == node)
          sweep_race_park_for_tests(2);
#endif
        if (fd >= 0) shutdown(fd, SHUT_RDWR);
        if (race) (void)async_race_tick(race, true);
        n_shutdown++;
      } else if (race) {
        uint64_t start_ns = async_race_tick(race, false);
        if (start_ns < next_ns) next_ns = start_ns;
      } else if (fd >= 0 && atomic_load(&node->flip_watch) != CHTTP_FLIP_NONE) {
        unsigned char watch = atomic_load(&node->flip_watch);
        bool on_input = (watch == CHTTP_FLIP_ON_INPUT);
        struct pollfd p = {
            .fd = fd, .events = on_input ? POLLIN : POLLOUT, .revents = 0};
        if (poll(&p, 1, 0) > 0 && (p.revents & p.events) != 0) {
          ceventloop_reg reg = atomic_load(&node->reg);
          if (reg && atomic_load(&node->flip_watch) == watch)
            ceventloop_modify(cli_engine_bundler.reactor, reg,
                              on_input ? cselect_read : cselect_write);
        }
      } else if (state == CHTTP_ASYNC_AWAITING_CONTINUE) {
        int cms;
        if (!deadline_remaining_ms(&continue_dl, &cms) &&
            !atomic_load(&node->continue_decided) &&
            !atomic_load(&node->continue_msg_started)) {
          ceventloop_reg reg = atomic_load(&node->reg);
          if (reg && node->state == CHTTP_ASYNC_AWAITING_CONTINUE &&
              !atomic_load(&node->continue_decided)) {
            ceventloop_modify(cli_engine_bundler.reactor, reg, cselect_write);
          }
        }
      }
      node = next;
    }
    ccol__mutex_unlock(stripe->mutex);
  }
  if (n_shutdown >= CHTTP_DEADLINE_SWEEP_BATCH) next_ns = 0;
  client_deadline_note_ns(next_ns);

  for (size_t i = 0; i < n_abandoned; i++) {
    if (abandoned[i]->write_fn) async_sink_close(abandoned[i]);
    async_fulfill_chain(abandoned[i], ccol_timed_out, NULL);
    async_chain_release(abandoned[i]);
  }

  if (n_shutdown > 0) {
    CLIENT_ENGINE_LOG_INFO("deadline sweep shut down %zu connection(s)",
                           n_shutdown);
  }
}

// Runs the sweep thread: It sleeps until g_sweep_next_ns, and at most the
// interval after the end of its previous walk. A wake for an earlier deadline
// only replans; the walk runs once the planned instant has come, so requests
// that start in a row don't cost a walk each.
static void *client_deadline_sweep_fn(void *arg) {
  (void)arg;
  t_on_sweep_thread = true;
  ccol__call_once(client_deadline_bundle.once, client_deadline_init_globals);
  const uint64_t interval_ns =
      (uint64_t)CHTTP_DEADLINE_SWEEP_INTERVAL_MS * 1000000u;
  uint64_t last_walk_ns = monotonic_now_ns();
  ccol__mutex_lock(client_deadline_bundle.mutex);
  while (!client_deadline_bundle.stop) {
    uint64_t now_ns = monotonic_now_ns();
    uint64_t wake_ns = last_walk_ns + interval_ns;
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
      ccol__cond_var_timedwait(client_deadline_bundle.cond_var,
                               client_deadline_bundle.mutex, wake);
      continue;
    }
    atomic_store(&g_sweep_planned_ns, 0);
    ccol__mutex_unlock(client_deadline_bundle.mutex);
    atomic_store(&g_sweep_next_ns, UINT64_MAX);
#ifdef RUNNING_UNIT_TESTS
    g_sweep_walk_plan_ns_for_tests = wake_ns;
#endif
    client_deadline_sweep_once();
    client_engine_retry_abandoned_reap();
    last_walk_ns = monotonic_now_ns();
    ccol__mutex_lock(client_deadline_bundle.mutex);
  }
  atomic_store(&g_sweep_planned_ns, 0);
  ccol__mutex_unlock(client_deadline_bundle.mutex);
  return NULL;
}

// Starts the sweep thread, from the first engine acquire. Gives
// ccol_unexpected_failure when the thread can't start.
static ccol_retval client_deadline_sweep_start(void) {
  client_deadline_bundle.stop = false;
  int rc = ccol__thread_create(client_deadline_bundle.thread,
                               client_deadline_sweep_fn, NULL);
  return (rc == 0) ? ccol_success : ccol_unexpected_failure;
}

// Stops the sweep thread and joins it, from the reaper, before the reactor and
// the DNS pool go away.
static void client_deadline_sweep_stop_and_join(void) {
  ccol__call_once(client_deadline_bundle.once, client_deadline_init_globals);
  ccol__mutex_lock(client_deadline_bundle.mutex);
  client_deadline_bundle.stop = true;
  ccol__cond_var_broadcast(client_deadline_bundle.cond_var);
  ccol__mutex_unlock(client_deadline_bundle.mutex);
  ccol__thread_join(client_deadline_bundle.thread);
}

#if CCOL_FORK_SAFETY_REQUIRED

// Fork safety
//
// Takes every lock of this module before fork(): It waits for a stopping engine
// and joins its reaper (which keeps running after it clears stopping and may
// hold an allocator lock), then takes the engine mutex, the sweep mutex and
// every stripe lock, so no lock of this module is held by a thread the child
// doesn't inherit.
static void chttpcli_atfork_prepare(void) {
#ifdef RUNNING_UNIT_TESTS
  ccol__atfork_order_record(ccol__atfork_module_chttpclient);
#endif
  ccol__call_once(cli_engine_bundler.once, client_engine_globals_init);
  ccol__call_once(client_deadline_bundle.once, client_deadline_init_globals);
  ccol__mutex_lock(cli_engine_bundler.mutex);
  if (atomic_load(&g_client_engine_pid) == getpid()) {
    while (cli_engine_bundler.stopping)
      ccol__cond_var_wait(cli_engine_bundler.stopped_cv,
                          cli_engine_bundler.mutex);
    client_engine_join_reaper_if_needed_locked();
  }
  ccol__mutex_lock(client_deadline_bundle.mutex);
  for (size_t i = 0; i < CHTTP_DEADLINE_SWEEP_STRIPES; i++)
    ccol__mutex_lock(client_deadline_bundle.stripes[i].mutex);
}

// Releases every lock the prepare handler took, in reverse order, for both the
// parent and the child handler.
static void chttpcli_atfork_unlock_all(void) {
  for (size_t i = CHTTP_DEADLINE_SWEEP_STRIPES; i-- > 0;)
    ccol__mutex_unlock(client_deadline_bundle.stripes[i].mutex);
  ccol__mutex_unlock(client_deadline_bundle.mutex);
  ccol__mutex_unlock(cli_engine_bundler.mutex);
}

// Releases every lock of this module in the parent after fork().
static void chttpcli_atfork_parent(void) { chttpcli_atfork_unlock_all(); }

// Forgets the inherited engine in the child after fork(), then unlocks every
// lock; the forking thread holds every lock in the child too, so a plain unlock
// is right.
static void chttpcli_atfork_child(void) {
  client_engine_forget_inherited_locked();
  chttpcli_atfork_unlock_all();
}
#endif

// Deep-copies a header map (char * -> char *) for a redirect chain, through the
// insertion-order cursor, which can't fail. Gives NULL for no memory and also
// for a NULL src; *oom tells the two apart.
static chmap clone_headers_map(ccol_memmgmt_procs *mp, chmap src) {
  char *err = NULL;
  chmap dst =
      chashmap_create_full(CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                           ccol_string, mp, NULL, NULL, &err);
  if (!dst) return NULL;
  for (const ccol_chmap_entry_ref *e = ccol_chmap_oldest_entry(src); e;) {
    const cmap_pair *k, *v;
    e = ccol_chmap_entry_read(e, &k, &v);
    if (chashmap_insert_elem(dst, k, v) != ccol_success) {
      chashmap__destroy(dst);
      return NULL;
    }
  }
  return dst;
}

// Allocates the shared state of a redirect chain for chttp_do_async_internal
// and counts it in cli's async in-flight total, so destroy waits for it. On
// success it owns tls_ctx; on failure tls_ctx stays with the caller. Headers,
// body and content type are deep-copied, so the caller can free its request at
// once.
static chttp_async_chain *async_chain_create(
    ccol_memmgmt_procs *mp, struct chttpclient *cli, cthrpool_future *future,
    chmap req_headers, const void *body_data, size_t body_len,
    const char *body_content_type, const char *initial_origin_key,
    ctls_ctx *tls_ctx, bool tls_ctx_usable, bool verify_host,
    uint64_t tls_generation, uint64_t connect_timeout_us,
    uint64_t request_timeout_us, size_t max_response_body_size,
    chttpcli_write_fn write_fn, void *write_ctx, bool expect_continue,
    bool prevent_tls_downgrade_on_redirect) {
  chttp_async_chain *chain =
      (chttp_async_chain *)ccol__mem_calloc(mp, 1, sizeof(*chain));
  if (!chain) return NULL;
  if (ccol__mutex_init(chain->lock) != 0) {
    ccol__mem_free(mp, chain);
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
  chain->overall_deadline = deadline_make(request_timeout_us);
  chain->max_response_body_size = max_response_body_size;
  chain->write_fn = write_fn;
  chain->write_ctx = write_ctx;
  chain->expect_continue = expect_continue;
  chain->prevent_tls_downgrade_on_redirect = prevent_tls_downgrade_on_redirect;

  if (req_headers) {
    chain->req_headers = clone_headers_map(mp, req_headers);
    if (!chain->req_headers) goto fail;
  }
  if (body_data && body_len > 0) {
    chain->body_data = ccol__mem_alloc(mp, body_len);
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

  ccol__mutex_lock(cli->async_count_lock);
  cli->async_in_flight_count++;
  ccol__mutex_unlock(cli->async_count_lock);

  return chain;

fail:
  if (chain->req_headers) chashmap__destroy(chain->req_headers);
  ccol__mem_free(mp, chain->body_data);
  ccol__mem_free(mp, chain->body_content_type);
  ccol__mem_free(mp, chain->initial_origin_key);
  ccol__mutex_destroy(chain->lock);
  ccol__mem_free(mp, chain);
  return NULL;
}

// Allocates size bytes plus, for a non-NULL mp, a copy of the procs right after
// them, pointed to by *procs_slot. A response or a result may be freed after
// its client is gone, so it carries its own allocator.
static void *alloc_with_procs_copy(ccol_memmgmt_procs *mp, size_t size,
                                   ccol_memmgmt_procs **copy_out) {
  size_t off = (size + _Alignof(ccol_memmgmt_procs) - 1) &
               ~(size_t)(_Alignof(ccol_memmgmt_procs) - 1);
  char *block = (char *)ccol__mem_calloc(
      mp, 1, mp ? off + sizeof(ccol_memmgmt_procs) : size);
  if (!block) return NULL;
  *copy_out = NULL;
  if (mp) {
    ccol_memmgmt_procs *copy = (ccol_memmgmt_procs *)(block + off);
    *copy = *mp;
    *copy_out = copy;
  }
  return block;
}

// Allocate a response and an async result with their own procs copy, since both
// may be freed after their client is destroyed.
static chttpcli_response *resp_alloc(ccol_memmgmt_procs *mp) {
  ccol_memmgmt_procs *copy;
  chttpcli_response *resp = (chttpcli_response *)alloc_with_procs_copy(
      mp, sizeof(chttpcli_response), &copy);
  if (resp) resp->ccol__m_procs = copy;
  return resp;
}

static chttpcli_async_result *async_result_alloc(ccol_memmgmt_procs *mp) {
  ccol_memmgmt_procs *copy;
  chttpcli_async_result *result =
      (chttpcli_async_result *)alloc_with_procs_copy(
          mp, sizeof(chttpcli_async_result), &copy);
  if (result) result->ccol__m_procs = copy;
  return result;
}

static void async_fulfill_chain(chttp_async_chain *chain, ccol_retval rv,
                                chttpcli_response *resp);

// Adds one hop reference to a live chain, for a redirect hand-off or a retry.
// Pairs with async_chain_release.
static void async_chain_retain(chttp_async_chain *chain) {
  ccol__mutex_lock(chain->lock);
  chain->refcount++;
  ccol__mutex_unlock(chain->lock);
}

// Drops one hop reference. The last one is the one reliable place to end the
// request: It fulfils the future as a backstop (a no-op when a hop already
// did), takes the chain off cli's async count under async_count_lock and wakes
// a waiting destroy, frees the chain and drops its engine reference.
static void async_chain_release(chttp_async_chain *chain) {
  if (!chain) return;
  ccol__mutex_lock(chain->lock);
  int remaining = --chain->refcount;
  ccol__mutex_unlock(chain->lock);
  if (remaining > 0) return;

  struct chttpclient *cli = chain->cli;

  async_fulfill_chain(chain, ccol_http_transfer_aborted, NULL);
  if (chain->tls_ctx) ctls_ctx_release(chain->tls_ctx);
  if (chain->req_headers) chashmap__destroy(chain->req_headers);
  ccol__mem_free(chain->mp, chain->body_data);
  ccol__mem_free(chain->mp, chain->body_content_type);
  ccol__mem_free(chain->mp, chain->carried_auth);
  ccol__mem_free(chain->mp, chain->carried_auth_origin);
  ccol__mem_free(chain->mp, chain->initial_origin_key);
  ccol__mutex_destroy(chain->lock);
  ccol__mem_free(chain->mp, chain);
  client_engine_release_chain();

  ccol__mutex_lock(cli->async_count_lock);
  if (cli->async_in_flight_count > 0) cli->async_in_flight_count--;
  if (cli->async_in_flight_count == 0)
    ccol__cond_var_broadcast(cli->async_count_drained);
  ccol__mutex_unlock(cli->async_count_lock);
}

// Allocates a ctx with its locks, fd -1 and no registration, for a fresh hop or
// a retry. Gives NULL on failure.
static chttp_async_ctx *async_ctx_create(ccol_memmgmt_procs *mp) {
  chttp_async_ctx *ctx =
      (chttp_async_ctx *)ccol__mem_calloc(mp, 1, sizeof(*ctx));
  if (!ctx) return NULL;
  ctx->mp = mp;
  ctx->fd = -1;
  if (ccol__mutex_init(ctx->idle_lock) != 0) {
    ccol__mem_free(mp, ctx);
    return NULL;
  }
  if (ccol__mutex_init(ctx->deadline_lock) != 0) {
    ccol__mutex_destroy(ctx->idle_lock);
    ccol__mem_free(mp, ctx);
    return NULL;
  }
  return ctx;
}

// Gets ctx->parser ready for the next response, releasing the line spill of an
// earlier one.
static void async_parser_start(chttp_async_ctx *ctx) {
  ccol__call_once(client_http1_settings_bundler.once, init_chttp1_settings);
  chttp1_parser_release(&ctx->parser);
  chttp1_parser_init(&ctx->parser, &client_http1_settings_bundler.settings);
  ctx->parser.data = &ctx->pctx;
  (void)chttp1_parser_enable_line_spill(&ctx->parser, ctx->mp);
}

static void async_race_detach(chttp_async_ctx *ctx);
static ccol_retval async_reg_modify(chttp_async_ctx *ctx, cselect_dir dir);
static void async_ctx_handle_if_abandoned(chttp_async_ctx *ctx);
static inline bool async_upload_may_resume(const chttp_async_ctx *ctx);
static void async_upload_resume(chttp_async_ctx *ctx);

// Releases everything a ctx holds and frees it. It leaves the deadline registry
// first, strictly before the close(), since the sweep shuts fds down and would
// otherwise hit a reused fd number. The registration is removed before the
// close too. A short lock of idle_lock first makes sure async_connect_task's
// window after ceventloop_add is over.
static void async_ctx_destroy_now(chttp_async_ctx *ctx) {
  ccol__mutex_lock(ctx->idle_lock);
  ccol__mutex_unlock(ctx->idle_lock);
  client_deadline_unregister(ctx);
  if (atomic_load(&ctx->abandoned))
    atomic_fetch_sub(&g_async_hops_abandoned, 1);
  async_race_detach(ctx);
  if (ctx->reg) ceventloop_remove(cli_engine_bundler.reactor, ctx->reg);
  ceventloop_reg room = atomic_exchange(&ctx->room_reg, CEVENTLOOP_REG_NULL);
  if (room) ceventloop_remove(cli_engine_bundler.reactor, room);
  if (ctx->tls) ctls_conn_destroy(ctx->tls);
  if (ctx->fd >= 0) close(ctx->fd);
  chttp1_parser_release(&ctx->parser);
  ccol__mutex_destroy(ctx->idle_lock);
  ccol__mutex_destroy(ctx->deadline_lock);
  ccol__mem_free(ctx->mp, ctx->wire);
  ccol__mem_free(ctx->mp, ctx->continue_carry);
  ccol__mem_free(ctx->mp, ctx->unix_socket_path);
  ccol__mem_free(ctx->mp, ctx->host);
  ccol__mem_free(ctx->mp, ctx->addrs);
  ccol__mem_free(ctx->mp, ctx->path_and_query);
  ccol__mem_free(ctx->mp, ctx->origin_key);
  parse_ctx_free_fields(&ctx->pctx);
  ccol__mem_free(ctx->mp, ctx->bb.buf);
  ccol__mem_free(ctx->mp, ctx);
}

// Frees a ctx; it's the one place that does. No application thread ever frees a
// registered ctx, so this runs either for a ctx that was never registered or
// inside a dispatch.
static void async_ctx_free(chttp_async_ctx *ctx) {
  if (!ctx) return;
  async_ctx_destroy_now(ctx);
}

// Frees a hop's ctx and drops its chain reference. Every terminal path of a ctx
// ends here, so the pair can't get out of step.
static void async_ctx_teardown(chttp_async_ctx *ctx) {
  chttp_async_chain *chain = ctx->chain;
  async_ctx_free(ctx);
  async_chain_release(chain);
}

// Hands the terminal result to the future exactly once. Every exit path of a
// request may call it; chain->fulfilled under chain->lock decides, since two
// hops' callbacks can run at once. It also settles g_async_requests_pending
// before the future becomes ready.
static void async_fulfill_chain(chttp_async_chain *chain, ccol_retval rv,
                                chttpcli_response *resp) {
  ccol__mutex_lock(chain->lock);
  bool already = chain->fulfilled;
  chain->fulfilled = true;
  chttpcli_async_result *result = chain->result;
  chain->result = NULL;
  ccol__mutex_unlock(chain->lock);
  if (already) {
    chttpcli_resp_free(resp);
    return;
  }
  client_async_request_settled();
  result->rv = rv;
  result->resp = resp;
  cthrpool_future_fulfill(chain->future, result);
}

// Marks the hop completed, so a later dispatch does nothing, and fulfils its
// chain with rv and resp.
static void async_fulfill(chttp_async_ctx *ctx, ccol_retval rv,
                          chttpcli_response *resp) {
  ctx->hop_completed = true;
  async_fulfill_chain(ctx->chain, rv, resp);
}

// Builds the response from the finished parse. The header map, field lines and
// body move out of ctx, so call it before the idle-pool offer resets ctx.
static chttpcli_response *async_build_response(chttp_async_ctx *ctx) {
  chttpcli_response *resp = resp_alloc(ctx->mp);
  if (!resp) return NULL;
  resp->status_code = ctx->pctx.status_code;
  if (ctx->chain->write_fn) {
    parse_ctx_free_fields(&ctx->pctx);
  } else {
    resp->body = ctx->bb.buf;
    resp->body_len = ctx->bb.len;
    resp->headers = ctx->pctx.headers;
    ctx->pctx.headers = NULL;
    resp->ccol__field_lines = ctx->pctx.field_lines;
    ctx->pctx.field_lines = NULL;
    ctx->bb.buf = NULL;
  }
  return resp;
}

// Builds the response and fulfils the future, for the EOF-driven completion,
// which never pools its connection.
static void async_fulfill_success(chttp_async_ctx *ctx) {
  chttpcli_response *resp = async_build_response(ctx);
  async_fulfill(ctx, resp ? ccol_success : ccol_not_enough_memory, resp);
}

#ifdef RUNNING_UNIT_TESTS
// Async idle pool
//
// Tier 2's pool, chmap(origin_key -> cvec of chttp_async_ctx *), with the same
// caps as Tier 1. A pooled connection stays registered with the reactor, so an
// IDLE dispatch notices a close or stray bytes and evicts it.
//
// Test hooks: Fail the push of an offer, fail the reactivation of a taken
// connection, and force a hard read error.
static _Atomic bool g_force_offer_push_fail_for_tests = false;
static _Atomic bool g_force_reactivate_fail_for_tests = false;
static _Atomic bool g_force_async_hard_read_error_for_tests = false;

// Test hooks for the write-direction fall-through of async_on_writable_impl: A
// sweep flip that lands on a ctx that already moved on to reading. The first
// forces such a stray write arm, the second does it for an idle ctx, and the
// counter counts how often the stray arm was undone.
static _Atomic bool g_force_stray_write_arm_for_tests = false;
static _Atomic bool g_force_stray_idle_write_arm_for_tests = false;
static _Atomic unsigned long long g_stray_write_arm_restores_for_tests = 0;

// Widens the window around the replay of the 100-continue carry in
// async_upload_pause, where a destroy may take the client's allocator away once
// the chain's last reference is gone.
static _Atomic long g_continue_carry_replay_delay_ms_for_tests = 0;
#ifdef RUNNING_UNIT_TESTS
// See chttpcli__arm_stray_write_at_take_for_tests.
static _Atomic bool g_stray_write_at_take_for_tests = false;
// See chttpcli__arm_stray_write_before_take_for_tests: 0 off, 1 armed, 2 the
// take armed the write direction, 3 the dispatch saw IDLE and waits, 4 the take
// moved the ctx to WRITING.
static _Atomic int g_stray_before_take_stage_for_tests = 0;

// Waits at most 2 s until g_stray_before_take_stage_for_tests reaches stage, so
// the two sides of the stray-write test run in order.
static void stray_before_take_wait_for_tests(int stage) {
  for (int i = 0; i < 2000; i++) {
    if (atomic_load(&g_stray_before_take_stage_for_tests) >= stage) return;
    struct timespec ts = {0, 1000000L};
    nanosleep(&ts, NULL);
  }
}
#endif

// Sleeps the carry replay delay when a test set one, to widen the window around
// the replay.
static void chttp_test_continue_carry_replay_delay(void) {
  long ms = atomic_load(&g_continue_carry_replay_delay_ms_for_tests);
  if (ms <= 0) return;
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

// Holds the IDLE branch of async_on_readable_impl once, between deciding the
// ctx is idle and reading, so a take can win against the evicting dispatch.
// Only a dispatch that finds input waiting is held. 0 off, 1 armed, 2 holding,
// 3 released; it ends after 5 s.
static _Atomic int g_idle_probe_hold_for_tests = 0;

// Runs the IDLE probe hold for fd, only when input is waiting, so the take can
// win against the dispatch.
static void chttp_test_idle_probe_hold(int fd) {
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
#endif

// Takes one off cli->idle_total_count_async and, at zero, wakes the destroy
// that waits on idle_async_drained for the pool to empty. The caller holds
// cli->lock.
static void async_idle_count_dec_locked(struct chttpclient *cli) {
  if (cli->idle_total_count_async > 0) cli->idle_total_count_async--;
  if (cli->idle_total_count_async == 0)
    ccol__cond_var_broadcast(cli->idle_async_drained);
}

// Removes target from its origin's idle list by swapping with the last element,
// drops the origin's entry once its list is empty, and gives true. It gives
// false when target isn't there (another path removed it first), which lets two
// paths race for it safely. The caller holds cli->lock.
static bool async_idle_remove_locked(struct chttpclient *cli,
                                     chttp_async_ctx *target) {
  if (!cli->idle_pools_async || !target->origin_key) return false;
  cmap_pair kp = {.ptr = target->origin_key,
                  .size = strlen(target->origin_key) + 1};
  cmap_pair vp;
  if (chashmap_get_elem_ref(cli->idle_pools_async, &kp, &vp) != ccol_success)
    return false;
  cvec list = read_cvec(vp.ptr);
  if (!list) return false;
  size_t n = cvector_elem_count(list);
  for (size_t i = 0; i < n; i++) {
    chttp_async_ctx **slot = (chttp_async_ctx **)cvector_at(list, i);
    if (*slot != target) continue;
    chttp_async_ctx *last = NULL;
    cvector_pop_back(list, &last);
    if (i < cvector_elem_count(list)) {
      chttp_async_ctx **slot2 = (chttp_async_ctx **)cvector_at(list, i);
      *slot2 = last;
    }
    if (cvector_elem_count(list) == 0) {
      cvector__destroy(list);
      chashmap_delete_elem(cli->idle_pools_async, &kp);
    }
    return true;
  }
  return false;
}

// Pops a usable idle connection for origin_key into *out, evicting stale,
// TLS-outdated and closed candidates. On a true return the ctx's idle_lock is
// held; the caller (async_submit_hop) unlocks it once the ctx is set up for the
// new hop and its write was tried.
static bool async_idle_pool_take(struct chttpclient *cli,
                                 const char *origin_key,
                                 chttp_async_chain *chain,
                                 chttp_async_ctx **out) {
  chttp_async_ctx *ctx = NULL;
  ccol__mutex_lock(cli->lock);
  if (cli->idle_pools_async) {
    cmap_pair kp = {.ptr = (void *)origin_key, .size = strlen(origin_key) + 1};
    cmap_pair vp;
    if (chashmap_get_elem_ref(cli->idle_pools_async, &kp, &vp) ==
        ccol_success) {
      cvec list = read_cvec(vp.ptr);
      if (list) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        size_t n = cvector_elem_count(list);
        for (size_t idx = n; idx-- > 0;) {
          chttp_async_ctx **slot = (chttp_async_ctx **)cvector_at(list, idx);
          chttp_async_ctx *cand = *slot;
          int64_t age_ms = ms_elapsed_floor(&cand->last_used, &now);
          if (age_ms > CHTTP_IDLE_MAX_AGE_MS ||
              (cand->is_https && cand->tls_generation != cli->tls_generation)) {
            int fd = cand->fd;
            if (fd >= 0) shutdown(fd, SHUT_RDWR);
            continue;
          }
          chttp_async_ctx *last = NULL;
          cvector_pop_back(list, &last);
          if (idx < cvector_elem_count(list)) {
            chttp_async_ctx **slot2 = (chttp_async_ctx **)cvector_at(list, idx);
            *slot2 = last;
          }
          async_idle_count_dec_locked(cli);
          ctx = cand;
          if (cvector_elem_count(list) == 0) {
            cvector__destroy(list);
            chashmap_delete_elem(cli->idle_pools_async, &kp);
          }
          break;
        }
      }
    }
  }
  ccol__mutex_unlock(cli->lock);
  if (!ctx) return false;

#ifdef RUNNING_UNIT_TESTS
  int stray_armed = 1;
  bool stray_before_take = atomic_compare_exchange_strong(
      &g_stray_before_take_stage_for_tests, &stray_armed, 2);
  if (stray_before_take) {
    (void)ceventloop_modify(cli_engine_bundler.reactor, ctx->reg,
                            cselect_write);
    stray_before_take_wait_for_tests(3);
  }
#endif
  ccol__mutex_lock(ctx->idle_lock);
#ifdef RUNNING_UNIT_TESTS
  ctx->idle_lock_held_for_tests = true;
#endif
  ccol__mutex_lock(ctx->deadline_lock);
  ctx->overall_deadline = chain->overall_deadline;
  ctx->inactivity_us = chain->inactivity_timeout_us;
  atomic_store(&ctx->last_io_ns, 0);
  ctx->timed_out = false;
  ctx->chain = chain;
  ctx->state = CHTTP_ASYNC_WRITING;
  ccol__mutex_unlock(ctx->deadline_lock);
  client_deadline_note(&chain->overall_deadline);
#ifdef RUNNING_UNIT_TESTS
  if (stray_before_take) {
    atomic_store(&g_stray_before_take_stage_for_tests, 4);
    struct timespec ts = {0, 200000000L};
    nanosleep(&ts, NULL);
  }
#endif
  *out = ctx;
  return true;
}

// Offers a finished connection back to the idle pool. false means the ctx was
// left completely untouched and the caller closes it. true means the pool owns
// it: It's in state IDLE with no chain, holds its own engine reference, and is
// off the deadline registry. The state change happens under idle_lock and
// deadline_lock, so the sweep can never judge it half made.
static bool async_idle_pool_offer(chttp_async_ctx *ctx) {
  struct chttpclient *cli = ctx->cli;

  if (client_engine_acquire() != ccol_success) return false;

  cvec list = NULL;
  bool has_room = false;
  ccol__mutex_lock(cli->lock);
  if (!cli->destroying && cli->idle_pools_async &&
      cli->idle_total_count_async < CHTTP_MAX_IDLE_TOTAL) {
    cmap_pair kp = {.ptr = ctx->origin_key,
                    .size = strlen(ctx->origin_key) + 1};
    cmap_pair vp;
    if (chashmap_get_elem_ref(cli->idle_pools_async, &kp, &vp) ==
        ccol_success) {
      list = read_cvec(vp.ptr);
    }
    if (!list && chashmap_elem_count(cli->idle_pools_async) <
                     effective_max_idle_origins()) {
      char *cverr = NULL;
      list =
          cvector_create_full(sizeof(chttp_async_ctx *), cli->m_procs, &cverr);
      if (list) {
        cmap_pair vp2 = {.ptr = &list, .size = sizeof(list)};
        if (chashmap_insert_elem(cli->idle_pools_async, &kp, &vp2) !=
            ccol_success) {
          cvector__destroy(list);
          list = NULL;
        }
      }
    }
    has_room = (list && cvector_elem_count(list) < CHTTP_MAX_IDLE_PER_ORIGIN);
  }
  if (!has_room) {
    ccol__mutex_unlock(cli->lock);
    client_engine_release();
    return false;
  }

  chttp_async_chain *old_chain = ctx->chain;

  parse_ctx_free_fields(&ctx->pctx);
  memset(&ctx->pctx, 0, sizeof(ctx->pctx));
  chttp1_parser_release(&ctx->parser);
  ccol__mem_free(ctx->mp, ctx->bb.buf);
  ctx->bb.buf = NULL;
  ctx->bb.len = ctx->bb.cap = 0;
  ctx->bb.oom = false;
  ctx->bb.too_large = false;
  ccol__mem_free(ctx->mp, ctx->wire);
  ctx->wire = NULL;
  ctx->wire_len = ctx->wire_sent = 0;
  ccol__mem_free(ctx->mp, ctx->continue_carry);
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
  ccol__mem_free(ctx->mp, ctx->path_and_query);
  ctx->path_and_query = NULL;
  ctx->hop_completed = false;
  ctx->reused = false;
  ctx->any_bytes_read = false;
  ctx->interim_responses_seen = 0;
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_offer_entered_for_tests, 1);
#endif
  ccol__mutex_lock(ctx->idle_lock);
  ccol__mutex_lock(ctx->deadline_lock);
  ctx->chain = NULL;
  ctx->state = CHTTP_ASYNC_IDLE;
  ctx->timed_out = false;
  if (atomic_exchange(&ctx->abandoned, false))
    atomic_fetch_sub(&g_async_hops_abandoned, 1);
  ccol__mutex_unlock(ctx->deadline_lock);
  ccol__mutex_unlock(ctx->idle_lock);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_offer_done_for_tests, 1);
#endif

  clock_gettime(CLOCK_MONOTONIC, &ctx->last_used);
  bool pushed;
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_force_offer_push_fail_for_tests, false)) {
    pushed = false;
  } else
#endif
  {
    pushed = (cvector_push_back(list, &ctx) == ccol_success);
  }
  if (pushed) cli->idle_total_count_async++;
  ccol__mutex_unlock(cli->lock);

  if (!pushed) {
    async_ctx_free(ctx);
    client_engine_release();
    async_chain_release(old_chain);
    return true;
  }

#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_force_stray_idle_write_arm_for_tests, false)) {
    async_reg_modify(ctx, cselect_write);
  }
#endif
  async_chain_release(old_chain);
  return true;
}

static void async_retry_hop(chttp_async_ctx *ctx);

// Ends a hop's ctx through its single teardown point. A reused connection that
// died before any response byte is retried once on a fresh connection, when the
// request didn't go out in full or is replayable and no body went out after a
// "100 Continue"; otherwise the failure is reported. A timeout reports
// ccol_timed_out and is never retried.
static void async_ctx_finish(chttp_async_ctx *ctx) {
  if (!ctx->hop_completed) {
    if (ctx->timed_out) {
      async_fulfill(ctx, ccol_timed_out, NULL);
    } else if (ctx->reused && !ctx->any_bytes_read && !ctx->retry_unsafe &&
               (ctx->replayable || ctx->wire_sent < ctx->wire_len)) {
      async_retry_hop(ctx);
    }
  }
  async_ctx_teardown(ctx);
}

// Tears down a ctx that sits in the idle pool, when its IDLE dispatch sees a
// close or an error. It acts only when this call is the one that removes the
// ctx from the pool (cli->lock decides), and then drops the ctx's own engine
// reference.
static void async_idle_ctx_finish(chttp_async_ctx *ctx) {
  struct chttpclient *cli = ctx->cli;
  ccol__mutex_lock(cli->lock);
  bool removed = async_idle_remove_locked(cli, ctx);
  ccol__mutex_unlock(cli->lock);
  if (!removed) return;
  async_ctx_free(ctx);
  client_engine_release();
  ccol__mutex_lock(cli->lock);
  async_idle_count_dec_locked(cli);
  ccol__mutex_unlock(cli->lock);
}

// Ends a connecting ctx whose connect failed, reporting ccol_timed_out when the
// sweep caused the failure and ccol_http_connection_failed otherwise.
static void async_connect_failed(chttp_async_ctx *ctx) {
  async_fulfill(
      ctx, ctx->timed_out ? ccol_timed_out : ccol_http_connection_failed, NULL);
  async_ctx_finish(ctx);
}

// Ends a hop's connection once its response is consumed: Offers the connection
// to the idle pool when reusable and the request didn't ask for close,
// otherwise closes it.
static void async_finish_connection(chttp_async_ctx *ctx, bool reusable) {
  if (reusable && !ctx->request_close && ctx->origin_key &&
      async_idle_pool_offer(ctx))
    return;
  async_ctx_finish(ctx);
}

static void async_retry_hop(chttp_async_ctx *ctx);

static void async_tls_try_write(chttp_async_ctx *ctx);

static void async_process_reading_data(chttp_async_ctx *ctx, const char *data,
                                       size_t data_len);

// Drives the TLS handshake of ctx one step, from a readable or writable
// dispatch, because the reactor knows nothing about TLS. It turns the
// registration to whichever direction the handshake needs next, starts the
// write once the handshake ends, and fails the hop on a handshake error.
static void async_tls_advance(chttp_async_ctx *ctx) {
  ctls_handshake_result r = ctls_conn_handshake_step(ctx->tls);
  if (r == CTLS_HANDSHAKE_DONE) {
    ctx->state = CHTTP_ASYNC_WRITING;
    if (async_reg_modify(ctx, cselect_write) != ccol_success) {
      async_ctx_finish(ctx);
      return;
    }
    async_tls_try_write(ctx);
    return;
  }
  if (r == CTLS_HANDSHAKE_ERROR) {
    long vr = ctls_conn_verify_result(ctx->tls);
    async_fulfill(ctx,
                  ctx->timed_out
                      ? ccol_timed_out
                      : (vr != 0 ? ccol_http_tls_cert_verification_failed
                                 : ccol_http_tls_handshake_failed),
                  NULL);
    async_ctx_finish(ctx);
    return;
  }
  cselect_dir want =
      (r == CTLS_HANDSHAKE_WANT_WRITE) ? cselect_write : cselect_read;
  if (async_reg_modify(ctx, want) != ccol_success) {
    async_ctx_finish(ctx);
  }
}

static void async_plain_try_write(chttp_async_ctx *ctx);

// Tells whether every request byte went out before a response started, so the
// pooling decision never reuses a connection with request bytes left behind.
static inline bool async_request_sent(const chttp_async_ctx *ctx) {
  return ctx->wire_sent >= ctx->wire_len && !ctx->answered_early;
}

// Tells whether the socket or the TLS layer holds input a read would take
// without waiting. After a read that blocked only the socket counts, since the
// TLS layer also reports a partial record as pending input.
static bool async_input_pending(chttp_async_ctx *ctx) {
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

// Stops the room watch of ctx if it has one (see ctx->room_reg), before reg
// turns to writing and before a teardown.
static void async_room_watch_stop(chttp_async_ctx *ctx) {
  ceventloop_reg room = atomic_exchange(&ctx->room_reg, CEVENTLOOP_REG_NULL);
  if (room) ceventloop_remove(cli_engine_bundler.reactor, room);
}

#ifdef RUNNING_UNIT_TESTS
// While armed, the next hop that turns to reading gets no dispatch: Its
// registration is paused right away, as if the dispatch pool could never run
// it, so a test can check that the request timeout still holds.
static _Atomic bool g_stall_next_read_for_tests = false;
static _Atomic ceventloop_reg g_stalled_reg_for_tests = CEVENTLOOP_REG_NULL;

// Arms g_stall_next_read_for_tests, so a test can check that a hop with no
// dispatch still ends at its request timeout.
void chttp__stall_next_read_for_tests(void) {
  atomic_store(&g_stalled_reg_for_tests, CEVENTLOOP_REG_NULL);
  atomic_store(&g_stall_next_read_for_tests, true);
}

// Resumes the registration the stall hook paused, so the test can let the hop
// finish. Gives true when there was one.
bool chttp__stalled_read_resume_for_tests(void) {
  ceventloop_reg reg =
      atomic_exchange(&g_stalled_reg_for_tests, CEVENTLOOP_REG_NULL);
  if (!reg) return false;
  return ceventloop_resume(cli_engine_bundler.reactor, reg) == ccol_success;
}
#endif

// Turns ctx->reg to direction dir. A room watch holds the fd's write direction,
// so turning reg to writing ends the room watch first (one registration per
// direction of an fd).
static ccol_retval async_reg_modify(chttp_async_ctx *ctx, cselect_dir dir) {
  if (dir == cselect_write) async_room_watch_stop(ctx);
#ifdef RUNNING_UNIT_TESTS
  if (dir == cselect_read && ctx->state == CHTTP_ASYNC_READING &&
      atomic_exchange(&g_stall_next_read_for_tests, false)) {
    ccol_retval mrv =
        ceventloop_modify(cli_engine_bundler.reactor, ctx->reg, dir);
    if (mrv == ccol_success &&
        ceventloop_pause(cli_engine_bundler.reactor, ctx->reg) == ccol_success)
      atomic_store(&g_stalled_reg_for_tests, ctx->reg);
    return mrv;
  }
#endif
  return ceventloop_modify(cli_engine_bundler.reactor, ctx->reg, dir);
}

static void async_on_room(ceventloop loop, ceventloop_reg reg, cselectable *sel,
                          void *arg);

// Starts the room watch of a duplex send that a full send buffer paused, so the
// send continues when there is room while a response is read at once. Gives
// false when the registration can't be added. The caller holds the fd's
// dispatch lock or owns the ctx alone.
static bool async_room_watch_start(chttp_async_ctx *ctx) {
  if (atomic_load(&ctx->room_reg)) return true;
  char *err = NULL;
  ceventloop_handlers handlers = {.on_writable = async_on_room};
  ceventloop_reg room = ceventloop_add(cli_engine_bundler.reactor,
                                       cselectable_fd(ctx->fd, cselect_write),
                                       handlers, ctx, &err);
  if (!room) return false;
  atomic_store(&ctx->room_reg, room);
  return true;
}

// Resumes the paused duplex send of ctx when its room watch fires. It runs
// under the fd's dispatch lock, so it never races a dispatch of ctx->reg; an
// error on the fd lands here too, and the write reports it.
static void async_on_room(ceventloop loop, ceventloop_reg reg, cselectable *sel,
                          void *arg) {
  (void)loop;
  (void)sel;
  chttp_async_ctx *ctx = (chttp_async_ctx *)arg;
  atomic_store(&ctx->last_io_ns, monotonic_now_ns());
  ceventloop_reg expected = reg;
  if (!atomic_compare_exchange_strong(&ctx->room_reg, &expected,
                                      CEVENTLOOP_REG_NULL))
    return;
  ceventloop_remove(cli_engine_bundler.reactor, reg);
  if (ctx->hop_completed) {
    async_ctx_handle_if_abandoned(ctx);
    return;
  }
  if (ctx->state == CHTTP_ASYNC_READING && ctx->upload_paused &&
      (ctx->duplex || async_upload_may_resume(ctx)))
    async_upload_resume(ctx);
}

// Feeds the bytes that came with the "100 Continue" to the parser before
// anything a later read takes. It can end the hop and free ctx, so it's the
// caller's last action; mp is read into a local first.
static void async_replay_continue_carry(chttp_async_ctx *ctx) {
  if (!ctx->continue_carry) return;
  ccol_memmgmt_procs *mp = ctx->mp;
  char *carry = ctx->continue_carry;
  size_t carry_len = ctx->continue_carry_len;
  ctx->continue_carry = NULL;
  ctx->continue_carry_len = 0;
  chttp_async_chain *carry_chain = ctx->chain;
  async_chain_retain(carry_chain);
#ifdef RUNNING_UNIT_TESTS
  chttp_test_continue_carry_replay_delay();
#endif
  async_process_reading_data(ctx, carry, carry_len);
#ifdef RUNNING_UNIT_TESTS
  chttp_test_continue_carry_replay_delay();
#endif
  ccol__mem_free(mp, carry);
  async_chain_release(carry_chain);
}

// Stops sending the request, for a while (paused) or for good, and hands the
// ctx to the read path. A duplex send that can't start its room watch falls
// back to the sweep flipping the direction (flip_watch). It can free ctx, so
// the caller does nothing after it.
static void async_upload_pause(chttp_async_ctx *ctx, bool paused) {
  ctx->upload_paused = paused;
  atomic_store(&ctx->flip_watch, CHTTP_FLIP_NONE);
  ctx->state = CHTTP_ASYNC_READING;
  if (async_reg_modify(ctx, cselect_read) != ccol_success) {
    async_ctx_finish(ctx);
    return;
  }
  if (paused && ctx->duplex && !async_room_watch_start(ctx))
    atomic_store(&ctx->flip_watch, CHTTP_FLIP_ON_ROOM);
  async_replay_continue_carry(ctx);
}

// Continues a paused send. It can free ctx, so the caller does nothing after
// it.
static void async_upload_resume(chttp_async_ctx *ctx) {
  async_room_watch_stop(ctx);
  if (ctx->resp_msg_started && ctx->pctx.status_code >= 200 &&
      ctx->pctx.status_code < 300)
    ctx->duplex = true;
  ctx->upload_paused = false;
  ctx->state = CHTTP_ASYNC_WRITING;
  if (ctx->tls) {
    async_tls_try_write(ctx);
  } else {
    async_plain_try_write(ctx);
  }
}

// After this many request bytes since the last check, a successful write looks
// for a response. A server that answers early and closes resets the connection,
// and on Linux that reset drops an answer the client hasn't read.
#define CHTTP_UPLOAD_INPUT_CHECK_BYTES (32u * 1024u)

// True when a write leaves request bytes to send and a response has started to
// arrive, which pauses the send. It polls only every
// CHTTP_UPLOAD_INPUT_CHECK_BYTES.
static bool async_upload_should_pause(chttp_async_ctx *ctx,
                                      bool awaiting_continue,
                                      size_t target_len) {
  if (awaiting_continue || ctx->wire_sent >= target_len ||
      ctx->wire_sent < ctx->upload_check_at)
    return false;
  ctx->upload_check_at = ctx->wire_sent + CHTTP_UPLOAD_INPUT_CHECK_BYTES;
  return async_input_pending(ctx);
}

// True when a paused send may continue: No message started since the last
// discarded one, or the one that did is a 2xx final response with its header
// block in.
static inline bool async_upload_may_resume(const chttp_async_ctx *ctx) {
  return ctx->upload_paused &&
         (!ctx->resp_msg_started ||
          (ctx->pctx.status_code >= 200 && ctx->pctx.status_code < 300));
}

// Writes as much of ctx->wire as ctls_conn_write takes now and keeps track of
// partial progress. With "Expect: 100-continue" it stops at the end of the
// header block and waits for the answer. A response that arrives while sending
// pauses the upload (see async_upload_pause), and EPIPE or ECONNRESET turns the
// hop to reading so the answer isn't lost. It can free ctx.
static void async_tls_try_write(chttp_async_ctx *ctx) {
  if (atomic_load_explicit(&ctx->flip_watch, memory_order_relaxed))
    atomic_store_explicit(&ctx->flip_watch, CHTTP_FLIP_NONE,
                          memory_order_relaxed);
  bool awaiting_continue =
      ctx->want_100_continue && !atomic_load(&ctx->continue_decided);
  size_t target_len = awaiting_continue ? ctx->header_len : ctx->wire_len;
  while (ctx->wire_sent < target_len) {
    ssize_t n;
#ifdef RUNNING_UNIT_TESTS
    if (tls_dir_inject_blocks(true)) {
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
      if (async_upload_should_pause(ctx, awaiting_continue, target_len)) {
        async_upload_pause(ctx, true);
        return;
      }
      continue;
    }
    if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
      if (!awaiting_continue && (ctx->duplex || async_input_pending(ctx))) {
        async_upload_pause(ctx, true);
        return;
      }
      cselect_dir want =
          conn_tls_wants_write(ctx->tls) ? cselect_write : cselect_read;
      if (want == cselect_write && !awaiting_continue)
        atomic_store(&ctx->flip_watch, CHTTP_FLIP_ON_INPUT);
#ifdef RUNNING_UNIT_TESTS
      if (want == cselect_read) {
        atomic_fetch_add(&g_tls_dir_read_interest_while_writing, 1u);
        tls_dir_note_wait(false);
      }
#endif
      if (async_reg_modify(ctx, want) != ccol_success) {
        async_ctx_finish(ctx);
      }
      return;
    }
    if (n == 0 || errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN) {
      async_upload_pause(ctx, false);
      return;
    }
    async_ctx_finish(ctx);
    return;
  }
  if (awaiting_continue) {
    ccol__mutex_lock(ctx->deadline_lock);
    ctx->continue_deadline = deadline_earlier(
        deadline_make(CHTTP_100_CONTINUE_WAIT_US), ctx->overall_deadline);
    chttp_deadline continue_dl = ctx->continue_deadline;
    ccol__mutex_unlock(ctx->deadline_lock);
    client_deadline_note(&continue_dl);
#ifdef RUNNING_UNIT_TESTS
    continue_deadline_stored_for_tests();
#endif
    ctx->state = CHTTP_ASYNC_AWAITING_CONTINUE;
    if (async_reg_modify(ctx, cselect_read) != ccol_success) {
      async_ctx_finish(ctx);
    }
    return;
  }
  if (!ctx->reused) {
    ccol__mem_free(ctx->mp, ctx->wire);
    ctx->wire = NULL;
    ctx->wire_len = ctx->wire_sent = 0;
  }
  ctx->state = CHTTP_ASYNC_READING;
  if (async_reg_modify(ctx, cselect_read) != ccol_success) {
    async_ctx_finish(ctx);
  }
}

// Writes as much of ctx->wire as send() takes now, the plain-HTTP counterpart
// of async_tls_try_write, with the same 100-continue stop, early response pause
// and switch to reading on EPIPE or ECONNRESET. It can free ctx.
static void async_plain_try_write(chttp_async_ctx *ctx) {
  if (atomic_load_explicit(&ctx->flip_watch, memory_order_relaxed))
    atomic_store_explicit(&ctx->flip_watch, CHTTP_FLIP_NONE,
                          memory_order_relaxed);
  bool awaiting_continue =
      ctx->want_100_continue && !atomic_load(&ctx->continue_decided);
  size_t target_len = awaiting_continue ? ctx->header_len : ctx->wire_len;
  while (ctx->wire_sent < target_len) {
    ssize_t n = send(ctx->fd, ctx->wire + ctx->wire_sent,
                     target_len - ctx->wire_sent, CCOL_MSG_NOSIGNAL);
    if (n > 0) {
      ctx->wire_sent += (size_t)n;
      if (async_upload_should_pause(ctx, awaiting_continue, target_len)) {
        async_upload_pause(ctx, true);
        return;
      }
      continue;
    }
    if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
      if (!awaiting_continue && (ctx->duplex || async_input_pending(ctx))) {
        async_upload_pause(ctx, true);
        return;
      }
      if (!awaiting_continue)
        atomic_store(&ctx->flip_watch, CHTTP_FLIP_ON_INPUT);
      if (async_reg_modify(ctx, cselect_write) != ccol_success) {
        async_ctx_finish(ctx);
      }
      return;
    }
    if (n < 0 && (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN)) {
      async_upload_pause(ctx, false);
      return;
    }
    async_ctx_finish(ctx);
    return;
  }
  if (awaiting_continue) {
    ccol__mutex_lock(ctx->deadline_lock);
    ctx->continue_deadline = deadline_earlier(
        deadline_make(CHTTP_100_CONTINUE_WAIT_US), ctx->overall_deadline);
    chttp_deadline continue_dl = ctx->continue_deadline;
    ccol__mutex_unlock(ctx->deadline_lock);
    client_deadline_note(&continue_dl);
#ifdef RUNNING_UNIT_TESTS
    continue_deadline_stored_for_tests();
#endif
    ctx->state = CHTTP_ASYNC_AWAITING_CONTINUE;
    if (async_reg_modify(ctx, cselect_read) != ccol_success) {
      async_ctx_finish(ctx);
    }
    return;
  }
  if (!ctx->reused) {
    ccol__mem_free(ctx->mp, ctx->wire);
    ctx->wire = NULL;
    ctx->wire_len = ctx->wire_sent = 0;
  }
  ctx->state = CHTTP_ASYNC_READING;
  if (async_reg_modify(ctx, cselect_read) != ccol_success) {
    async_ctx_finish(ctx);
    return;
  }
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_force_stray_write_arm_for_tests, false)) {
    async_reg_modify(ctx, cselect_write);
  }
#endif
}

static void async_handle_redirect(chttp_async_ctx *ctx, bool reusable);

// What async_dispatch_kind found: An active hop, an idle pooled connection, or
// a hop already completed.
typedef enum {
  CHTTP_ASYNC_DISPATCH_ACTIVE,
  CHTTP_ASYNC_DISPATCH_IDLE,
  CHTTP_ASYNC_DISPATCH_ABANDONED,
} chttp_async_dispatch_kind;

// Classifies a dispatch of ctx, for every dispatch callback: It reads
// ctx->state and checks hop_completed again, both under idle_lock, because an
// unlocked read can see a take half done. ABANDONED covers a hop that completed
// between the caller's unlocked check and this one.
static chttp_async_dispatch_kind async_dispatch_kind(chttp_async_ctx *ctx) {
  ccol__mutex_lock(ctx->idle_lock);
  bool completed = ctx->hop_completed;
  bool idle = (ctx->state == CHTTP_ASYNC_IDLE);
  ccol__mutex_unlock(ctx->idle_lock);
  if (completed) return CHTTP_ASYNC_DISPATCH_ABANDONED;
  return idle ? CHTTP_ASYNC_DISPATCH_IDLE : CHTTP_ASYNC_DISPATCH_ACTIVE;
}

// Handles a dispatch that finds hop_completed already true: It runs a teardown
// that an application-thread path left for the dispatch (pending_app_teardown),
// and does nothing for a stray dispatch.
static void async_ctx_handle_if_abandoned(chttp_async_ctx *ctx) {
  if (ctx->pending_app_teardown) async_ctx_teardown(ctx);
}

// Publishes reg into ctx->reg while it's still CEVENTLOOP_REG_NULL, with a
// compare-and-exchange, because a dispatch can run before async_connect_task
// has stored what ceventloop_add returned. Both are the same handle.
static inline void async_adopt_reg(chttp_async_ctx *ctx, ceventloop_reg reg) {
  ceventloop_reg expected = CEVENTLOOP_REG_NULL;
  atomic_compare_exchange_strong(&ctx->reg, &expected, reg);
}

static void async_on_readable_impl(ceventloop loop, ceventloop_reg reg,
                                   cselectable *sel, chttp_async_ctx *ctx);

// Tells, once per hop on a connection from the idle pool, whether it's really
// empty before the first request byte goes out: A MSG_PEEK (or a TLS read) must
// find no input and no EOF, and the IDLE dispatch must not have taken input
// (idle_tainted). A take can beat the dispatch that would evict it, so the hop
// checks for itself. false sends the hop to the retry path.
static bool async_reused_conn_is_clean(chttp_async_ctx *ctx) {
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

// Starts or continues the send of a ctx in CHTTP_ASYNC_WRITING (TLS or plain).
// It can free ctx, so it's the caller's last action.
static void async_drive_write(chttp_async_ctx *ctx) {
  if (ctx->reused && !ctx->reuse_checked && ctx->wire_sent == 0 &&
      !async_reused_conn_is_clean(ctx)) {
    async_ctx_finish(ctx);
    return;
  }
  if (ctx->tls) {
    async_tls_try_write(ctx);
  } else {
    async_plain_try_write(ctx);
  }
}

// Finishes a write-direction dispatch for a ctx with nothing of ours to flush
// there: It turns the registration back to reading (or leaves an idle ctx
// alone), reading the state again under idle_lock so a take that moved the ctx
// to WRITING is never driven half-built.
static void async_writable_rest(chttp_async_ctx *ctx) {
  ccol__mutex_lock(ctx->idle_lock);
  chttp_async_state resting_state = ctx->state;
  bool restore = (resting_state == CHTTP_ASYNC_READING ||
                  resting_state == CHTTP_ASYNC_IDLE);
  ccol_retval mrv = restore ? ceventloop_modify(cli_engine_bundler.reactor,
                                                ctx->reg, cselect_read)
                            : ccol_success;
  ccol__mutex_unlock(ctx->idle_lock);
#ifdef RUNNING_UNIT_TESTS
  if (restore)
    atomic_fetch_add_explicit(&g_stray_write_arm_restores_for_tests, 1,
                              memory_order_relaxed);
#endif
  if (mrv != ccol_success && resting_state == CHTTP_ASYNC_READING)
    async_ctx_finish(ctx);
}

// Handles a write-direction dispatch: The connect result, the TLS handshake,
// the request send, a stray flip from the sweep, and a TLS read that asked for
// the write direction. It returns at once for a completed hop and adopts reg
// first, since this can be the very first dispatch.
static void async_on_writable_impl(ceventloop loop, ceventloop_reg reg,
                                   cselectable *sel, chttp_async_ctx *ctx) {
  (void)loop;
  (void)sel;
  atomic_store(&ctx->last_io_ns, monotonic_now_ns());
  if (ctx->hop_completed) {
    async_ctx_handle_if_abandoned(ctx);
    return;
  }
  async_adopt_reg(ctx, reg);

  chttp_async_dispatch_kind kind = async_dispatch_kind(ctx);
  if (kind == CHTTP_ASYNC_DISPATCH_ABANDONED) {
    async_ctx_handle_if_abandoned(ctx);
    return;
  }
#ifdef RUNNING_UNIT_TESTS
  if (kind == CHTTP_ASYNC_DISPATCH_IDLE) {
    int stray_stage = 2;
    if (atomic_compare_exchange_strong(&g_stray_before_take_stage_for_tests,
                                       &stray_stage, 3))
      stray_before_take_wait_for_tests(4);
  }
#endif
  if (kind == CHTTP_ASYNC_DISPATCH_IDLE) {
    async_writable_rest(ctx);
    return;
  }

  if (ctx->state == CHTTP_ASYNC_CONNECTING) {
    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    if (getsockopt(ctx->fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0 ||
        soerr != 0) {
      async_connect_failed(ctx);
      return;
    }
    ccol__mem_free(ctx->mp, ctx->addrs);
    ctx->addrs = NULL;
    if (!ctx->is_unix) apply_tcp_nodelay(ctx->fd);

    if (ctx->is_https) {
#ifdef RUNNING_UNIT_TESTS
      if (ctx->tls_generation != ctx->chain->tls_generation) {
        ccol__fatal_err(
            "chttpclient: a fresh TLS connection carries a tls_generation "
            "other than its own chain's");
      }
#endif
      ccol_retval crv;
      ctx->tls = tls_client_create(ctx->chain->tls_ctx, ctx->fd, ctx->host,
                                   ctx->verify_host, &crv);
      if (!ctx->tls) {
        async_fulfill(ctx, crv, NULL);
        async_ctx_finish(ctx);
        return;
      }
      ctx->state = CHTTP_ASYNC_TLS_HANDSHAKING;
      async_tls_advance(ctx);
      return;
    }

    ctx->state = CHTTP_ASYNC_WRITING;
  }

  if (ctx->state == CHTTP_ASYNC_TLS_HANDSHAKING) {
    async_tls_advance(ctx);
    return;
  }

  if (ctx->state == CHTTP_ASYNC_WRITING) {
    async_drive_write(ctx);
    return;
  }
  if (ctx->state == CHTTP_ASYNC_AWAITING_CONTINUE) {
    if (atomic_load(&ctx->continue_msg_started)) {
      if (async_reg_modify(ctx, cselect_read) != ccol_success)
        async_ctx_finish(ctx);
      return;
    }
    {
      ccol__mutex_lock(ctx->deadline_lock);
      chttp_deadline own_dl = ctx->continue_deadline;
      ccol__mutex_unlock(ctx->deadline_lock);
      int own_ms;
      if (deadline_remaining_ms(&own_dl, &own_ms)) {
        if (async_reg_modify(ctx, cselect_read) != ccol_success)
          async_ctx_finish(ctx);
        return;
      }
    }
    if (atomic_exchange(&ctx->continue_decided, true)) return;
    ctx->interim_responses_seen = 0;
    ctx->state = CHTTP_ASYNC_WRITING;
    if (ctx->tls) {
      async_tls_try_write(ctx);
    } else {
      async_plain_try_write(ctx);
    }
    return;
  }
  if (ctx->state == CHTTP_ASYNC_READING && ctx->upload_paused &&
      (ctx->duplex || async_upload_may_resume(ctx))) {
    async_upload_resume(ctx);
    return;
  }
  if (ctx->state == CHTTP_ASYNC_READING && ctx->tls) {
    if (async_reg_modify(ctx, cselect_read) != ccol_success) {
      async_ctx_finish(ctx);
      return;
    }
    async_on_readable_impl(loop, reg, sel, ctx);
    return;
  }

  async_writable_rest(ctx);
}

// Thin ceventloop callbacks that cast arg back to the ctx and call the _impl
// function. They take no reference: The reactor's dispatch lock and the single
// teardown point keep the ctx alive during a dispatch.
static void async_on_writable(ceventloop loop, ceventloop_reg reg,
                              cselectable *sel, void *arg) {
  async_on_writable_impl(loop, reg, sel, (chttp_async_ctx *)arg);
}

// Parses response bytes in the READING state, from a socket read or from the
// replay of the 100-continue carry. It skips interim responses, refuses a
// response that broke a cap, and on a complete message offers or closes the
// connection and then follows a redirect or fulfils the future. It can free
// ctx.
static void async_process_reading_data(chttp_async_ctx *ctx, const char *data,
                                       size_t data_len) {
  for (;;) {
    if (data_len > 0) {
      ctx->resp_msg_started = true;
      if (ctx->upload_paused) ctx->answered_early = true;
    }
    chttp1_errno err = chttp1_parser_execute(&ctx->parser, data, data_len);
    if (err == CHTTP1_PAUSED) {
      size_t consumed = chttp1_parser_consumed(&ctx->parser);
      bool trailing_this_msg = consumed < data_len;

      if (ctx->pctx.status_code >= 100 && ctx->pctx.status_code < 200) {
        if (++ctx->interim_responses_seen > CHTTP_MAX_INTERIM_RESPONSES) {
          ctx->hop_completed = true;
          async_fulfill(ctx, ccol_http_transfer_aborted, NULL);
          async_ctx_finish(ctx);
          return;
        }
        ccol_retval rrv = parse_ctx_reset_for_continue(&ctx->pctx);
        if (rrv != ccol_success) {
          ctx->hop_completed = true;
          async_fulfill(ctx, ccol_not_enough_memory, NULL);
          async_ctx_finish(ctx);
          return;
        }
        async_parser_start(ctx);
        ctx->resp_msg_started = false;
        ctx->answered_early = false;
        if (!trailing_this_msg) {
          if (ctx->upload_paused) async_upload_resume(ctx);
          return;
        }
        data += consumed;
        data_len -= consumed;
        continue;
      }

      ctx->hop_completed = true;
      if (trailing_this_msg) ctx->pctx.trailing_garbage = true;
      bool keep_alive = chttp1_should_keep_alive(&ctx->parser) &&
                        !ctx->pctx.trailing_garbage && async_request_sent(ctx);

      if (ctx->pctx.will_redirect) {
        async_handle_redirect(ctx, keep_alive);
      } else {
        chttp_async_chain *chain = ctx->chain;
        async_chain_retain(chain);
        chttpcli_response *resp = async_build_response(ctx);
        async_finish_connection(ctx, keep_alive);
        async_fulfill_chain(chain, resp ? ccol_success : ccol_not_enough_memory,
                            resp);
        async_chain_release(chain);
      }
      return;
    }
    if (err == CHTTP1_USER) {
      ctx->hop_completed = true;
      async_fulfill(ctx,
                    ctx->pctx.too_large
                        ? ccol_msg_too_large
                        : (ctx->pctx.error ? ccol_not_enough_memory
                                           : ccol_http_transfer_aborted),
                    NULL);
      async_ctx_finish(ctx);
      return;
    }
    if (err != CHTTP1_OK) {
      ctx->hop_completed = true;
      async_fulfill(ctx, ccol_http_transfer_aborted, NULL);
      async_ctx_finish(ctx);
      return;
    }
    if (ctx->upload_paused && ctx->pctx.status_code >= 200) {
      if (ctx->pctx.status_code < 300) {
        async_upload_resume(ctx);
        return;
      }
      ctx->upload_paused = false;
    }
    return;
  }
}

// Handles bytes that arrive while the body waits for a "100 Continue", the
// event-driven form of Tier 1's chttp_send_and_read wait. A "100 Continue"
// sends the body (after claiming continue_decided against the timeout path);
// any other message is the response, and the body is never sent. It can free
// ctx.
static void async_awaiting_continue_on_data(chttp_async_ctx *ctx,
                                            const char *data, size_t data_len) {
  for (;;) {
    if (data_len > 0) atomic_store(&ctx->continue_msg_started, true);
    chttp1_errno err = chttp1_parser_execute(&ctx->parser, data, data_len);
    if (err == CHTTP1_PAUSED) {
      size_t consumed = chttp1_parser_consumed(&ctx->parser);
      bool trailing_this_msg = consumed < data_len;

      if (ctx->pctx.status_code == 100) {
        if (atomic_exchange(&ctx->continue_decided, true)) return;

        ccol_retval rrv = parse_ctx_reset_for_continue(&ctx->pctx);
        if (rrv != ccol_success) {
          ctx->hop_completed = true;
          async_fulfill(ctx, ccol_not_enough_memory, NULL);
          async_ctx_finish(ctx);
          return;
        }
        async_parser_start(ctx);
        ctx->retry_unsafe = true;
        ctx->interim_responses_seen = 0;

        if (trailing_this_msg) {
          ctx->continue_carry =
              (char *)ccol__mem_alloc(ctx->mp, data_len - consumed);
          if (!ctx->continue_carry) {
            ctx->hop_completed = true;
            async_fulfill(ctx, ccol_not_enough_memory, NULL);
            async_ctx_finish(ctx);
            return;
          }
          memcpy(ctx->continue_carry, data + consumed, data_len - consumed);
          ctx->continue_carry_len = data_len - consumed;
          async_upload_pause(ctx, true);
          return;
        }

        ctx->state = CHTTP_ASYNC_WRITING;
        if (ctx->tls) {
          async_tls_try_write(ctx);
        } else {
          async_plain_try_write(ctx);
        }
        return;
      }

      if (ctx->pctx.status_code >= 100 && ctx->pctx.status_code < 200) {
        if (++ctx->interim_responses_seen > CHTTP_MAX_INTERIM_RESPONSES) {
          if (atomic_exchange(&ctx->continue_decided, true)) return;
          ctx->hop_completed = true;
          async_fulfill(ctx, ccol_http_transfer_aborted, NULL);
          async_ctx_finish(ctx);
          return;
        }
        ccol_retval rrv = parse_ctx_reset_for_continue(&ctx->pctx);
        if (rrv != ccol_success) {
          if (atomic_exchange(&ctx->continue_decided, true)) return;
          ctx->hop_completed = true;
          async_fulfill(ctx, ccol_not_enough_memory, NULL);
          async_ctx_finish(ctx);
          return;
        }
        async_parser_start(ctx);
        if (!trailing_this_msg) {
          atomic_store(&ctx->continue_msg_started, false);
          return;
        }
        data += consumed;
        data_len -= consumed;
        continue;
      }

      if (atomic_exchange(&ctx->continue_decided, true)) return;
      ctx->hop_completed = true;
      if (trailing_this_msg) ctx->pctx.trailing_garbage = true;
      if (ctx->pctx.will_redirect) {
        async_handle_redirect(ctx, false);
      } else {
        chttp_async_chain *chain = ctx->chain;
        async_chain_retain(chain);
        chttpcli_response *resp = async_build_response(ctx);
        async_finish_connection(ctx, false);
        async_fulfill_chain(chain, resp ? ccol_success : ccol_not_enough_memory,
                            resp);
        async_chain_release(chain);
      }
      return;
    }
    if (err == CHTTP1_USER) {
      if (atomic_exchange(&ctx->continue_decided, true)) return;
      ctx->hop_completed = true;
      async_fulfill(ctx,
                    ctx->pctx.too_large
                        ? ccol_msg_too_large
                        : (ctx->pctx.error ? ccol_not_enough_memory
                                           : ccol_http_transfer_aborted),
                    NULL);
      async_ctx_finish(ctx);
      return;
    }
    if (err != CHTTP1_OK) {
      if (atomic_exchange(&ctx->continue_decided, true)) return;
      ctx->hop_completed = true;
      async_fulfill(ctx, ccol_http_transfer_aborted, NULL);
      async_ctx_finish(ctx);
      return;
    }
    return;
  }
}

// Handles a read-direction dispatch: An idle pooled connection (evicted on any
// input or EOF), the TLS handshake, the 100-continue wait and the response
// read, with the TLS layer drained before a wait. It reaps a teardown left for
// the dispatch, and checks the request deadline before each read.
static void async_on_readable_impl(ceventloop loop, ceventloop_reg reg,
                                   cselectable *sel, chttp_async_ctx *ctx) {
  (void)loop;
  (void)sel;
  atomic_store(&ctx->last_io_ns, monotonic_now_ns());

  if (ctx->hop_completed) {
    async_ctx_handle_if_abandoned(ctx);
    return;
  }
  async_adopt_reg(ctx, reg);

  chttp_async_dispatch_kind kind = async_dispatch_kind(ctx);
  if (kind == CHTTP_ASYNC_DISPATCH_ABANDONED) {
    async_ctx_handle_if_abandoned(ctx);
    return;
  }
  if (kind == CHTTP_ASYNC_DISPATCH_IDLE) {
#ifdef RUNNING_UNIT_TESTS
    chttp_test_idle_probe_hold(ctx->fd);
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
      return;
    }
    ctx->idle_tainted = true;
    async_idle_ctx_finish(ctx);
    return;
  }

  if (ctx->state == CHTTP_ASYNC_TLS_HANDSHAKING) {
    async_tls_advance(ctx);
    return;
  }

  if (ctx->state == CHTTP_ASYNC_WRITING) {
    async_drive_write(ctx);
    return;
  }

  if (ctx->timed_out ||
      read_deadline_passed(&ctx->chain->overall_deadline, ctx->fd)) {
    if (ctx->state == CHTTP_ASYNC_AWAITING_CONTINUE &&
        atomic_exchange(&ctx->continue_decided, true))
      return;
    ctx->timed_out = true;
    ctx->hop_completed = true;
    async_fulfill(ctx, ccol_timed_out, NULL);
    async_ctx_finish(ctx);
    return;
  }

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
  if (ctx->any_bytes_read &&
      atomic_exchange(&g_force_async_hard_read_error_for_tests, false)) {
    hard_error = true;
    injected = true;
  }
#endif

  if (!injected) {
    if (ctx->tls) {
#ifdef RUNNING_UNIT_TESTS
      if (tls_dir_inject_blocks(false)) {
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
      ctx->tls_read_blocked =
          n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN);
      if (n < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
          if (ctx->state == CHTTP_ASYNC_READING &&
              async_upload_may_resume(ctx)) {
            async_upload_resume(ctx);
            return;
          }
          if (ctx->state == CHTTP_ASYNC_READING &&
              conn_tls_wants_write(ctx->tls)) {
#ifdef RUNNING_UNIT_TESTS
            atomic_fetch_add(&g_tls_dir_write_interest_while_reading, 1u);
            tls_dir_note_wait(true);
#endif
            if (async_reg_modify(ctx, cselect_write) != ccol_success) {
              async_ctx_finish(ctx);
            }
          }
          return;
        }
        hard_error = true;
      } else {
        eof = (n == 0);
      }
      if (n > 0) {
        size_t total = (size_t)n;
        size_t cap = sizeof(buf);
        while (ctls_conn_has_pending_input(ctx->tls)) {
          if (total == cap) {
            size_t ncap = cap * 2;
            char *nb =
                (ncap > cap) ? (char *)ccol__mem_alloc(ctx->mp, ncap) : NULL;
            if (!nb) {
              drain_oom = true;
              break;
            }
            memcpy(nb, data, total);
            ccol__mem_free(ctx->mp, heap_data);
            heap_data = nb;
            data = nb;
            cap = ncap;
          }
          if (read_deadline_passed(&ctx->chain->overall_deadline, ctx->fd)) {
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
          if (ctx->state == CHTTP_ASYNC_READING && async_upload_may_resume(ctx))
            async_upload_resume(ctx);
          return;
        }
        hard_error = true;
      } else {
        eof = (n == 0);
      }
    }
  }
  if (n > 0) ctx->any_bytes_read = true;

  if (hard_error) {
    async_ctx_finish(ctx);
    return;
  }

  if (eof) {
    chttp1_errno fe = chttp1_parser_finish(&ctx->parser);
    bool ok = ((fe == CHTTP1_OK || fe == CHTTP1_PAUSED) &&
               ctx->pctx.message_complete);
    if (!ok) {
      async_ctx_finish(ctx);
      return;
    }
    if (ctx->state == CHTTP_ASYNC_AWAITING_CONTINUE) {
      if (atomic_exchange(&ctx->continue_decided, true)) return;
    }
    ctx->hop_completed = true;
    if (ctx->pctx.will_redirect) {
      async_handle_redirect(ctx, false);
    } else {
      async_fulfill_success(ctx);
      async_ctx_finish(ctx);
    }
    return;
  }

  if (drain_timed_out) {
    ccol__mem_free(ctx->mp, heap_data);
    if (ctx->state == CHTTP_ASYNC_AWAITING_CONTINUE &&
        atomic_exchange(&ctx->continue_decided, true))
      return;
    ctx->timed_out = true;
    ctx->hop_completed = true;
    async_fulfill(ctx, ccol_timed_out, NULL);
    async_ctx_finish(ctx);
    return;
  }

  if (drain_oom) {
    ccol__mem_free(ctx->mp, heap_data);
    if (ctx->state == CHTTP_ASYNC_AWAITING_CONTINUE &&
        atomic_exchange(&ctx->continue_decided, true))
      return;
    ctx->hop_completed = true;
    async_fulfill(ctx, ccol_not_enough_memory, NULL);
    async_ctx_finish(ctx);
    return;
  }

  ccol_memmgmt_procs data_procs;
  ccol_memmgmt_procs *data_mp = NULL;
  if (heap_data && ctx->mp) {
    data_procs = *ctx->mp;
    data_mp = &data_procs;
  }
  if (ctx->state == CHTTP_ASYNC_AWAITING_CONTINUE) {
    async_awaiting_continue_on_data(ctx, data, (size_t)n);
  } else {
    async_process_reading_data(ctx, data, (size_t)n);
  }
  ccol__mem_free(data_mp, heap_data);
}

// The read callback of a hop's registration; see async_on_writable.
static void async_on_readable(ceventloop loop, ceventloop_reg reg,
                              cselectable *sel, void *arg) {
  async_on_readable_impl(loop, reg, sel, (chttp_async_ctx *)arg);
}

// Handles an error or hang-up dispatch: Evicts an idle connection, ends a
// failed connect, and otherwise ends the hop with a transfer or connect error
// (a timeout when the sweep marked it), retrying a reused connection once.
static void async_on_error_impl(ceventloop loop, ceventloop_reg reg,
                                cselectable *sel, chttp_async_ctx *ctx) {
  (void)loop;
  (void)sel;
  atomic_store(&ctx->last_io_ns, monotonic_now_ns());
  if (ctx->hop_completed) {
    async_ctx_handle_if_abandoned(ctx);
    return;
  }
  async_adopt_reg(ctx, reg);
  chttp_async_dispatch_kind kind = async_dispatch_kind(ctx);
  if (kind == CHTTP_ASYNC_DISPATCH_ABANDONED) {
    async_ctx_handle_if_abandoned(ctx);
    return;
  }
  if (kind == CHTTP_ASYNC_DISPATCH_IDLE) {
    async_idle_ctx_finish(ctx);
    return;
  }
  if (ctx->state == CHTTP_ASYNC_CONNECTING) {
    async_connect_failed(ctx);
    return;
  }
  if (ctx->state == CHTTP_ASYNC_TLS_HANDSHAKING) {
    long vr = ctx->tls ? ctls_conn_verify_result(ctx->tls) : 0;
    async_fulfill(ctx,
                  ctx->timed_out
                      ? ccol_timed_out
                      : (vr != 0 ? ccol_http_tls_cert_verification_failed
                                 : ccol_http_tls_handshake_failed),
                  NULL);
    async_ctx_finish(ctx);
    return;
  }
  async_ctx_finish(ctx);
}

// The error callback of a hop's registration; see async_on_writable.
static void async_on_error(ceventloop loop, ceventloop_reg reg,
                           cselectable *sel, void *arg) {
  async_on_error_impl(loop, reg, sel, (chttp_async_ctx *)arg);
}

// Async Happy Eyeballs
//
// A TCP target with several addresses connects through a race of attempts, each
// a socket with its own registration whose arg is the attempt. The first to
// connect hands its fd to the ctx, which registers it; the losers are closed.
// The race is refcounted (the ctx and each registration), and lock guards it.
typedef struct chttp_connect_race_s chttp_connect_race;

// One attempt of a race.
typedef struct {
  chttp_connect_race *race;
  int fd;
  ceventloop_reg reg;
} chttp_connect_attempt;

// The race of one hop.
struct chttp_connect_race_s {
  ccol__mutex lock;
  ccol_memmgmt_procs procs;
  bool has_procs;
  int refs;
  chttp_async_ctx *ctx;
  bool decided;
  size_t pending;
  size_t next;
  struct timespec next_start;
  chttp_addr_list *addrs;
  chttp_connect_attempt attempts[];
};

static void async_connect_register(chttp_async_ctx *ctx, int fd);

// Drops one race reference, and frees the race with the last one, so neither
// the ctx nor a late attempt dispatch frees it early.
static void async_race_release(chttp_connect_race *r) {
  ccol__mutex_lock(r->lock);
  bool last = (--r->refs == 0);
  ccol__mutex_unlock(r->lock);
  if (!last) return;
  ccol_memmgmt_procs procs = r->procs;
  ccol_memmgmt_procs *mp = r->has_procs ? &procs : NULL;
  ccol__mutex_destroy(r->lock);
  ccol__mem_free(mp, r->addrs);
  ccol__mem_free(mp, r);
}

// Drops the race reference of an attempt's registration once the reactor has
// removed it (its on_removed callback).
static void async_race_on_removed(void *arg) {
  async_race_release(((chttp_connect_attempt *)arg)->race);
}

// Closes one attempt, removing the registration before the close so the fd
// number can't reach another socket while the reactor watches it. The caller
// holds r->lock.
static void async_race_close_attempt_locked(chttp_connect_race *r,
                                            chttp_connect_attempt *a) {
  if (a->reg) {
    ceventloop_remove(cli_engine_bundler.reactor, a->reg);
    a->reg = CEVENTLOOP_REG_NULL;
  }
  if (a->fd >= 0) {
    close(a->fd);
    a->fd = -1;
    r->pending--;
  }
}

static void async_race_on_event(ceventloop loop, ceventloop_reg reg,
                                cselectable *sel, void *arg);

// Starts the next candidate that accepts a connect and registers it; one that
// fails on the spot makes way for the next. The caller holds r->lock.
static void async_race_launch_locked(chttp_connect_race *r) {
  while (r->next < r->addrs->count) {
    size_t idx = r->next++;
    bool in_progress = false;
    int fd = addr_connect_start(&r->addrs->addrs[idx], &in_progress);
    if (fd < 0) continue;
    chttp_connect_attempt *a = &r->attempts[idx];
    ceventloop_handlers handlers = {
        .on_writable = async_race_on_event,
        .on_error = async_race_on_event,
        .on_removed = async_race_on_removed,
    };
    char *err = NULL;
    ceventloop_reg reg =
        ceventloop_add(cli_engine_bundler.reactor,
                       cselectable_fd(fd, cselect_write), handlers, a, &err);
    if (!reg) {
      close(fd);
      continue;
    }
    a->fd = fd;
    a->reg = reg;
    r->refs++;
    r->pending++;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    r->next_start = timespec_add_ms(now, connect_attempt_delay_ms());
    if (r->next < r->addrs->count)
      client_deadline_note_ns(timespec_to_ns(&r->next_start));
    return;
  }
}

// Decides the race for winner (NULL for nobody) and closes the other attempts,
// so exactly one fd reaches the ctx. Gives the ctx to act on; the caller holds
// r->lock, releases it, and then acts.
static chttp_async_ctx *async_race_decide_locked(
    chttp_connect_race *r, chttp_connect_attempt *winner) {
  for (size_t i = 0; i < r->addrs->count; i++) {
    chttp_connect_attempt *a = &r->attempts[i];
    if (a != winner) async_race_close_attempt_locked(r, a);
  }
  r->decided = true;
  chttp_async_ctx *ctx = r->ctx;
  r->ctx = NULL;
  return ctx;
}

// An attempt's dispatch: A connected attempt wins and hands its fd to the ctx;
// a failed one makes way for the next candidate, and the last failure ends the
// hop.
static void async_race_on_event(ceventloop loop, ceventloop_reg reg,
                                cselectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  (void)sel;
  chttp_connect_attempt *a = (chttp_connect_attempt *)arg;
  chttp_connect_race *r = a->race;
  ccol__mutex_lock(r->lock);
  if (r->decided || a->fd < 0) {
    ccol__mutex_unlock(r->lock);
    return;
  }
  int soerr = 0;
  socklen_t slen = sizeof(soerr);
  if (getsockopt(a->fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) == 0 &&
      soerr == 0) {
    int fd = a->fd;
    ceventloop_remove(cli_engine_bundler.reactor, a->reg);
    a->reg = CEVENTLOOP_REG_NULL;
    a->fd = -1;
    r->pending--;
    chttp_async_ctx *ctx = async_race_decide_locked(r, a);
    ccol__mutex_unlock(r->lock);
    async_connect_register(ctx, fd);
    return;
  }
  async_race_close_attempt_locked(r, a);
  chttp_async_ctx *ctx = r->ctx;
  if (!ctx->timed_out) async_race_launch_locked(r);
  if (r->pending > 0) {
    ccol__mutex_unlock(r->lock);
    return;
  }
  ctx = async_race_decide_locked(r, NULL);
  ccol__mutex_unlock(r->lock);
  async_fulfill(
      ctx, ctx->timed_out ? ccol_timed_out : ccol_http_connection_failed, NULL);
  async_ctx_teardown(ctx);
}

// Called by the sweep, under the ctx's stripe lock, for a ctx with a running
// race. An expired connect deadline shuts every attempt down (they fail through
// their own dispatch); otherwise the next candidate starts once its delay is
// over. Gives the next instant the race needs the sweep.
static uint64_t async_race_tick(chttp_connect_race *r, bool expired) {
  uint64_t next_ns = UINT64_MAX;
  ccol__mutex_lock(r->lock);
  if (!r->decided) {
    if (expired || (r->ctx && r->ctx->timed_out)) {
      for (size_t i = 0; i < r->addrs->count; i++)
        if (r->attempts[i].fd >= 0) shutdown(r->attempts[i].fd, SHUT_RDWR);
    } else if (r->next < r->addrs->count) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (ms_until(&r->next_start, &now) == 0) {
#ifdef RUNNING_UNIT_TESTS
        sweep_note_due_for_tests(1, timespec_to_ns(&r->next_start));
#endif
        async_race_launch_locked(r);
      }
      if (r->next < r->addrs->count) next_ns = timespec_to_ns(&r->next_start);
    }
  }
  ccol__mutex_unlock(r->lock);
  return next_ns;
}

// Starts the race for ctx over ctx->addrs, which it takes over. Gives false
// when the race can't be built.
static bool async_race_start(chttp_async_ctx *ctx) {
  size_t n = ctx->addrs->count;
  chttp_connect_race *r = (chttp_connect_race *)ccol__mem_calloc(
      ctx->mp, 1, sizeof(*r) + n * sizeof(r->attempts[0]));
  if (!r) return false;
  if (ccol__mutex_init(r->lock) != 0) {
    ccol__mem_free(ctx->mp, r);
    return false;
  }
  if (ctx->mp) {
    r->procs = *ctx->mp;
    r->has_procs = true;
  }
  r->refs = 1;
  r->ctx = ctx;
  r->addrs = ctx->addrs;
  ctx->addrs = NULL;
  for (size_t i = 0; i < n; i++) {
    r->attempts[i].race = r;
    r->attempts[i].fd = -1;
  }
  ctx->state = CHTTP_ASYNC_CONNECTING;
  atomic_store(&ctx->race, r);

  ccol__mutex_lock(r->lock);
  async_race_launch_locked(r);
  bool failed = (r->pending == 0);
  if (failed) (void)async_race_decide_locked(r, NULL);
  ccol__mutex_unlock(r->lock);
  if (failed) {
    async_fulfill(ctx, ccol_http_connection_failed, NULL);
    async_ctx_teardown(ctx);
  }
  return true;
}

// Lets go of ctx's race when ctx is freed, deciding it if it isn't yet, so no
// attempt outlives its ctx.
static void async_race_detach(chttp_async_ctx *ctx) {
  chttp_connect_race *r = atomic_load(&ctx->race);
  if (!r) return;
  atomic_store(&ctx->race, NULL);
  ccol__mutex_lock(r->lock);
  if (!r->decided) (void)async_race_decide_locked(r, NULL);
  ccol__mutex_unlock(r->lock);
  async_race_release(r);
}

// Connects a fresh hop: Resolves a TCP target or builds the unix address, then
// makes one non-blocking connect (or starts the race) and registers the fd. It
// runs on a DNS pool worker for a named target, inline for a literal or a unix
// target. It checks timed_out before and after the lookup, so a hop the sweep
// already answered makes no lookup and no connect.
static void async_connect_task(void *arg) {
  chttp_async_ctx *ctx = (chttp_async_ctx *)arg;
  int fd = -1;

  if (ctx->is_unix) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t path_len = strlen(ctx->unix_socket_path);
    if (path_len >= sizeof(addr.sun_path)) {
      async_fulfill(ctx, ccol_http_invalid_url, NULL);
      async_ctx_teardown(ctx);
      return;
    }
    memcpy(addr.sun_path, ctx->unix_socket_path, path_len + 1);
    fd = ccol_socket_nb(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      async_fulfill(ctx, ccol_http_connection_failed, NULL);
      async_ctx_teardown(ctx);
      return;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 &&
        errno != EINPROGRESS) {
      close(fd);
      async_fulfill(ctx, ccol_http_connection_failed, NULL);
      async_ctx_teardown(ctx);
      return;
    }
  } else {
    if (ctx->timed_out) {
      async_fulfill(ctx, ccol_timed_out, NULL);
      async_ctx_teardown(ctx);
      return;
    }
    ccol_retval rrv = addr_list_resolve_for_target(ctx->mp, ctx->host,
                                                   ctx->port, &ctx->addrs);
    if (rrv != ccol_success) {
      async_fulfill(ctx, rrv, NULL);
      async_ctx_teardown(ctx);
      return;
    }
    if (ctx->timed_out) {
      async_fulfill(ctx, ccol_timed_out, NULL);
      async_ctx_teardown(ctx);
      return;
    }
    addr_list_interleave(ctx->addrs);
    if (ctx->addrs->count > 1) {
      if (!async_race_start(ctx)) {
        async_fulfill(ctx, ccol_not_enough_memory, NULL);
        async_ctx_teardown(ctx);
      }
      return;
    }
    bool in_progress = false;
    fd = addr_connect_start(&ctx->addrs->addrs[0], &in_progress);
    if (fd < 0) {
      async_fulfill(ctx, ccol_http_connection_failed, NULL);
      async_ctx_teardown(ctx);
      return;
    }
    int remaining_ms;
    if (in_progress &&
        !deadline_remaining_ms(&ctx->connect_deadline, &remaining_ms))
      ctx->timed_out = true;
  }
  async_connect_register(ctx, fd);
}

// Starts the connect of a fresh hop: A name lookup goes to the DNS pool, since
// getaddrinfo() blocks, and anything else runs async_connect_task inline.
static ccol_retval async_connect_begin(chttp_async_ctx *ctx) {
  if (ctx->is_unix || host_is_ip_literal(ctx->host)) {
    async_connect_task(ctx);
    return ccol_success;
  }
  return cthrpool_submit(cli_engine_bundler.dns_pool, async_connect_task, ctx,
                         NULL);
}

// Hands the socket fd to ctx and registers it for write readiness. It stores
// the fd before it reads timed_out (both sequentially consistent), so the sweep
// either shuts this fd down or the task ends the hop itself.
static void async_connect_register(chttp_async_ctx *ctx, int fd) {
  ctx->fd = fd;
  ctx->state = CHTTP_ASYNC_CONNECTING;

  if (ctx->timed_out) {
    async_fulfill(ctx, ccol_timed_out, NULL);
    async_ctx_teardown(ctx);
    return;
  }

  char *err = NULL;
  ceventloop_handlers handlers = {
      .on_readable = async_on_readable,
      .on_writable = async_on_writable,
      .on_error = async_on_error,
  };
  ccol__mutex_lock(ctx->idle_lock);
  ctx->reg =
      ceventloop_add(cli_engine_bundler.reactor,
                     cselectable_fd(fd, cselect_write), handlers, ctx, &err);
  bool reg_failed = !ctx->reg;
  ccol__mutex_unlock(ctx->idle_lock);
  if (reg_failed) {
    async_fulfill(ctx, ccol_not_enough_memory, NULL);
    async_ctx_teardown(ctx);
  }
}

// Retries a reused connection that died before any response byte on a fresh ctx
// and connection, as Tier 1 does. The old ctx is torn down by its own dispatch,
// so nothing has to neutralise its callbacks.
static void async_retry_hop(chttp_async_ctx *old_ctx) {
  old_ctx->hop_completed = true;
  chttp_async_chain *chain = old_ctx->chain;

  async_chain_retain(chain);

  chttp_async_ctx *ctx = async_ctx_create(chain->mp);
  if (!ctx) {
    async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
    async_chain_release(chain);
    return;
  }

  ccol__mutex_lock(ctx->deadline_lock);
  ctx->overall_deadline = chain->overall_deadline;
  ctx->inactivity_us = chain->inactivity_timeout_us;
  atomic_store(&ctx->last_io_ns, 0);
  ccol__mutex_unlock(ctx->deadline_lock);
  ctx->chain = chain;
  ctx->cli = old_ctx->cli;
  ctx->hop = old_ctx->hop;
  ctx->cur_method = old_ctx->cur_method;
  ctx->is_https = old_ctx->is_https;
  ctx->verify_host = old_ctx->verify_host;
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
  ctx->want_100_continue = old_ctx->want_100_continue;
  ctx->header_len = old_ctx->header_len;
  ctx->replayable = old_ctx->replayable;
  ctx->request_close = old_ctx->request_close;
  atomic_store(&ctx->continue_decided, false);
  atomic_store(&ctx->continue_msg_started, false);

  ctx->pctx.mp = chain->mp;
  ctx->pctx.is_head_request = old_ctx->pctx.is_head_request;
  ctx->pctx.requested_sink_fn =
      chain->write_fn ? sink_async_user : sink_buffered;
  ctx->pctx.requested_sink_ctx = chain->write_fn ? (void *)chain : &ctx->bb;
  ctx->pctx.headers = old_ctx->pctx.headers;
  old_ctx->pctx.headers = NULL;
  ctx->bb.mp = chain->mp;
  ctx->bb.max_size = chain->max_response_body_size;

  ctx->reused = false;
  ctx->any_bytes_read = false;
  ctx->connect_deadline = deadline_make(chain->connect_timeout_us);

  async_parser_start(ctx);

  client_deadline_register(ctx);
  client_deadline_note(&ctx->connect_deadline);
  client_deadline_note(&chain->overall_deadline);

  ccol_retval sr = async_connect_begin(ctx);
  if (sr != ccol_success) {
    async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
    async_ctx_free(ctx);
    async_chain_release(chain);
  }
}

// Fails a hop that couldn't be submitted, for async_submit_hop's error paths:
// fulfils the chain with rv and tears ctx down.
static void async_submit_hop_fail(chttp_async_ctx *ctx, ccol_retval rv) {
  chttp_async_chain *chain = ctx->chain;
  async_fulfill_chain(chain, rv, NULL);
  if (ctx->reused) {
    ctx->pending_app_teardown = true;
    ctx->hop_completed = true;
#ifdef RUNNING_UNIT_TESTS
    atomic_store(&g_abandon_shutdown_under_lock_for_tests,
                 ctx->idle_lock_held_for_tests ? 1 : 0);
#endif
    if (ctx->fd >= 0) shutdown(ctx->fd, SHUT_RDWR);
#ifdef RUNNING_UNIT_TESTS
    ctx->idle_lock_held_for_tests = false;
#endif
    ccol__mutex_unlock(ctx->idle_lock);
    client_engine_release();
  } else {
    async_ctx_free(ctx);
    async_chain_release(chain);
  }
}

// Prepares one hop of a chain and submits it, for the first hop and every
// redirect hop. It parses the URL, applies the transport and TLS checks, builds
// the hop's view of the request (redirect header drops, carried userinfo
// Authorization, method and body after a downgrade), serializes it, and then
// either takes a pooled connection (writing under its idle_lock) or starts a
// fresh connect. On success the hop holds a chain reference; on failure it
// fulfils the future with the error, keeps the refcount as it was, and gives
// false.
static bool async_submit_hop(chttp_async_chain *chain, const char *url_str,
                             chttp_method method, const void *body_data,
                             size_t body_len, const char *body_content_type,
                             int hop) {
  async_chain_retain(chain);

  chttp_url url;
  ccol_retval prv = parse_chttp_url(chain->mp, url_str, &url);
  if (prv != ccol_success) {
    async_fulfill_chain(chain, prv, NULL);
    async_chain_release(chain);
    return false;
  }
  if (url.is_https && !chain->tls_ctx_usable) {
    url_free(chain->mp, &url);
    async_fulfill_chain(chain, tls_ctx_unusable_code(chain->tls_ctx_oom), NULL);
    async_chain_release(chain);
    return false;
  }

  chain->redirect_drops =
      redirect_header_drops(chain->initial_origin_key, url.origin_key);

  const char *effective_auth = NULL;
  if (url.userinfo_authorization) {
    effective_auth = url.userinfo_authorization;
  } else if (chain->carried_auth &&
             origin_keys_same(chain->carried_auth_origin, url.origin_key)) {
    effective_auth = chain->carried_auth;
  }
  if (url.userinfo_authorization) {
    char *na = ccol_strdup(chain->mp, url.userinfo_authorization);
    char *no = ccol_strdup(chain->mp, url.origin_key);
    if (!na || !no) {
      ccol__mem_free(chain->mp, na);
      ccol__mem_free(chain->mp, no);
      url_free(chain->mp, &url);
      async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
      async_chain_release(chain);
      return false;
    }
    ccol__mem_free(chain->mp, chain->carried_auth);
    ccol__mem_free(chain->mp, chain->carried_auth_origin);
    chain->carried_auth = na;
    chain->carried_auth_origin = no;
  } else if (chain->carried_auth &&
             !origin_keys_same(chain->carried_auth_origin, url.origin_key)) {
    ccol__mem_free(chain->mp, chain->carried_auth);
    ccol__mem_free(chain->mp, chain->carried_auth_origin);
    chain->carried_auth = NULL;
    chain->carried_auth_origin = NULL;
  }

  chttp_async_ctx *ctx = NULL;
  bool reused = async_idle_pool_take(chain->cli, url.origin_key, chain, &ctx);
#ifdef RUNNING_UNIT_TESTS
  if (reused && atomic_exchange(&g_stray_write_at_take_for_tests, false)) {
    (void)ceventloop_modify(cli_engine_bundler.reactor, ctx->reg,
                            cselect_write);
    struct timespec pause_ts = {0, 200000000L};
    nanosleep(&pause_ts, NULL);
  }
#endif
  if (!reused) {
    ctx = async_ctx_create(chain->mp);
    if (!ctx) {
      url_free(chain->mp, &url);
      async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
      async_chain_release(chain);
      return false;
    }
    ctx->cli = chain->cli;
    ctx->connect_deadline = deadline_make(chain->connect_timeout_us);
  }
  ccol__mutex_lock(ctx->deadline_lock);
  ctx->overall_deadline = chain->overall_deadline;
  ctx->inactivity_us = chain->inactivity_timeout_us;
  atomic_store(&ctx->last_io_ns, 0);
  ccol__mutex_unlock(ctx->deadline_lock);
  ctx->chain = chain;
  ctx->hop = hop;
  ctx->cur_method = method;
  ctx->is_https = url.is_https;
  ctx->is_ipv6 = url.is_ipv6;
  ctx->verify_host = chain->verify_host;
  if (!reused) ctx->tls_generation = chain->tls_generation;
  ctx->reused = reused;
  ctx->reuse_checked = false;
  ctx->any_bytes_read = false;
  ctx->hop_completed = false;
  ctx->interim_responses_seen = 0;

  ccol__mem_free(chain->mp, ctx->path_and_query);
  ctx->path_and_query = ccol_strdup(chain->mp, url.path_and_query);
  if (!ctx->path_and_query) {
    url_free(chain->mp, &url);
    async_submit_hop_fail(ctx, ccol_not_enough_memory);
    return false;
  }

  ctx->pctx.mp = chain->mp;
  ctx->pctx.is_head_request = (method == CHTTP_HEAD);
  ctx->pctx.requested_sink_fn =
      chain->write_fn ? sink_async_user : sink_buffered;
  ctx->pctx.requested_sink_ctx = chain->write_fn ? (void *)chain : &ctx->bb;
  ctx->bb.mp = chain->mp;
  ctx->bb.max_size = chain->max_response_body_size;

  char *herr = NULL;
  ctx->pctx.headers =
      chashmap_create_full(CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                           ccol_string, chain->mp, NULL, NULL, &herr);
  if (!ctx->pctx.headers) {
    url_free(chain->mp, &url);
    async_submit_hop_fail(ctx, ccol_not_enough_memory);
    return false;
  }

  chttp_request hop_req;
  memset(&hop_req, 0, sizeof(hop_req));
  hop_req.method = method;
  hop_req.headers = chain->req_headers;
  hop_req.body.data = body_data;
  hop_req.body.len = body_len;
  hop_req.body.content_type = body_content_type;
  hop_req.expect_continue = chain->expect_continue;

  chttp_header_presence hop_hp;
  prv = serialize_request(chain->mp, &hop_req, &url, effective_auth,
                          chain->redirect_drops, &ctx->wire, &ctx->wire_len,
                          &hop_hp);
  if (prv != ccol_success) {
    url_free(chain->mp, &url);
    async_submit_hop_fail(ctx, prv);
    return false;
  }
  bool body_carrying_method_ac =
      (method == CHTTP_POST || method == CHTTP_PUT || method == CHTTP_PATCH);
  ctx->want_100_continue = chain->expect_continue && !hop_hp.has_expect &&
                           body_carrying_method_ac && body_data && body_len > 0;
  ctx->replayable = request_is_replayable(method, &hop_hp);
  ctx->request_close = hop_hp.request_close;
  ctx->header_len = ctx->want_100_continue ? ctx->wire_len - body_len : 0;
  atomic_store(&ctx->continue_decided, false);
  atomic_store(&ctx->continue_msg_started, false);

  if (!ctx->origin_key) {
    ctx->origin_key = ccol_strdup(chain->mp, url.origin_key);
    if (!ctx->origin_key) {
      url_free(chain->mp, &url);
      async_submit_hop_fail(ctx, ccol_not_enough_memory);
      return false;
    }
  }

  if (!reused) {
    ctx->is_unix = url.is_unix;
    if (url.is_unix) {
      ctx->unix_socket_path = ccol_strdup(chain->mp, url.unix_socket_path);
      if (!ctx->unix_socket_path) {
        url_free(chain->mp, &url);
        async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
        async_ctx_teardown(ctx);
        return false;
      }
    } else {
      ctx->host = ccol_strdup(chain->mp, url.host);
      ctx->port = url.port;
      if (!ctx->host) {
        url_free(chain->mp, &url);
        async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
        async_ctx_teardown(ctx);
        return false;
      }
    }
  }
  url_free(chain->mp, &url);

  async_parser_start(ctx);

  if (reused) {
    ccol_retval reactivate_rv;
#ifdef RUNNING_UNIT_TESTS
    if (atomic_exchange(&g_force_reactivate_fail_for_tests, false)) {
      reactivate_rv = ccol_unexpected_failure;
    } else
#endif
    {
      reactivate_rv = ceventloop_modify(cli_engine_bundler.reactor, ctx->reg,
                                        cselect_write);
    }
    if (reactivate_rv != ccol_success) {
      ctx->pending_app_teardown = true;
      async_retry_hop(ctx);
#ifdef RUNNING_UNIT_TESTS
      atomic_store(&g_abandon_shutdown_under_lock_for_tests,
                   ctx->idle_lock_held_for_tests ? 1 : 0);
#endif
      if (ctx->fd >= 0) shutdown(ctx->fd, SHUT_RDWR);
#ifdef RUNNING_UNIT_TESTS
      ctx->idle_lock_held_for_tests = false;
#endif
      ccol__mutex_unlock(ctx->idle_lock);
      client_engine_release();
      return true;
    }
#ifdef RUNNING_UNIT_TESTS
    ctx->idle_lock_held_for_tests = false;
#endif
    ccol__mutex_unlock(ctx->idle_lock);
    client_engine_release();
    return true;
  }

  client_deadline_register(ctx);
  client_deadline_note(&ctx->connect_deadline);
  client_deadline_note(&chain->overall_deadline);

  ccol_retval sr = async_connect_begin(ctx);
  if (sr != ccol_success) {
    async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
    async_ctx_teardown(ctx);
    return false;
  }
  return true;
}

// Follows a redirect once a hop's response is fully parsed as one. It resolves
// Location against this hop's URL, applies the method and body rules (307 and
// 308 keep them, other statuses turn into a bodiless GET unless HEAD), finishes
// this hop's connection, and submits the next hop. A temporary chain reference
// keeps the chain alive across the hand-off. Past the redirect limit, or when
// Location can't be resolved, it fulfils the future instead.
static void async_handle_redirect(chttp_async_ctx *ctx, bool reusable) {
  chttp_async_chain *chain = ctx->chain;
  async_chain_retain(chain);

  if (ctx->hop >= CHTTP_MAX_REDIRECTS) {
    async_finish_connection(ctx, reusable);
    async_fulfill_chain(chain, ccol_http_too_many_redirects, NULL);
    async_chain_release(chain);
    return;
  }

  chttp_url base;
  memset(&base, 0, sizeof(base));
  base.is_https = ctx->is_https;
  base.is_ipv6 = ctx->is_ipv6;
  base.is_unix = ctx->is_unix;
  base.unix_socket_path = ctx->unix_socket_path;
  base.host = ctx->host;
  base.port = ctx->port;
  base.path_and_query = ctx->path_and_query;

  ccol_retval resolve_rv = ccol_success;
  char *next_url =
      resolve_redirect_url(chain->mp, &base, ctx->pctx.location, &resolve_rv);
  bool transport_refused = false;
  ccol_retval transport_rv =
      next_url
          ? redirect_transport_allowed(chain->mp, &base, next_url,
                                       chain->prevent_tls_downgrade_on_redirect)
          : ccol_success;
  if (transport_rv != ccol_success) {
    ccol__mem_free(chain->mp, next_url);
    next_url = NULL;
    transport_refused = true;
  }
  bool preserve =
      (ctx->pctx.status_code == 307 || ctx->pctx.status_code == 308);
  chttp_method next_method = ctx->cur_method;
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
  int next_hop = ctx->hop + 1;

  async_finish_connection(ctx, reusable);

  if (next_url) {
    async_submit_hop(chain, next_url, next_method, next_body_data,
                     next_body_len, next_body_ct, next_hop);
    ccol__mem_free(chain->mp, next_url);
  } else if (transport_refused) {
    async_fulfill_chain(chain, transport_rv, NULL);
  } else {
    async_fulfill_chain(chain, resolve_rv, NULL);
  }
  async_chain_release(chain);
}

// Runs the Tier 2 and Tier 3 checks before anything is queued, so a bad request
// fails at once with the same code as in Tier 1: Valid cli and req, a URL that
// parses into *url_out (freed by the caller after success), and usable TLS for
// https.
static ccol_retval chttp_async_preflight_check(struct chttpclient *cli,
                                               const chttp_request *req,
                                               chttp_url *url_out) {
  if (!cli || !req) return ccol_invalid_args;

  ccol_retval prv = parse_chttp_url(cli->m_procs, req->url, url_out);
  if (prv != ccol_success) return prv;

  ccol__mutex_lock(cli->lock);
  if (url_out->is_https) tls_ctx_retry_if_oom_locked(cli);
  bool tls_ctx_usable = cli->tls_ctx_usable;
  bool tls_ctx_oom = cli->tls_ctx_oom;
  ccol__mutex_unlock(cli->lock);

  if (url_out->is_https && !tls_ctx_usable) {
    url_free(cli->m_procs, url_out);
    return tls_ctx_unusable_code(tls_ctx_oom);
  }
  return ccol_success;
}

// Submits req to run asynchronously on cli and gives its future, which the
// caller frees once with cthrpool_future_free. It gives NULL with *rv_out set
// when the request can't be queued (bad arguments, a bad URL, unusable TLS, a
// failed allocation, an engine that won't start). It snapshots the client's TLS
// context and settings under cli->lock and allocates the result before the
// future exists, so the fulfilment never allocates.
static cthrpool_future *chttp_do_async_internal(struct chttpclient *cli,
                                                const chttp_request *req,
                                                chttpcli_write_fn write_fn,
                                                void *write_ctx,
                                                ccol_retval *rv_out) {
  chttp_url url;
  ccol_retval prv = chttp_async_preflight_check(cli, req, &url);
  if (prv != ccol_success) {
    *rv_out = prv;
    return NULL;
  }
  *rv_out = ccol_not_enough_memory;
  ccol_memmgmt_procs *mp = cli->m_procs;
  char *initial_origin_key = ccol_strdup(mp, url.origin_key);
  url_free(mp, &url);
  if (!initial_origin_key) return NULL;

  ctls_ctx *tls_ctx;
  bool tls_ctx_usable;
  bool verify_host;
  uint64_t tls_generation;
  uint64_t connect_timeout_us;
  uint64_t request_timeout_us;
  uint64_t inactivity_timeout_us;
  size_t max_response_body_size;
  ccol__mutex_lock(cli->lock);
  tls_ctx_retry_if_oom_locked(cli);
  tls_ctx = cli->tls_ctx;
  tls_ctx_usable = cli->tls_ctx_usable;
  bool tls_ctx_oom = cli->tls_ctx_oom;
  verify_host =
      !cli->tls.insecure_skip_verify && !cli->tls.insecure_skip_hostname_check;
  tls_generation = cli->tls_generation;
  connect_timeout_us = cli->connect_timeout_us;
  request_timeout_us = cli->request_timeout_us;
  inactivity_timeout_us = cli->inactivity_timeout_us;
  max_response_body_size = cli->max_response_body_size;
  if (tls_ctx) ctls_ctx_retain(tls_ctx);
  ccol__mutex_unlock(cli->lock);

  ccol_retval arv = client_engine_acquire_chain();
  if (arv != ccol_success) {
    if (tls_ctx) ctls_ctx_release(tls_ctx);
    ccol__mem_free(mp, initial_origin_key);
    *rv_out = arv;
    return NULL;
  }

  chttpcli_async_result *result = async_result_alloc(mp);
  char *ferr = NULL;
  cthrpool_future *future =
      result ? cthrpool_future_create_detached(&ferr) : NULL;
  if (!future) {
    if (result) chttpcli_async_result_free(result);
    if (tls_ctx) ctls_ctx_release(tls_ctx);
    client_async_request_settled();
    client_engine_release_chain();
    ccol__mem_free(mp, initial_origin_key);
    return NULL;
  }
  *rv_out = ccol_success;
  cthrpool_future *f = future;

  chttp_async_chain *chain = async_chain_create(
      mp, cli, future, (chmap)req->headers, req->body.data, req->body.len,
      req->body.content_type, initial_origin_key, tls_ctx, tls_ctx_usable,
      verify_host, tls_generation, connect_timeout_us, request_timeout_us,
      max_response_body_size, write_fn, write_ctx, req->expect_continue,
      req->prevent_tls_downgrade_on_redirect);
  if (chain) {
    chain->inactivity_timeout_us = inactivity_timeout_us;
    chain->tls_ctx_oom = tls_ctx_oom;
  }
  ccol__mem_free(mp, initial_origin_key);
  if (!chain) {
    if (tls_ctx) ctls_ctx_release(tls_ctx);
    client_async_request_settled();
    result->rv = ccol_not_enough_memory;
    result->resp = NULL;
    cthrpool_future_fulfill(future, result);
    client_engine_release_chain();
    return f;
  }
  chain->result = result;
  async_submit_hop(chain, req->url, req->method, req->body.data, req->body.len,
                   req->body.content_type, 0);
  return f;
}

#ifdef RUNNING_UNIT_TESTS
// Engine test hooks
//
// Give a test sizeof(chttp_async_chain), the engine's reference count and
// logger, and let it clear the installed logger, see whether the engine runs,
// take and drop a reference, wait for a teardown and see whether the globals
// were ever initialized.
size_t chttp__async_chain_struct_size_for_tests(void) {
  return sizeof(chttp_async_chain);
}

int chttpcli__engine_ref_count_for_tests(void) {
  ccol__call_once(cli_engine_bundler.once, client_engine_globals_init);
  ccol__mutex_lock(cli_engine_bundler.mutex);
  int n = (int)cli_engine_bundler.reactor_refs;
  ccol__mutex_unlock(cli_engine_bundler.mutex);
  return n;
}

clog chttpcli__engine_logger_for_tests(void) {
  ccol__call_once(cli_engine_bundler.once, client_engine_globals_init);
  ccol__mutex_lock(cli_engine_bundler.mutex);
  clog l = cli_engine_bundler.logger;
  ccol__mutex_unlock(cli_engine_bundler.mutex);
  return l;
}

void chttpcli__clear_engine_user_logger_for_tests(void) {
  ccol__call_once(cli_engine_bundler.once, client_engine_globals_init);
  ccol__mutex_lock(cli_engine_bundler.mutex);
  clog user = cli_engine_bundler.user_logger;
  cli_engine_bundler.user_logger = CLOG_NULL;
  if (cli_engine_bundler.logger == user) cli_engine_bundler.logger = CLOG_NULL;
  ccol__mutex_unlock(cli_engine_bundler.mutex);
  if (user) clog_close(user);
}

bool chttpcli__engine_running_for_tests(void) {
  ccol__call_once(cli_engine_bundler.once, client_engine_globals_init);
  ccol__mutex_lock(cli_engine_bundler.mutex);
  bool running = (cli_engine_bundler.reactor != CEVENTLOOP_NULL);
  ccol__mutex_unlock(cli_engine_bundler.mutex);
  return running;
}

ccol_retval chttpcli__engine_acquire_for_tests(void) {
  return client_engine_acquire();
}

void chttpcli__engine_release_for_tests(void) { client_engine_release(); }

void chttpcli__engine_wait_for_quiescence_for_tests(void) {
  client_engine_wait_for_quiescence();
}

bool chttpcli__engine_globals_entered_for_tests(void) {
  return atomic_load(&g_engine_globals_entered);
}

// Gives the running engine's DNS pool (CTHRPOOL_NULL with no engine), so a test
// can check what the engine built.
cthrpool chttpcli__engine_dns_pool_for_tests(void) {
  ccol__call_once(cli_engine_bundler.once, client_engine_globals_init);
  ccol__mutex_lock(cli_engine_bundler.mutex);
  cthrpool h = cli_engine_bundler.dns_pool;
  ccol__mutex_unlock(cli_engine_bundler.mutex);
  return h;
}

// Arm the reaper holds (see g_reaper_final_hold_ms_for_tests and
// g_reaper_tail_hold_ms_for_tests) and tell whether each was reached, so a test
// can race the reaper's last steps.
void chttpcli__hold_next_reaper_final_section_for_tests(int ms) {
  atomic_store(&g_reaper_final_hold_entered_for_tests, false);
  atomic_store(&g_reaper_final_hold_ms_for_tests, ms);
}

bool chttpcli__reaper_final_section_was_entered_for_tests(void) {
  return atomic_load(&g_reaper_final_hold_entered_for_tests);
}

void chttpcli__hold_next_reaper_tail_for_tests(int ms) {
  atomic_store(&g_reaper_tail_hold_entered_for_tests, false);
  atomic_store(&g_reaper_tail_hold_left_for_tests, false);
  atomic_store(&g_reaper_tail_hold_ms_for_tests, ms);
}

bool chttpcli__reaper_tail_was_entered_for_tests(void) {
  return atomic_load(&g_reaper_tail_hold_entered_for_tests);
}

bool chttpcli__reaper_tail_was_left_for_tests(void) {
  return atomic_load(&g_reaper_tail_hold_left_for_tests);
}

// Waits until no teardown is in flight and leaves the finished reaper unjoined,
// so a test can fork in the state between an engine stop and the next acquire.
void chttpcli__engine_wait_stopped_unjoined_for_tests(void) {
  ccol__call_once(cli_engine_bundler.once, client_engine_globals_init);
  ccol__mutex_lock(cli_engine_bundler.mutex);
  while (cli_engine_bundler.stopping)
    ccol__cond_var_wait(cli_engine_bundler.stopped_cv,
                        cli_engine_bundler.mutex);
  ccol__mutex_unlock(cli_engine_bundler.mutex);
}

// Copies the handle of the unjoined reaper into *out, so a test can compare it
// after a fork. Gives false when there's none.
bool chttpcli__engine_unjoined_reaper_for_tests(ccol__thread_id *out) {
  ccol__call_once(cli_engine_bundler.once, client_engine_globals_init);
  ccol__mutex_lock(cli_engine_bundler.mutex);
  bool joinable = cli_engine_bundler.reaper_joinable;
  if (joinable) *out = cli_engine_bundler.reaper_thread;
  ccol__mutex_unlock(cli_engine_bundler.mutex);
  return joinable;
}

// Read and reset the probe of g_abandon_shutdown_under_lock_for_tests, so a
// test can check the shutdown order of an abandoned ctx.
int chttpcli__abandon_shutdown_under_lock_for_tests(void) {
  return atomic_load(&g_abandon_shutdown_under_lock_for_tests);
}

void chttpcli__reset_abandon_shutdown_probe_for_tests(void) {
  atomic_store(&g_abandon_shutdown_under_lock_for_tests, -1);
}

// Arms the TLS direction injection (0 off, 1 a write needs readable, 2 a read
// needs writable) and clears its counters. A test disarms it before returning,
// since the engine threads keep running.
void chttpcli__tls_dir_inject_for_tests(int mode) {
  atomic_store(&g_tls_dir_blocked, false);
  atomic_store(&g_tls_dir_released, false);
  atomic_store(&g_tls_dir_block_attempts, 0);
  atomic_store(&g_tls_dir_read_interest_while_writing, 0);
  atomic_store(&g_tls_dir_write_interest_while_reading, 0);
  atomic_store(&g_tls_dir_inject_mode, mode);
}

// Read the state of the TLS direction injection, so a test can see that the
// code waited in the right direction: Whether the block was released, the
// interest in the other direction seen while it was pending, and how many
// attempts it refused (a caller that ignores the reported direction spins and
// drives that count up).
bool chttpcli__tls_dir_released_for_tests(void) {
  return atomic_load(&g_tls_dir_released);
}

unsigned chttpcli__tls_dir_read_interest_while_writing_for_tests(void) {
  return atomic_load(&g_tls_dir_read_interest_while_writing);
}

unsigned chttpcli__tls_dir_write_interest_while_reading_for_tests(void) {
  return atomic_load(&g_tls_dir_write_interest_while_reading);
}

unsigned chttpcli__tls_dir_block_attempts_for_tests(void) {
  return atomic_load(&g_tls_dir_block_attempts);
}

// Hold, query and release the next reaper spawn (see
// g_reaper_spawn_hold_for_tests) and read the created and joined counts, so a
// test can check that no reaper handle is lost.
void chttpcli__hold_next_reaper_spawn_for_tests(void) {
  atomic_store(&g_reaper_spawn_gate_for_tests, false);
  atomic_store(&g_reaper_spawn_held_for_tests, false);
  atomic_store(&g_reaper_spawn_hold_for_tests, true);
}

bool chttpcli__reaper_spawn_is_held_for_tests(void) {
  return atomic_load(&g_reaper_spawn_held_for_tests);
}

void chttpcli__release_reaper_spawn_for_tests(void) {
  atomic_store(&g_reaper_spawn_gate_for_tests, true);
}

unsigned chttpcli__reaper_created_count_for_tests(void) {
  return atomic_load(&g_reaper_created_for_tests);
}

unsigned chttpcli__reaper_joined_count_for_tests(void) {
  return atomic_load(&g_reaper_joined_for_tests);
}

// Make every reaper spawn fail and read how often the spawn gave up, so a test
// reaches the abandoned-reap path without a real thread limit.
void chttpcli__force_reaper_spawn_fail_for_tests(bool on) {
  atomic_store(&g_force_reaper_spawn_fail_for_tests, on);
}

unsigned chttpcli__engine_reaper_abandoned_count_for_tests(void) {
  return atomic_load(&g_reaper_abandoned_count_for_tests);
}

// Gives the reactor thread count the engine was last built with, so a test can
// check chttpcli_set_engine_num_reactor_threads.
size_t chttpcli__engine_num_reactor_threads_for_tests(void) {
  ccol__call_once(cli_engine_bundler.once, client_engine_globals_init);
  ccol__mutex_lock(cli_engine_bundler.mutex);
  size_t n = cli_engine_bundler.last_resolved_num_reactor_threads;
  ccol__mutex_unlock(cli_engine_bundler.mutex);
  return n;
}

// Ages every connection in cli's async idle pool past CHTTP_IDLE_MAX_AGE_MS, so
// the next take evicts it without waiting 60 s.
void chttpcli__force_async_idle_stale_for_tests(struct chttpclient *cli) {
  ccol__mutex_lock(cli->lock);
  if (cli->idle_pools_async) {
    cmap_iterator *it = chashmap_begin_iter(cli->idle_pools_async, NULL);
    for (; it; it = it->ccol__next_fn(it)) {
      cvec list = read_cvec(it->val_pair->ptr);
      if (!list) continue;
      size_t n = cvector_elem_count(list);
      for (size_t i = 0; i < n; i++) {
        chttp_async_ctx *actx = *(chttp_async_ctx **)cvector_at(list, i);
        actx->last_used.tv_sec -= (CHTTP_IDLE_MAX_AGE_MS / 1000L) + 5;
      }
    }
  }
  ccol__mutex_unlock(cli->lock);
}

// Shuts down every fd in cli's async idle pool, so a test can drive the IDLE
// dispatch that removes and frees each connection.
void chttpcli__shutdown_async_idle_connections_for_tests(
    struct chttpclient *cli) {
  ccol__mutex_lock(cli->lock);
  if (cli->idle_pools_async) {
    cmap_iterator *it = chashmap_begin_iter(cli->idle_pools_async, NULL);
    for (; it; it = it->ccol__next_fn(it)) {
      cvec list = read_cvec(it->val_pair->ptr);
      if (!list) continue;
      size_t n = cvector_elem_count(list);
      for (size_t i = 0; i < n; i++) {
        chttp_async_ctx *actx = *(chttp_async_ctx **)cvector_at(list, i);
        int afd = actx->fd;
        if (afd >= 0) shutdown(afd, SHUT_RDWR);
      }
    }
  }
  ccol__mutex_unlock(cli->lock);
}

// Count the connections of cli's async idle pool and those that carry a
// timed_out mark (always 0 when correct), so a test can check pool membership
// directly.
size_t chttpcli__async_idle_total_count_for_tests(struct chttpclient *cli) {
  ccol__mutex_lock(cli->lock);
  size_t n = cli->idle_total_count_async;
  ccol__mutex_unlock(cli->lock);
  return n;
}

size_t chttpcli__async_idle_timed_out_count_for_tests(struct chttpclient *cli) {
  size_t n = 0;
  ccol__mutex_lock(cli->lock);
  if (cli->idle_pools_async) {
    cmap_iterator *it = chashmap_begin_iter(cli->idle_pools_async, NULL);
    for (; it; it = it->ccol__next_fn(it)) {
      cvec list = read_cvec(it->val_pair->ptr);
      size_t count = list ? cvector_elem_count(list) : 0;
      for (size_t i = 0; i < count; i++) {
        chttp_async_ctx **slot = (chttp_async_ctx **)cvector_at(list, i);
        if ((*slot)->timed_out) n++;
      }
    }
  }
  ccol__mutex_unlock(cli->lock);
  return n;
}

// Count the origin keys of the Tier 1 and the async idle pools, so a test can
// see that an emptied origin loses its entry.
size_t chttpcli__idle_pools_key_count_for_tests(struct chttpclient *cli) {
  ccol__mutex_lock(cli->lock);
  size_t n = cli->idle_pools ? chashmap_elem_count(cli->idle_pools) : 0;
  ccol__mutex_unlock(cli->lock);
  return n;
}

size_t chttpcli__idle_pools_async_key_count_for_tests(struct chttpclient *cli) {
  ccol__mutex_lock(cli->lock);
  size_t n =
      cli->idle_pools_async ? chashmap_elem_count(cli->idle_pools_async) : 0;
  ccol__mutex_unlock(cli->lock);
  return n;
}

// Overrides CHTTP_MAX_IDLE_ORIGINS for the whole process; 0 restores it.
void chttpcli__set_max_idle_origins_for_tests(size_t n) {
  atomic_store(&g_max_idle_origins_override_for_tests, n);
}

// Gives the client behind h without pinning it, for single-threaded tests that
// can't race a destroy.
struct chttpclient *chttpcli__resolve_for_tests(chttpcli h) {
  ccol__call_once(chttpcli_slot_table.once, chttpcli_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  ccol__rw_lock_rdlock(chttpcli_slot_table.rwlock);
  struct chttpclient *raw = NULL;
  if (idx < cvector_elem_count(chttpcli_slot_table.slots)) {
    chttpcli_slot *slot =
        (chttpcli_slot *)cvector_at(chttpcli_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  ccol__rw_lock_unlock(chttpcli_slot_table.rwlock);
  return raw;
}

// Give the slot table's capacity and its free-list length, so a test can see
// that freed slots are reused.
size_t chttpcli__free_index_count_for_tests(void) {
  ccol__call_once(chttpcli_slot_table.once, chttpcli_slot_table_init_globals);
  ccol__rw_lock_rdlock(chttpcli_slot_table.rwlock);
  size_t n = cvector_elem_count(chttpcli_slot_table.free_indices);
  ccol__rw_lock_unlock(chttpcli_slot_table.rwlock);
  return n;
}

size_t chttpcli__slot_table_capacity_for_tests(void) {
  ccol__call_once(chttpcli_slot_table.once, chttpcli_slot_table_init_globals);
  ccol__rw_lock_rdlock(chttpcli_slot_table.rwlock);
  size_t n = cvector_elem_count(chttpcli_slot_table.slots);
  ccol__rw_lock_unlock(chttpcli_slot_table.rwlock);
  return n;
}

// Gives the number of pins held on h's slot, so a test can check that resolves
// and unpins stay balanced.
size_t chttpcli__pin_count_for_tests(chttpcli h) {
  return ccol_pintable_pins_for(&chttpcli_pintable, h);
}

// Make the next push of async_idle_pool_offer fail, or arm a stray write flip
// on the next plain hop that finishes writing or the next connection that goes
// idle, so a test reaches paths that allocator failures can't (see the hook
// variables above the async idle pool section).
void chttpcli__force_offer_push_fail_once_for_tests(void) {
  atomic_store(&g_force_offer_push_fail_for_tests, true);
}

void chttpcli__force_stray_write_arm_once_for_tests(void) {
  atomic_store(&g_force_stray_write_arm_for_tests, true);
}

void chttpcli__force_stray_idle_write_arm_once_for_tests(void) {
  atomic_store(&g_force_stray_idle_write_arm_for_tests, true);
}

// Arm, wait for and release the IDLE probe hold (see
// g_idle_probe_hold_for_tests), so a test can make a take beat the evicting
// dispatch.
void chttpcli__arm_idle_probe_hold_for_tests(void) {
  atomic_store(&g_idle_probe_hold_for_tests, 1);
}

bool chttpcli__wait_idle_probe_held_for_tests(long timeout_ms) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
  for (long i = 0; i < timeout_ms; i++) {
    if (atomic_load(&g_idle_probe_hold_for_tests) == 2) return true;
    nanosleep(&ts, NULL);
  }
  return atomic_load(&g_idle_probe_hold_for_tests) == 2;
}

void chttpcli__release_idle_probe_hold_for_tests(void) {
  int holding = 2;
  if (!atomic_compare_exchange_strong(&g_idle_probe_hold_for_tests, &holding,
                                      3))
    atomic_store(&g_idle_probe_hold_for_tests, 0);
}

// Sets the delay around the 100-continue carry replay (0 turns it off), so a
// test can widen the window where a destroy may race the replay.
void chttpcli__set_continue_carry_replay_delay_ms_for_tests(long ms) {
  atomic_store(&g_continue_carry_replay_delay_ms_for_tests, ms);
}

// Arm the stray write dispatch at a take: Either the take arms the write
// direction under idle_lock and waits 200 ms, or it arms it before taking
// idle_lock, waits for the dispatch to classify the ctx as IDLE, then moves it
// to WRITING. The last one tells whether that hold happened.
void chttpcli__arm_stray_write_at_take_for_tests(void) {
  atomic_store(&g_stray_write_at_take_for_tests, true);
}

void chttpcli__arm_stray_write_before_take_for_tests(void) {
  atomic_store(&g_stray_before_take_stage_for_tests, 1);
}

bool chttpcli__stray_write_before_take_held_for_tests(void) {
  bool held = atomic_load(&g_stray_before_take_stage_for_tests) >= 3;
  atomic_store(&g_stray_before_take_stage_for_tests, 0);
  return held;
}

// Gives how often the write-direction fall-through put a registration back to
// reading, so the stray-write tests can see it happened.
unsigned long long chttpcli__stray_write_arm_restores_for_tests(void) {
  return atomic_load_explicit(&g_stray_write_arm_restores_for_tests,
                              memory_order_relaxed);
}

// Make the next reactivation of a reused connection fail, or the next read of a
// ctx that has already read once act as ECONNRESET, without touching the real
// registration or socket.
void chttpcli__force_reactivate_fail_once_for_tests(void) {
  atomic_store(&g_force_reactivate_fail_for_tests, true);
}

void chttpcli__force_async_hard_read_error_once_for_tests(void) {
  atomic_store(&g_force_async_hard_read_error_for_tests, true);
}
#endif

// Submits a buffered request to the async engine (Tier 2), for callers that
// don't want to block. Gives the future, whose result holds the response, or
// NULL on failure.
cthrpool_future *chttpcli_do_async(chttpcli h, const chttp_request *req) {
  struct chttpclient *raw = chttpcli_resolve(h);
  if (!raw) return NULL;
  ccol_retval rv;
  cthrpool_future *f = chttp_do_async_internal(raw, req, NULL, NULL, &rv);
  chttpcli_resolve_unpin(raw);
  return f;
}

// Submits a streaming request to the async engine (Tier 2): The body goes to
// write_fn on an engine thread, so a large body needs no buffer. Gives the
// future, or NULL on failure.
cthrpool_future *chttpcli_do_async_streaming(chttpcli h,
                                             const chttp_request *req,
                                             chttpcli_write_fn write_fn,
                                             void *write_ctx) {
  if (!write_fn) return NULL;
  struct chttpclient *raw = chttpcli_resolve(h);
  if (!raw) return NULL;
  ccol_retval rv;
  cthrpool_future *f =
      chttp_do_async_internal(raw, req, write_fn, write_ctx, &rv);
  chttpcli_resolve_unpin(raw);
  return f;
}

// Gives the result a fulfilled future holds, so a caller of chttpcli_do_async
// can read its outcome.
chttpcli_async_result *chttpcli_async_result_get(cthrpool_future *f) {
  return (chttpcli_async_result *)cthrpool_future_get(f);
}

// Frees an async result and its response, through the procs copy the result
// carries (read into a local, since it lives in the freed block).
void chttpcli_async_result_free(chttpcli_async_result *result) {
  if (!result) return;
  ccol_memmgmt_procs procs;
  ccol_memmgmt_procs *mp = NULL;
  if (result->ccol__m_procs) {
    procs = *result->ccol__m_procs;
    mp = &procs;
  }
  ccol__mem_free(mp, result);
}

// Pooled sync API (Tier 3)
//
// Runs req through the async engine and blocks on its future, so many threads
// can share the engine's connections with a synchronous call. Hands back the
// result as chttpcli_do does; the submit's failure code is returned as it is.
ccol_retval chttpcli_do_pooled(chttpcli h, const chttp_request *req,
                               chttpcli_response **resp_out) {
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;

  struct chttpclient *raw = chttpcli_resolve(h);
  if (!raw) return ccol_invalid_args;
  ccol_retval qrv;
  cthrpool_future *f = chttp_do_async_internal(raw, req, NULL, NULL, &qrv);
  chttpcli_resolve_unpin(raw);
  if (!f) return qrv;

  chttpcli_async_result *result = chttpcli_async_result_get(f);
  if (!result) {
    cthrpool_future_free(f);
    return ccol_unexpected_failure;
  }

  ccol_retval rv = result->rv;
  *resp_out = result->resp;
  chttpcli_async_result_free(result);
  cthrpool_future_free(f);
  return rv;
}

// Runs a streaming request through the async engine and blocks on it, the
// streaming form of chttpcli_do_pooled: The body goes to write_fn on an engine
// thread, and *status_code_out gets the final status.
ccol_retval chttpcli_do_pooled_streaming(chttpcli h, const chttp_request *req,
                                         chttpcli_write_fn write_fn,
                                         void *write_ctx,
                                         int *status_code_out) {
  if (!write_fn) return ccol_invalid_args;

  struct chttpclient *raw = chttpcli_resolve(h);
  if (!raw) return ccol_invalid_args;
  ccol_retval qrv;
  cthrpool_future *f =
      chttp_do_async_internal(raw, req, write_fn, write_ctx, &qrv);
  chttpcli_resolve_unpin(raw);
  if (!f) return qrv;

  chttpcli_async_result *result = chttpcli_async_result_get(f);
  if (!result) {
    cthrpool_future_free(f);
    return ccol_unexpected_failure;
  }

  ccol_retval rv = result->rv;
  if (result->resp) {
    if (rv == ccol_success && status_code_out) {
      *status_code_out = result->resp->status_code;
    }
    chttpcli_resp_free(result->resp);
  }
  chttpcli_async_result_free(result);
  cthrpool_future_free(f);
  return rv;
}

#ifdef RUNNING_UNIT_TESTS
// Client constructors
//
// The create step that chttpcli__arm_create_fault_for_tests armed, or
// CHTTPCLI__FAULT_NONE.
static atomic_int g_chttpcli_create_fault;

// Arms one create step to fail, so a test can sweep every failure path of
// chttpcli_create_mp.
void chttpcli__arm_create_fault_for_tests(chttpcli__fault step) {
  atomic_store(&g_chttpcli_create_fault, (int)step);
}

// True once for the armed step, on the first create that reaches it.
static bool chttpcli_create_fault_fires(chttpcli__fault step) {
  int expected = (int)step;
  return atomic_compare_exchange_strong(&g_chttpcli_create_fault, &expected,
                                        (int)CHTTPCLI__FAULT_NONE);
}
// Evaluates to true when the armed create fault fires at step, so
// chttpcli_create_mp can fail one step on demand in a test build; without the
// test hooks it's false and a create never fails a step on its own.
#define CHTTPCLI_CREATE_FAULT(step) chttpcli_create_fault_fires(step)
#else
#define CHTTPCLI_CREATE_FAULT(step) false
#endif

// Creates a client that allocates through mprocs (NULL for malloc); it's the
// constructor behind chttpcli_create and every tier's client. It builds the
// locks (the limiter's condition variable on CLOCK_MONOTONIC, the clock of the
// deadlines), the idle pools and the default TLS context, and acquires a handle
// slot last, so nothing can resolve a half-built client. Gives the handle, or
// CHTTPCLI_NULL with *err_str set.
chttpcli chttpcli_create_mp(ccol_memmgmt_procs *mprocs, char **err_str) {
  if (mprocs && !ccol__verify_memmgmt_procs(mprocs, err_str))
    return CHTTPCLI_NULL;

  ccol_memmgmt_procs *mp = NULL;
  if (mprocs) {
    mp = (ccol_memmgmt_procs *)mprocs->malloc(sizeof(ccol_memmgmt_procs));
    if (!mp) {
      if (err_str) *err_str = CCOL__ERR_STR("failed to allocate mprocs");
      return CHTTPCLI_NULL;
    }
    memcpy(mp, mprocs, sizeof(ccol_memmgmt_procs));
  }

  struct chttpclient *cli =
      (struct chttpclient *)ccol__mem_calloc(mp, 1, sizeof(struct chttpclient));
  if (!cli) {
    if (err_str) *err_str = CCOL__ERR_STR("failed to allocate client");
    if (mp) mp->free(mp);
    return CHTTPCLI_NULL;
  }

  cli->m_procs = mp;
  cli->tls = CHTTP_TLS_DEFAULT;
  cli->connect_timeout_us = CHTTPCLI_DEFAULT_CONNECT_TIMEOUT_US;
  cli->inactivity_timeout_us = CHTTPCLI_DEFAULT_INACTIVITY_TIMEOUT_US;
  if (CHTTPCLI_CREATE_FAULT(CHTTPCLI__FAULT_LOCK) ||
      ccol__mutex_init(cli->lock) != 0) {
    if (err_str) *err_str = CCOL__ERR_STR("failed to initialize mutex");
    ccol__mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_NULL;
  }
  if (CHTTPCLI_CREATE_FAULT(CHTTPCLI__FAULT_AVAILABLE_CV) ||
      ccol_cond_var_init_monotonic(&cli->available) != 0) {
    if (err_str)
      *err_str = CCOL__ERR_STR("failed to initialize condition variable");
    ccol__mutex_destroy(cli->lock);
    ccol__mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_NULL;
  }
  if (CHTTPCLI_CREATE_FAULT(CHTTPCLI__FAULT_IDLE_ASYNC_DRAINED_CV) ||
      ccol__cond_var_init(cli->idle_async_drained) != 0) {
    if (err_str)
      *err_str = CCOL__ERR_STR("failed to initialize condition variable");
    ccol__mutex_destroy(cli->lock);
    ccol__cond_var_destroy(cli->available);
    ccol__mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_NULL;
  }
  if (CHTTPCLI_CREATE_FAULT(CHTTPCLI__FAULT_ASYNC_COUNT_LOCK) ||
      ccol__mutex_init(cli->async_count_lock) != 0) {
    if (err_str) *err_str = CCOL__ERR_STR("failed to initialize mutex");
    ccol__mutex_destroy(cli->lock);
    ccol__cond_var_destroy(cli->available);
    ccol__cond_var_destroy(cli->idle_async_drained);
    ccol__mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_NULL;
  }
  if (CHTTPCLI_CREATE_FAULT(CHTTPCLI__FAULT_ASYNC_COUNT_DRAINED_CV) ||
      ccol__cond_var_init(cli->async_count_drained) != 0) {
    if (err_str)
      *err_str = CCOL__ERR_STR("failed to initialize condition variable");
    ccol__mutex_destroy(cli->lock);
    ccol__cond_var_destroy(cli->available);
    ccol__cond_var_destroy(cli->idle_async_drained);
    ccol__mutex_destroy(cli->async_count_lock);
    ccol__mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_NULL;
  }

  char *herr = NULL;
  cli->idle_pools =
      chashmap_create_full(CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                           ccol_pointer, mp, NULL, NULL, &herr);
  if (!cli->idle_pools) {
    if (err_str) *err_str = CCOL__ERR_STR("failed to allocate idle pool map");
    ccol__mutex_destroy(cli->lock);
    ccol__cond_var_destroy(cli->available);
    ccol__cond_var_destroy(cli->idle_async_drained);
    ccol__mutex_destroy(cli->async_count_lock);
    ccol__cond_var_destroy(cli->async_count_drained);
    ccol__mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_NULL;
  }

  cli->idle_pools_async =
      chashmap_create_full(CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                           ccol_pointer, mp, NULL, NULL, &herr);
  if (!cli->idle_pools_async) {
    if (err_str)
      *err_str = CCOL__ERR_STR("failed to allocate async idle pool map");
    chashmap__destroy(cli->idle_pools);
    ccol__mutex_destroy(cli->lock);
    ccol__cond_var_destroy(cli->available);
    ccol__cond_var_destroy(cli->idle_async_drained);
    ccol__mutex_destroy(cli->async_count_lock);
    ccol__cond_var_destroy(cli->async_count_drained);
    ccol__mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_NULL;
  }

  if (rebuild_tls_ctx_locked(cli) != ccol_success) {
    if (err_str)
      *err_str = CCOL__ERR_STR("failed to build default TLS context");
    chashmap__destroy(cli->idle_pools);
    chashmap__destroy(cli->idle_pools_async);
    ccol__mutex_destroy(cli->lock);
    ccol__cond_var_destroy(cli->available);
    ccol__cond_var_destroy(cli->idle_async_drained);
    ccol__mutex_destroy(cli->async_count_lock);
    ccol__cond_var_destroy(cli->async_count_drained);
    ccol__mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_NULL;
  }

  chttpcli h = chttpcli_handle_slot_acquire(cli);
  if (h == 0) {
    if (err_str) *err_str = CCOL__ERR_STR("failed to allocate client slot");
    if (cli->tls_ctx) ctls_ctx_release(cli->tls_ctx);
    chashmap__destroy(cli->idle_pools);
    chashmap__destroy(cli->idle_pools_async);
    ccol__mutex_destroy(cli->lock);
    ccol__cond_var_destroy(cli->available);
    ccol__cond_var_destroy(cli->idle_async_drained);
    ccol__mutex_destroy(cli->async_count_lock);
    ccol__cond_var_destroy(cli->async_count_drained);
    ccol__mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_NULL;
  }
  return h;
}

// Client configuration
//
// Sets the size of h's concurrency limiter (0 picks the default), so an
// application bounds its parallel requests. Takes cli->lock.
ccol_retval chttpcli_set_pool_size(chttpcli h, size_t n) {
  struct chttpclient *cli = chttpcli_resolve(h);
  if (!cli) return ccol_invalid_args;
  ccol__mutex_lock(cli->lock);
  cli->configured_pool_size = n;
  cli->pool_cap = resolve_pool_cap(n);
  cli->pool_initialized = true;
  ccol__cond_var_broadcast(cli->available);
  ccol__mutex_unlock(cli->lock);
  chttpcli_resolve_unpin(cli);
  return ccol_success;
}

// Set h's connect, request and inactivity timeouts in microseconds (0 for no
// limit), so no request waits for ever on a silent peer. Each takes cli->lock
// and affects requests that start later.
ccol_retval chttpcli_set_connect_timeout(chttpcli h, uint64_t us) {
  struct chttpclient *cli = chttpcli_resolve(h);
  if (!cli) return ccol_invalid_args;
  ccol__mutex_lock(cli->lock);
  cli->connect_timeout_us = us;
  ccol__mutex_unlock(cli->lock);
  chttpcli_resolve_unpin(cli);
  return ccol_success;
}

ccol_retval chttpcli_set_request_timeout(chttpcli h, uint64_t us) {
  struct chttpclient *cli = chttpcli_resolve(h);
  if (!cli) return ccol_invalid_args;
  ccol__mutex_lock(cli->lock);
  cli->request_timeout_us = us;
  ccol__mutex_unlock(cli->lock);
  chttpcli_resolve_unpin(cli);
  return ccol_success;
}

ccol_retval chttpcli_set_inactivity_timeout(chttpcli h, uint64_t us) {
  struct chttpclient *cli = chttpcli_resolve(h);
  if (!cli) return ccol_invalid_args;
  ccol__mutex_lock(cli->lock);
  cli->inactivity_timeout_us = us;
  ccol__mutex_unlock(cli->lock);
  chttpcli_resolve_unpin(cli);
  return ccol_success;
}

#ifdef RUNNING_UNIT_TESTS
// Reads the three timeouts of h for a test.
bool chttpcli__timeouts_for_tests(chttpcli h, uint64_t *connect_us,
                                  uint64_t *request_us,
                                  uint64_t *inactivity_us) {
  struct chttpclient *cli = chttpcli_resolve(h);
  if (!cli) return false;
  ccol__mutex_lock(cli->lock);
  *connect_us = cli->connect_timeout_us;
  *request_us = cli->request_timeout_us;
  *inactivity_us = cli->inactivity_timeout_us;
  ccol__mutex_unlock(cli->lock);
  chttpcli_resolve_unpin(cli);
  return true;
}
#endif

// Sets the cap on a buffered response body of h (0 for none), so a bad server
// can't make the client hold an unbounded body. Takes cli->lock.
ccol_retval chttpcli_set_max_response_body_size(chttpcli h, size_t max_bytes) {
  struct chttpclient *cli = chttpcli_resolve(h);
  if (!cli) return ccol_invalid_args;
  ccol__mutex_lock(cli->lock);
  cli->max_response_body_size = max_bytes;
  ccol__mutex_unlock(cli->lock);
  chttpcli_resolve_unpin(cli);
  return ccol_success;
}

// Replaces the client's TLS configuration and rebuilds its context. It refuses
// a certificate without its key (and the reverse), a CA bundle without a cert
// pair to go with mutual TLS settings, and insecure_skip_verify beside a CA
// bundle. A rebuild that runs out of memory puts the defaults back and gives
// ccol_not_enough_memory. Takes cli->lock.
ccol_retval chttpcli_set_tls(chttpcli h, const chttp_tls_config *tls) {
  if (!h) return ccol_invalid_args;
  if (tls && ((tls->cert_path && !tls->key_path) ||
              (!tls->cert_path && tls->key_path)))
    return ccol_invalid_args;
  if (tls && tls->insecure_skip_verify && tls->ca_bundle_path)
    return ccol_invalid_args;

  struct chttpclient *cli = chttpcli_resolve(h);
  if (!cli) return ccol_invalid_args;

  ccol__mutex_lock(cli->lock);

  ccol__mem_free(cli->m_procs, cli->owned_cert_path);
  ccol__mem_free(cli->m_procs, cli->owned_key_path);
  ccol__mem_free(cli->m_procs, cli->owned_ca_bundle_path);
  cli->owned_cert_path = cli->owned_key_path = cli->owned_ca_bundle_path = NULL;

  if (!tls) {
    cli->tls = CHTTP_TLS_DEFAULT;
    ccol_retval rv = rebuild_tls_ctx_locked(cli);
    ccol__mutex_unlock(cli->lock);
    chttpcli_resolve_unpin(cli);
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
    if (rebuild_tls_ctx_locked(cli) != ccol_success) goto oom;
    ccol__mutex_unlock(cli->lock);
    chttpcli_resolve_unpin(cli);
    return ccol_success;
  }

oom:
  ccol__mem_free(cli->m_procs, cli->owned_cert_path);
  ccol__mem_free(cli->m_procs, cli->owned_key_path);
  ccol__mem_free(cli->m_procs, cli->owned_ca_bundle_path);
  cli->owned_cert_path = cli->owned_key_path = cli->owned_ca_bundle_path = NULL;
  cli->tls = CHTTP_TLS_DEFAULT;
  rebuild_tls_ctx_locked(cli);
  ccol__mutex_unlock(cli->lock);
  chttpcli_resolve_unpin(cli);
  return ccol_not_enough_memory;
}

// Client destruction
//
// Destroys a client. It resolves the handle and marks the slot unused in one
// critical section, so a second destroy is fatal. It is fatal too from a
// request of this client on the calling thread and on an engine thread. It
// waits for Tier 1 calls (pins and in-flight count) and for the client's async
// chains, closes the idle connections of both pools, frees the client, and
// releases the slot last. Destroying the default client clears the singleton
// first.
void chttpcli__destroy(chttpcli cli) {
  if (!cli) return;

  chttpcli expected = cli;
  atomic_compare_exchange_strong(&default_client_bundler.client, &expected, 0);

  ccol__call_once(chttpcli_slot_table.once, chttpcli_slot_table_init_globals);
  uint32_t idx = (uint32_t)(cli >> 32);
  uint32_t gen = (uint32_t)(cli & 0xFFFFFFFFu);
  ccol__rw_lock_wrlock(chttpcli_slot_table.rwlock);
  chttpcli_slot *slot = NULL;
  struct chttpclient *raw = NULL;
  if (idx < cvector_elem_count(chttpcli_slot_table.slots)) {
    chttpcli_slot *s =
        (chttpcli_slot *)cvector_at(chttpcli_slot_table.slots, idx);
    if (s->in_use && s->generation == gen) {
      slot = s;
      raw = s->ptr;
    }
  }
  if (!raw) {
    ccol__rw_lock_unlock(chttpcli_slot_table.rwlock);
    ccol__fatal_err(
        "chttpcli_destroy: handle is stale or already destroyed "
        "(double-destroy / use-after-destroy of a chttpcli handle)");
  }
  if (chttpcli_call_runs_on_this_thread(raw)) {
    ccol__rw_lock_unlock(chttpcli_slot_table.rwlock);
    ccol__fatal_err(
        "chttpcli_destroy: called from inside a request of the same "
        "client (a write_fn of that client), which would wait for itself");
  }
  if (chttpcli_runs_on_engine_thread()) {
    ccol__rw_lock_unlock(chttpcli_slot_table.rwlock);
    ccol__fatal_err(
        "chttpcli_destroy: called on a thread of the async engine (a "
        "write_fn of chttpcli_do_async_streaming or "
        "chttpcli_do_pooled_streaming); the destroy waits for work that "
        "the engine threads run, so destroy the client on another thread");
  }
  slot->in_use = false;
  ccol_pintable_retire(&chttpcli_pintable, idx);
  ccol__rw_lock_unlock(chttpcli_slot_table.rwlock);

  ccol__mutex_lock(raw->lock);
  raw->destroying = true;
  ccol__cond_var_broadcast(raw->available);
  ccol__mutex_unlock(raw->lock);

  {
    long delay_ns = 1000;
    while (ccol_pintable_pins(&chttpcli_pintable, idx) > 0) {
      struct timespec ts = {.tv_sec = 0, .tv_nsec = delay_ns};
      nanosleep(&ts, NULL);
      if (delay_ns < 1000000L) delay_ns *= 2;
    }
  }

  ccol__mutex_lock(raw->lock);
  while (raw->in_flight_count > 0)
    ccol__cond_var_wait(raw->available, raw->lock);
  ccol__mutex_unlock(raw->lock);

  ccol__mutex_lock(raw->async_count_lock);
  while (raw->async_in_flight_count > 0)
    ccol__cond_var_wait(raw->async_count_drained, raw->async_count_lock);
  ccol__mutex_unlock(raw->async_count_lock);

  if (raw->idle_pools) {
    for (const ccol_chmap_entry_ref *e =
             ccol_chmap_oldest_entry(raw->idle_pools);
         e;) {
      const cmap_pair *k, *v;
      e = ccol_chmap_entry_read(e, &k, &v);
      cvec list = read_cvec(v->ptr);
      if (list) {
        chttp_conn c;
        while (cvector_elem_count(list) > 0 &&
               cvector_pop_back(list, &c) == ccol_success)
          conn_teardown(raw->m_procs, &c);
        cvector__destroy(list);
      }
    }
    chashmap__destroy(raw->idle_pools);
  }

  ccol__mutex_lock(raw->lock);
  if (raw->idle_pools_async) {
    for (const ccol_chmap_entry_ref *e =
             ccol_chmap_oldest_entry(raw->idle_pools_async);
         e;) {
      const cmap_pair *k, *v;
      e = ccol_chmap_entry_read(e, &k, &v);
      cvec list = read_cvec(v->ptr);
      if (!list) continue;
      size_t n = cvector_elem_count(list);
      for (size_t i = 0; i < n; i++) {
        chttp_async_ctx *actx = *(chttp_async_ctx **)cvector_at(list, i);
        int afd = actx->fd;
        if (afd >= 0) shutdown(afd, SHUT_RDWR);
      }
    }
  }
  while (raw->idle_total_count_async > 0)
    ccol__cond_var_wait(raw->idle_async_drained, raw->lock);
  ccol__mutex_unlock(raw->lock);

  if (raw->idle_pools_async) {
    for (const ccol_chmap_entry_ref *e =
             ccol_chmap_oldest_entry(raw->idle_pools_async);
         e;) {
      const cmap_pair *k, *v;
      e = ccol_chmap_entry_read(e, &k, &v);
      cvec list = read_cvec(v->ptr);
      if (list) cvector__destroy(list);
    }
    chashmap__destroy(raw->idle_pools_async);
  }

  if (raw->tls_ctx) ctls_ctx_release(raw->tls_ctx);

  ccol__mutex_destroy(raw->lock);
  ccol__cond_var_destroy(raw->available);
  ccol__cond_var_destroy(raw->idle_async_drained);
  ccol__mutex_destroy(raw->async_count_lock);
  ccol__cond_var_destroy(raw->async_count_drained);

  ccol_memmgmt_procs *mp = raw->m_procs;
  ccol__mem_free(mp, raw->owned_cert_path);
  ccol__mem_free(mp, raw->owned_key_path);
  ccol__mem_free(mp, raw->owned_ca_bundle_path);
  ccol__mem_free(mp, raw);
  if (mp) mp->free(mp);

  ccol__rw_lock_wrlock(chttpcli_slot_table.rwlock);
  chttpcli_slot *slot2 =
      (chttpcli_slot *)cvector_at(chttpcli_slot_table.slots, idx);
  slot2->ptr = NULL;
  slot2->generation++;
  chttpcli_free_index_release_locked(idx);
  chttpcli_release_slot_table_if_deferred_locked();
  ccol__rw_lock_unlock(chttpcli_slot_table.rwlock);
}

// Request execution
//
// A response that starts arriving while Tier 1 sends the request (a 401 or 413
// from a server that never reads the body). Every tier follows curl's rules:
// interim responses are dropped and the upload continues, a final status of 300
// or more stops the upload once its head is parsed, and a 2xx keeps uploading.
// A connection whose response started early is never reused.
typedef struct {
  chttp1_parser parser;
  bool parser_live;
  bool done;
  bool keep_alive;
  bool stop_send;
  bool answered_early;
  size_t n_discarded;
  ccol_retval error;
} chttp_early_resp;

// Sets every field but the parser, which is read only while parser_live is set.
static void early_resp_init(chttp_early_resp *e) {
  e->parser_live = false;
  e->done = false;
  e->keep_alive = false;
  e->stop_send = false;
  e->answered_early = false;
  e->n_discarded = 0;
  e->error = ccol_success;
}

// Releases the line spill of e's parser if the parser was started, on every
// path that ends an early response.
static void early_resp_release(chttp_early_resp *e) {
  if (e->parser_live) chttp1_parser_release(&e->parser);
  e->parser_live = false;
}

// Starts the parser of e for the response.
static void early_resp_start_parser(chttp_early_resp *e,
                                    chttp_parse_ctx *pctx) {
  ccol__call_once(client_http1_settings_bundler.once, init_chttp1_settings);
  chttp1_parser_init(&e->parser, &client_http1_settings_bundler.settings);
  e->parser.data = pctx;
  (void)chttp1_parser_enable_line_spill(&e->parser, pctx->mp);
  e->parser_live = true;
}

// Feeds data to e's parser while the request goes out, under the rules above;
// an interim response is dropped and parsing continues after it. The state
// lands in e and pctx.
static void early_resp_feed(chttp_early_resp *e, chttp_parse_ctx *pctx,
                            const char *data, size_t len) {
  while (len > 0 && !e->done && e->error == ccol_success) {
    if (!e->parser_live) early_resp_start_parser(e, pctx);
    chttp1_errno err = chttp1_parser_execute(&e->parser, data, len);
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
        ccol_retval rrv = parse_ctx_reset_for_continue(pctx);
        if (rrv != ccol_success) e->error = rrv;
        continue;
      }
      e->done = true;
      e->keep_alive = keep_alive;
      if (len > 0) {
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

// Handles the peer closing while the request goes out, since a message the
// parser holds may end at that close; with none there is no answer.
static void early_resp_eof(chttp_early_resp *e, chttp_parse_ctx *pctx) {
  e->stop_send = true;
  if (!e->parser_live || e->done) return;
  chttp1_errno fe = chttp1_parser_finish(&e->parser);
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

// Sends data[0..len) like chttp_send_all while taking in a response that
// arrives meanwhile, so an early 401 or 413 isn't lost to a reset. It polls for
// input in the same poll as for room. Gives ccol_success when the send
// completes or the rules stop it; *sent_out is the number of bytes that went
// out.
static ccol_retval chttp_send_all_watch(chttp_conn *conn, const char *data,
                                        size_t len, chttp_deadline *overall,
                                        chttp_parse_ctx *pctx,
                                        chttp_early_resp *e,
                                        bool *any_bytes_read_out,
                                        size_t *sent_out) {
  size_t sent = 0;
  *sent_out = 0;
  short want_events = POLLOUT;
  char buf[8192];
  bool read_blocked = false;
  while (sent < len && !e->stop_send) {
    int wait_ms;
    if (!deadline_remaining_ms(overall, &wait_ms) ||
        !conn_inactivity_left_ms(conn, &wait_ms))
      return ccol_timed_out;
    bool readable =
        conn->tls && !read_blocked && ctls_conn_has_pending_input(conn->tls);
    bool writable = false;
    if (!readable) {
#ifdef RUNNING_UNIT_TESTS
      if (tls_dir_note_wait((want_events & POLLOUT) != 0)) {
        writable = true;
      } else
#endif
      {
        struct pollfd pfd = {.fd = conn->fd,
                             .events = (short)(want_events | POLLIN),
                             .revents = 0};
        struct timespec wait_start;
        clock_gettime(CLOCK_MONOTONIC, &wait_start);
        int rc = poll(&pfd, 1, wait_ms);
        conn_inactivity_charge(conn, &wait_start);
        if (rc < 0) {
          if (errno == EINTR) continue;
          return ccol_http_transfer_aborted;
        }
        if (rc == 0) continue;
        if (pfd.revents & POLLNVAL) return ccol_http_transfer_aborted;
        readable = (pfd.revents & POLLIN) != 0;
        writable = (pfd.revents & (POLLOUT | POLLERR | POLLHUP)) != 0 ||
                   (readable && want_events == POLLIN);
      }
    }

    if (readable) {
      ssize_t n = conn_read(conn, buf, sizeof(buf));
      if (n > 0) {
        read_blocked = false;
        *any_bytes_read_out = true;
        early_resp_feed(e, pctx, buf, (size_t)n);
        e->answered_early = e->done || e->parser_live;
        continue;
      }
      if (n == 0) {
        read_blocked = false;
        early_resp_eof(e, pctx);
        continue;
      }
      if (errno != EWOULDBLOCK) {
        e->stop_send = true;
        continue;
      }
      read_blocked = true;
      if (!writable) continue;
    }

    ssize_t n = conn_write(conn, data + sent, len - sent);
    if (n < 0) {
      if (errno == EWOULDBLOCK) {
        if (conn->tls)
          want_events = conn_tls_wants_write(conn->tls) ? POLLOUT : POLLIN;
        continue;
      }
      if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
        e->stop_send = true;
        continue;
      }
      return ccol_http_transfer_aborted;
    }
    if (n == 0) {
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

// Reads the rest of the response once chttp_send_all_watch is done, the message
// its parser holds or the next one. request_complete false means the connection
// is never reused.
static ccol_retval chttp_early_resp_finish(
    chttp_conn *conn, chttp_parse_ctx *pctx, chttp_deadline *overall,
    chttp_early_resp *e, bool request_complete, bool *keep_alive_out,
    bool *any_bytes_read_out) {
  ccol_retval rv;
  if (e->done) {
    *keep_alive_out = e->keep_alive;
    rv = ccol_success;
  } else if (e->parser_live) {
    char *leftover = NULL;
    size_t leftover_len = 0;
    rv = chttp_read_message_with(&e->parser, conn, pctx, overall, NULL, NULL, 0,
                                 keep_alive_out, any_bytes_read_out, &leftover,
                                 &leftover_len);
    chttp1_parser_release(&e->parser);
    e->parser_live = false;
    if (rv == ccol_success && pctx->status_code >= 100 &&
        pctx->status_code < 200) {
      if (e->n_discarded >= CHTTP_MAX_INTERIM_RESPONSES) {
        rv = ccol_http_transfer_aborted;
      } else {
        rv = parse_ctx_reset_for_continue(pctx);
      }
      if (rv == ccol_success) {
        char *next_leftover = NULL;
        size_t next_leftover_len = 0;
        rv = chttp_read_message_loop(conn, pctx, overall, NULL, leftover,
                                     leftover_len, 0, e->n_discarded + 1,
                                     keep_alive_out, any_bytes_read_out,
                                     &next_leftover, &next_leftover_len);
        ccol__mem_free(pctx->mp, leftover);
        leftover = next_leftover;
        leftover_len = next_leftover_len;
      }
    }
    if (rv == ccol_success && leftover_len > 0) {
      pctx->trailing_garbage = true;
      *keep_alive_out = false;
    }
    ccol__mem_free(pctx->mp, leftover);
  } else {
    char *leftover = NULL;
    size_t leftover_len = 0;
    rv = chttp_read_message_loop(conn, pctx, overall, NULL, NULL, 0, 0,
                                 e->n_discarded, keep_alive_out,
                                 any_bytes_read_out, &leftover, &leftover_len);
    if (rv == ccol_success && leftover_len > 0) {
      pctx->trailing_garbage = true;
      *keep_alive_out = false;
    }
    ccol__mem_free(pctx->mp, leftover);
  }
  if (rv == ccol_success && !request_complete) *keep_alive_out = false;
  return rv;
}

// Sends wire (the serialized request, body last) and reads the response. With
// use_100_continue the header block goes first and the body waits up to
// CHTTP_100_CONTINUE_WAIT_US for an answer; only the first byte of a message is
// bounded by that window. retry_unsafe is set once a request that isn't
// replayable went out in full, or a body went after a "100 Continue", so the
// caller never resends it.
static ccol_retval chttp_send_and_read(
    chttp_conn *conn, const char *wire, size_t wire_len, size_t body_len,
    bool use_100_continue, bool replayable, chttp_deadline *overall,
    chttp_parse_ctx *pctx, bool *keep_alive_out, bool *any_bytes_read_out,
    bool *retry_unsafe_out) {
  *retry_unsafe_out = false;
  chttp_early_resp early;
  early_resp_init(&early);
  size_t sent = 0;
  ccol_retval prv;
  if (!use_100_continue) {
    prv = chttp_send_all_watch(conn, wire, wire_len, overall, pctx, &early,
                               any_bytes_read_out, &sent);
    if (!replayable && sent == wire_len) *retry_unsafe_out = true;
    if (prv == ccol_success)
      prv = chttp_early_resp_finish(conn, pctx, overall, &early,
                                    !early.answered_early && sent == wire_len,
                                    keep_alive_out, any_bytes_read_out);
    early_resp_release(&early);
    return prv;
  }

  size_t header_len = wire_len - body_len;
  prv = chttp_send_all(conn, wire, header_len, overall);
  if (prv != ccol_success) return prv;

#ifdef RUNNING_UNIT_TESTS
  __atomic_add_fetch(&chttp_tier1_continue_waits, 1, __ATOMIC_RELAXED);
#endif
  chttp_deadline continue_dl = deadline_make(CHTTP_100_CONTINUE_WAIT_US);
  chttp_deadline wait_dl = deadline_earlier(continue_dl, *overall);

  char *leftover = NULL;
  size_t leftover_len = 0;
  prv = chttp_read_message_loop(conn, pctx, overall, &wait_dl, NULL, 0, 100, 0,
                                keep_alive_out, any_bytes_read_out, &leftover,
                                &leftover_len);
  if (prv == ccol_timed_out) {
    int overall_ms;
    if (!deadline_remaining_ms(overall, &overall_ms)) return ccol_timed_out;
    prv = chttp_send_all_watch(conn, wire + header_len, body_len, overall, pctx,
                               &early, any_bytes_read_out, &sent);
    if (!replayable && sent == body_len) *retry_unsafe_out = true;
    if (prv == ccol_success)
      prv = chttp_early_resp_finish(conn, pctx, overall, &early,
                                    !early.answered_early && sent == body_len,
                                    keep_alive_out, any_bytes_read_out);
    early_resp_release(&early);
    return prv;
  }
  if (prv != ccol_success) {
    ccol__mem_free(pctx->mp, leftover);
    return prv;
  }

  if (pctx->status_code != 100) {
    *keep_alive_out = false;
    if (leftover_len > 0) pctx->trailing_garbage = true;
    ccol__mem_free(pctx->mp, leftover);
    return ccol_success;
  }

  prv = parse_ctx_reset_for_continue(pctx);
  if (prv != ccol_success) {
    ccol__mem_free(pctx->mp, leftover);
    return prv;
  }

  *retry_unsafe_out = true;
  *any_bytes_read_out = false;
  if (leftover_len > 0) {
    *any_bytes_read_out = true;
    early_resp_feed(&early, pctx, leftover, leftover_len);
  }
  ccol__mem_free(pctx->mp, leftover);
  prv = early.error;
  if (prv == ccol_success)
    prv = chttp_send_all_watch(conn, wire + header_len, body_len, overall, pctx,
                               &early, any_bytes_read_out, &sent);
  if (prv == ccol_success)
    prv = chttp_early_resp_finish(conn, pctx, overall, &early,
                                  !early.answered_early && sent == body_len,
                                  keep_alive_out, any_bytes_read_out);
  early_resp_release(&early);
  return prv;
}

// Runs one Tier 1 request on cli and follows its redirects; chttpcli_do,
// chttpcli_do_streaming and the convenience calls all end here. It sets
// *resp_out to NULL first and anchors the overall deadline before taking a
// limiter slot, so time spent waiting for a slot counts. It pushes a call
// frame, takes a TLS snapshot under cli->lock, and per hop takes a pooled
// connection or opens one, sends and reads. A reused connection that died
// before any response byte is retried once when the request is safe to resend.
// A connection whose request asked for close is never pooled.
static ccol_retval chttp_do_internal(struct chttpclient *cli,
                                     const chttp_request *req, bool streaming,
                                     chttpcli_write_fn user_write_fn,
                                     void *user_write_ctx,
                                     chttpcli_response **resp_out,
                                     int *status_code_out) {
  if (!streaming) {
    if (!resp_out) return ccol_invalid_args;
    *resp_out = NULL;
  }
  if (!cli || !req) return ccol_invalid_args;
  if (streaming && !user_write_fn) return ccol_invalid_args;

  uint64_t request_timeout_us;
  ccol__mutex_lock(cli->lock);
  request_timeout_us = cli->request_timeout_us;
  ccol__mutex_unlock(cli->lock);
  chttp_deadline overall_dl = deadline_make(request_timeout_us);

  ccol_retval rv = slot_acquire(cli, &overall_dl);
  if (rv != ccol_success) return rv;

  ccol_memmgmt_procs *mp = cli->m_procs;

  uint64_t connect_timeout_us;
  uint64_t inactivity_timeout_us;
  bool verify_host;
  ctls_ctx *tls_ctx;
  bool tls_ctx_usable;
  size_t max_response_body_size;
  uint64_t tls_generation;
  ccol__mutex_lock(cli->lock);
  tls_ctx_retry_if_oom_locked(cli);
  connect_timeout_us = cli->connect_timeout_us;
  inactivity_timeout_us = cli->inactivity_timeout_us;
  tls_ctx = cli->tls_ctx;
  tls_ctx_usable = cli->tls_ctx_usable;
  bool tls_ctx_oom = cli->tls_ctx_oom;
  tls_generation = cli->tls_generation;
  max_response_body_size = cli->max_response_body_size;
  verify_host =
      !cli->tls.insecure_skip_verify && !cli->tls.insecure_skip_hostname_check;
  if (tls_ctx) ctls_ctx_retain(tls_ctx);
  ccol__mutex_unlock(cli->lock);

  char *cur_url = ccol_strdup(mp, req->url);
  if (!cur_url) {
    if (tls_ctx) ctls_ctx_release(tls_ctx);
    slot_release(cli);
    return ccol_not_enough_memory;
  }

  chttp_request_body empty_body = CHTTP_NO_BODY;
  chttp_method cur_method = req->method;
  chttp_request_body cur_body = req->body;

  char *carried_auth = NULL;
  char *carried_auth_origin = NULL;

  char *initial_origin_key = NULL;
  unsigned redirect_drops = 0;

  ccol_retval result = ccol_http_too_many_redirects;

  for (int hop = 0; hop <= CHTTP_MAX_REDIRECTS; hop++) {
    chttp_url url;
    ccol_retval prv = parse_chttp_url(mp, cur_url, &url);
    if (prv != ccol_success) {
      result = prv;
      break;
    }

    if (!initial_origin_key) {
      initial_origin_key = ccol_strdup(mp, url.origin_key);
      if (!initial_origin_key) {
        url_free(mp, &url);
        result = ccol_not_enough_memory;
        break;
      }
    } else {
      redirect_drops =
          redirect_header_drops(initial_origin_key, url.origin_key);
    }

    if (url.is_https && !tls_ctx_usable) {
      url_free(mp, &url);
      result = tls_ctx_unusable_code(tls_ctx_oom);
      break;
    }

    const char *effective_auth = NULL;
    if (url.userinfo_authorization) {
      effective_auth = url.userinfo_authorization;
    } else if (carried_auth &&
               origin_keys_same(carried_auth_origin, url.origin_key)) {
      effective_auth = carried_auth;
    }
    if (url.userinfo_authorization) {
      char *na = ccol_strdup(mp, url.userinfo_authorization);
      char *no = ccol_strdup(mp, url.origin_key);
      if (!na || !no) {
        ccol__mem_free(mp, na);
        ccol__mem_free(mp, no);
        url_free(mp, &url);
        result = ccol_not_enough_memory;
        break;
      }
      ccol__mem_free(mp, carried_auth);
      ccol__mem_free(mp, carried_auth_origin);
      carried_auth = na;
      carried_auth_origin = no;
    } else if (carried_auth &&
               !origin_keys_same(carried_auth_origin, url.origin_key)) {
      ccol__mem_free(mp, carried_auth);
      ccol__mem_free(mp, carried_auth_origin);
      carried_auth = NULL;
      carried_auth_origin = NULL;
    }

    chttp_request hop_req = *req;
    hop_req.method = cur_method;
    hop_req.body = cur_body;

    char *wire = NULL;
    size_t wire_len = 0;
    chttp_header_presence hop_hp = {0};
    prv = serialize_request(mp, &hop_req, &url, effective_auth, redirect_drops,
                            &wire, &wire_len, &hop_hp);
    if (prv != ccol_success) {
      url_free(mp, &url);
      result = prv;
      break;
    }

    chttp_deadline connect_dl = deadline_make(connect_timeout_us);

    chttp_conn conn;
    bool reused = idle_pool_take(cli, url.origin_key, &conn);
    if (!reused) {
      prv = conn_open(mp, &url, url.is_https, tls_ctx, verify_host,
                      tls_generation, &connect_dl, &overall_dl, &conn);
      if (prv != ccol_success) {
        ccol__mem_free(mp, wire);
        url_free(mp, &url);
        result = prv;
        break;
      }
    }
    conn_inactivity_start(&conn, inactivity_timeout_us);

    chttp_parse_ctx pctx;
    memset(&pctx, 0, sizeof(pctx));
    pctx.mp = mp;
    pctx.is_head_request = (cur_method == CHTTP_HEAD);

    chttp_bodybuf bb;
    memset(&bb, 0, sizeof(bb));
    bb.mp = mp;
    bb.max_size = max_response_body_size;
    pctx.requested_sink_fn = streaming ? user_write_fn : sink_buffered;
    pctx.requested_sink_ctx = streaming ? user_write_ctx : &bb;

    char *herr = NULL;
    pctx.headers =
        chashmap_create_full(CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,
                             ccol_string, ccol_string, mp, NULL, NULL, &herr);
    if (!pctx.headers) {
      ccol__mem_free(mp, wire);
      conn_teardown(mp, &conn);
      url_free(mp, &url);
      result = ccol_not_enough_memory;
      break;
    }

    bool body_carrying_method =
        (cur_method == CHTTP_POST || cur_method == CHTTP_PUT ||
         cur_method == CHTTP_PATCH);
    bool has_explicit_expect = hop_hp.has_expect;
    bool use_100_continue = req->expect_continue && !has_explicit_expect &&
                            body_carrying_method && cur_body.data &&
                            cur_body.len > 0;

    bool keep_alive = false;
    bool any_bytes_read = false;
    bool retry_unsafe = false;
    bool replayable = request_is_replayable(cur_method, &hop_hp);
    prv = chttp_send_and_read(&conn, wire, wire_len, cur_body.len,
                              use_100_continue, replayable, &overall_dl, &pctx,
                              &keep_alive, &any_bytes_read, &retry_unsafe);
    if (prv != ccol_success && reused && !any_bytes_read && !retry_unsafe) {
      conn_teardown(mp, &conn);
      chttp_deadline retry_connect_dl = deadline_make(connect_timeout_us);
      prv = conn_open(mp, &url, url.is_https, tls_ctx, verify_host,
                      tls_generation, &retry_connect_dl, &overall_dl, &conn);
      if (prv == ccol_success) {
        reused = false;
        conn_inactivity_start(&conn, inactivity_timeout_us);
        prv = chttp_send_and_read(
            &conn, wire, wire_len, cur_body.len, use_100_continue, replayable,
            &overall_dl, &pctx, &keep_alive, &any_bytes_read, &retry_unsafe);
      }
    }
    ccol__mem_free(mp, wire);
    if (prv != ccol_success) {
      conn_teardown(mp, &conn);
      parse_ctx_free_fields(&pctx);
      ccol__mem_free(mp, bb.buf);
      url_free(mp, &url);
      result = prv;
      break;
    }

    bool reusable =
        keep_alive && !pctx.trailing_garbage && !hop_hp.request_close;
    if (reusable) {
      idle_pool_offer(cli, &conn);
    } else {
      conn_teardown(mp, &conn);
    }

    if (pctx.will_redirect) {
      if (hop >= CHTTP_MAX_REDIRECTS) {
        parse_ctx_free_fields(&pctx);
        ccol__mem_free(mp, bb.buf);
        url_free(mp, &url);
        result = ccol_http_too_many_redirects;
        break;
      }
      ccol_retval resolve_rv = ccol_success;
      char *next_url =
          resolve_redirect_url(mp, &url, pctx.location, &resolve_rv);
      bool transport_refused = false;
      ccol_retval transport_rv =
          next_url
              ? redirect_transport_allowed(
                    mp, &url, next_url, req->prevent_tls_downgrade_on_redirect)
              : ccol_success;
      if (transport_rv != ccol_success) {
        ccol__mem_free(mp, next_url);
        next_url = NULL;
        transport_refused = true;
      }
      bool preserve = (pctx.status_code == 307 || pctx.status_code == 308);
      parse_ctx_free_fields(&pctx);
      ccol__mem_free(mp, bb.buf);
      url_free(mp, &url);
      ccol__mem_free(mp, cur_url);
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

    if (streaming) {
      if (status_code_out) *status_code_out = pctx.status_code;
      parse_ctx_free_fields(&pctx);
      url_free(mp, &url);
      result = ccol_success;
      break;
    }

    chttpcli_response *resp = resp_alloc(mp);
    if (!resp) {
      parse_ctx_free_fields(&pctx);
      ccol__mem_free(mp, bb.buf);
      url_free(mp, &url);
      result = ccol_not_enough_memory;
      break;
    }
    resp->status_code = pctx.status_code;
    resp->body = bb.buf;
    resp->body_len = bb.len;
    resp->headers = pctx.headers;
    pctx.headers = NULL;
    resp->ccol__field_lines = pctx.field_lines;
    pctx.field_lines = NULL;
    parse_ctx_free_fields(&pctx);
    url_free(mp, &url);
    *resp_out = resp;
    result = ccol_success;
    break;
  }

  ccol__mem_free(mp, cur_url);
  ccol__mem_free(mp, carried_auth);
  ccol__mem_free(mp, carried_auth_origin);
  ccol__mem_free(mp, initial_origin_key);
  if (tls_ctx) ctls_ctx_release(tls_ctx);
  slot_release(cli);
  return result;
}

// Runs a buffered request on h synchronously (Tier 1), the plain blocking call.
// *resp_out is NULL before anything can fail, even for a stale handle.
ccol_retval chttpcli_do(chttpcli h, const chttp_request *req,
                        chttpcli_response **resp_out) {
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;
  struct chttpclient *raw = chttpcli_resolve(h);
  if (!raw) return ccol_invalid_args;
  chttpcli_call_frame frame;
  chttpcli_call_push(&frame, raw, false);
  ccol_retval rv =
      chttp_do_internal(raw, req, false, NULL, NULL, resp_out, NULL);
  chttpcli_call_pop(&frame);
  chttpcli_resolve_unpin(raw);
  return rv;
}

// Runs a streaming request on h synchronously, so a large body needs no buffer:
// the body goes to write_fn and *status_code_out gets the final status.
ccol_retval chttpcli_do_streaming(chttpcli h, const chttp_request *req,
                                  chttpcli_write_fn write_fn, void *write_ctx,
                                  int *status_code_out) {
  struct chttpclient *raw = chttpcli_resolve(h);
  if (!raw) return ccol_invalid_args;
  chttpcli_call_frame frame;
  chttpcli_call_push(&frame, raw, false);
  ccol_retval rv = chttp_do_internal(raw, req, true, write_fn, write_ctx, NULL,
                                     status_code_out);
  chttpcli_call_pop(&frame);
  chttpcli_resolve_unpin(raw);
  return rv;
}

// Default client
//
// Creates the default client once, for chttp_default_client; client_at_exit
// destroys it before OpenSSL's cleanup.
static void init_default_client(void) {
  ccol__call_once(cli_engine_bundler.exit_hook_once, client_exit_hook_install);
  atomic_store(&default_client_bundler.client, chttpcli_create(NULL));
}

// Destroys the default client from client_at_exit, so its threads are gone
// before OpenSSL's cleanup, unless a Tier 1 call runs on it (it may be the call
// that called exit()). chttp_default_client gives CHTTPCLI_NULL afterwards.
static void default_client_destroy_if_idle(void) {
  chttpcli h = atomic_load(&default_client_bundler.client);
  if (!h) return;
  struct chttpclient *raw = chttpcli_resolve(h);
  if (!raw) return;
  ccol__mutex_lock(raw->lock);
  bool busy = raw->in_flight_count > 0;
  ccol__mutex_unlock(raw->lock);
  chttpcli_resolve_unpin(raw);
  if (busy) return;
  if (atomic_compare_exchange_strong(&default_client_bundler.client, &h, 0))
    chttpcli__destroy(h);
}

// Gives the process's default client, created on first use, for the convenience
// calls and for callers that need no client of their own.
chttpcli chttp_default_client(void) {
  ccol__call_once(default_client_bundler.once, init_default_client);
  return atomic_load(&default_client_bundler.client);
}

// At unload or exit, clears the default client handle and then destroys the
// client (so the defensive compare-and-swap in destroy can't race), and frees
// the slot table when no client is left, or leaves that to the last destroy.
__attribute__((destructor)) static void cleanup_default_client(void) {
  chttpcli cli = atomic_exchange(&default_client_bundler.client, 0);
  if (cli) chttpcli__destroy(cli);

  ccol__call_once(chttpcli_slot_table.once, chttpcli_slot_table_init_globals);
  ccol__rw_lock_wrlock(chttpcli_slot_table.rwlock);
  if (chttpcli_any_slot_live_locked()) {
    chttpcli_slot_table.release_deferred = true;
  } else {
    chttpcli_release_slot_table_locked();
  }
  ccol__rw_lock_unlock(chttpcli_slot_table.rwlock);
}

// Tells whether any slot names a client. It reads ptr and not in_use, because a
// destroy clears in_use first and still needs the table until its last step.
// The caller holds the write lock.
static bool chttpcli_any_slot_live_locked(void) {
  size_t slot_count = cvector_elem_count(chttpcli_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++) {
    chttpcli_slot *slot =
        (chttpcli_slot *)cvector_at(chttpcli_slot_table.slots, i);
    if (slot->ptr != NULL) return true;
  }
  return false;
}

// Releases the slot table if the exit destructor deferred that and no slot is
// live any more; the last destroy calls it. Out of line, so the destroy path
// keeps its shape. The caller holds the write lock.
static __attribute__((noinline)) void
chttpcli_release_slot_table_if_deferred_locked(void) {
  if (chttpcli_slot_table.release_deferred &&
      !chttpcli_any_slot_live_locked()) {
    chttpcli_release_slot_table_locked();
  }
}

// Frees the bookkeeping of the slot table and the pin index, setting each to
// NULL so a later call sees it released, at exit or after the last destroy. The
// caller holds the write lock and no slot is live.
static void chttpcli_release_slot_table_locked(void) {
  cvector_destroy(chttpcli_slot_table.slots);
  cvector_destroy(chttpcli_slot_table.free_indices);
  ccol_pintable_dispose(&chttpcli_pintable);
  chttpcli_slot_table.release_deferred = false;
}

// Convenience API
//
// Runs req on the default client, for callers that need no client of their own.
// *resp_out is set to NULL first.
ccol_retval chttp_do(const chttp_request *req, chttpcli_response **resp_out) {
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;
  chttpcli cli = chttp_default_client();
  if (!cli) return ccol_unexpected_failure;
  return chttpcli_do(cli, req, resp_out);
}

// Builds a request from the arguments and runs it on the default client. A bad
// method or an inconsistent body is ccol_invalid_args before the create, so a
// failed create means only a failed allocation.
ccol_retval chttp_run_query(chttp_method method, const char *url,
                            const chttp_request_body *body, chmap headers,
                            chttpcli_response **resp_out) {
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;
  if (!url) return ccol_invalid_args;
  if ((unsigned)method >= (unsigned)CHTTP_ANY) return ccol_invalid_args;
  if (body && !body->data && body->len > 0) return ccol_invalid_args;
  chttp_request *req = chttp_request_create(method, url, body, NULL);
  if (!req) return ccol_not_enough_memory;

  req->headers = headers;
  ccol_retval rv = chttp_do(req, resp_out);
  req->headers = NULL;
  chttp_request_free(req);
  return rv;
}

// Run a GET, POST, PUT, DELETE or PATCH on the default client through
// chttp_run_query, the one-line forms of the most common requests.
ccol_retval chttp_get(const char *url, chttpcli_response **resp_out) {
  return chttp_run_query(CHTTP_GET, url, NULL, NULL, resp_out);
}

ccol_retval chttp_post(const char *url, const chttp_request_body *body,
                       chttpcli_response **resp_out) {
  return chttp_run_query(CHTTP_POST, url, body, NULL, resp_out);
}

ccol_retval chttp_put(const char *url, const chttp_request_body *body,
                      chttpcli_response **resp_out) {
  return chttp_run_query(CHTTP_PUT, url, body, NULL, resp_out);
}

ccol_retval chttp_delete(const char *url, chttpcli_response **resp_out) {
  return chttp_run_query(CHTTP_DELETE, url, NULL, NULL, resp_out);
}

ccol_retval chttp_patch(const char *url, const chttp_request_body *body,
                        chttpcli_response **resp_out) {
  return chttp_run_query(CHTTP_PATCH, url, body, NULL, resp_out);
}

// Response API
//
// The stack buffer size for the response header lookups (as for requests).
enum { CHTTP_RESP_NAME_STACK_BUF = CHTTP_NAME_STACK_BUF };
// Lower-cases name with resp's procs for the response header accessors, on the
// stack when it fits, so looking up known names on every response allocates
// nothing. Gives the buffer or NULL.
static char *resp_lower_name(const chttpcli_response *resp, const char *name,
                             char *stack_buf, size_t *len_out) {
  return lower_name_copy(resp->ccol__m_procs, name, stack_buf, len_out);
}

// Looks up the lower-case name in resp's header map, for the header accessors.
static const char *resp_map_value(const chttpcli_response *resp,
                                  const char *lower, size_t nlen) {
  cmap_pair kp = {.ptr = (void *)lower, .size = nlen + 1};
  cmap_pair vp;
  if (chashmap_get_elem_ref((chmap)resp->headers, &kp, &vp) != ccol_success)
    return NULL;
  return (const char *)vp.ptr;
}

// Finds occurrence index of the lower-case name and counts all occurrences into
// *count_out (may be NULL), for the header accessors. A repeated name has
// records in the field lines; a name that appears once has only its map entry.
static const char *resp_occurrence(const chttpcli_response *resp,
                                   const char *lower, size_t nlen, size_t index,
                                   size_t *count_out) {
  const chttp_field_lines *lines =
      (const chttp_field_lines *)resp->ccol__field_lines;
  size_t seen = 0;
  const char *found = NULL;
  const chttp_field_slot *slot = field_lines_find(lines, lower, nlen);
  for (uint32_t o = slot ? slot->head : 0; o;) {
    chttp_field_rec r = field_rec_at(lines, o - 1);
    if (seen == index) {
      found = field_rec_value(lines, o - 1, &r);
      if (!count_out) return found;
    }
    seen++;
    o = r.next;
  }
  if (seen == 0) {
    const char *v = resp_map_value(resp, lower, nlen);
    seen = v ? 1 : 0;
    found = (v && index == 0) ? v : NULL;
  }
  if (count_out) *count_out = seen;
  return found;
}

// Gives the value of name in resp (repeated occurrences joined by ", ",
// Set-Cookie its first), or NULL. It's the accessor for most callers.
const char *chttpcli_resp_header(const chttpcli_response *resp,
                                 const char *name) {
  if (!resp || !name || !resp->headers) return NULL;
  char stack_lower[CHTTP_RESP_NAME_STACK_BUF];
  size_t nlen;
  char *lower = resp_lower_name(resp, name, stack_lower, &nlen);
  if (!lower) return NULL;
  const char *v = resp_map_value(resp, lower, nlen);
  if (lower != stack_lower) ccol__mem_free(resp->ccol__m_procs, lower);
  return v;
}

// Gives how many times name appears in resp, so a caller can walk a repeated
// field.
size_t chttpcli_resp_header_count(const chttpcli_response *resp,
                                  const char *name) {
  if (!resp || !name || !resp->headers) return 0;
  char stack_lower[CHTTP_RESP_NAME_STACK_BUF];
  size_t nlen;
  char *lower = resp_lower_name(resp, name, stack_lower, &nlen);
  if (!lower) return 0;
  size_t count = 0;
  (void)resp_occurrence(resp, lower, nlen, 0, &count);
  if (lower != stack_lower) ccol__mem_free(resp->ccol__m_procs, lower);
  return count;
}

// Gives occurrence index of name in resp, or NULL, for walking a repeated field
// such as Set-Cookie.
const char *chttpcli_resp_header_at(const chttpcli_response *resp,
                                    const char *name, size_t index) {
  if (!resp || !name || !resp->headers) return NULL;
  char stack_lower[CHTTP_RESP_NAME_STACK_BUF];
  size_t nlen;
  char *lower = resp_lower_name(resp, name, stack_lower, &nlen);
  if (!lower) return NULL;
  const char *v = resp_occurrence(resp, lower, nlen, index, NULL);
  if (lower != stack_lower) ccol__mem_free(resp->ccol__m_procs, lower);
  return v;
}

// Frees a response through the procs copy it carries, so it may be freed after
// its client is gone.
void chttpcli_resp_free(chttpcli_response *resp) {
  if (!resp) return;
  ccol_memmgmt_procs procs;
  ccol_memmgmt_procs *mp = NULL;
  if (resp->ccol__m_procs) {
    procs = *resp->ccol__m_procs;
    mp = &procs;
  }
  ccol__mem_free(mp, resp->body);
  if (resp->headers) chashmap__destroy((chmap)resp->headers);
  field_lines_free(mp, (chttp_field_lines *)resp->ccol__field_lines);
  ccol__mem_free(mp, resp);
}
