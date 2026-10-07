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
#include <internal/cdeadline.h>
#include <internal/chashinsert.h>
#include <internal/chttp1_parser.h>
#include <internal/cpintable.h>
#include <internal/csock.h>
#include <internal/cstrutil.h>
#include <internal/ctls.h>
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
#include <strings.h>
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
/* This bounds the number of DISTINCT origin keys that the chmap of the idle
 * pool can hold an entry for. It is independent of CHTTP_MAX_IDLE_TOTAL and
 * CHTTP_MAX_IDLE_PER_ORIGIN above. Those two bound the live pooled
 * CONNECTIONS. They never bound the number of distinct origins that had one.
 * Some clients contact many different origins and never go back to most of
 * them. A crawler is one example. An outbound-heavy service that fans out to
 * many hosts is another. Without this cap, such a client keeps one permanent
 * chmap entry for each origin that it ever saw. The pool of an origin can
 * later become capped out or aged out. No other trigger then removes the
 * entry of that origin. By that time the entry is empty, or the client never
 * goes back to it. The reuse-triggered prune in _idle_pool_take and
 * _async_idle_pool_take frees the entry of an origin under one condition
 * only. The client must go back to THAT origin and must drain its pool to
 * zero. That never happens for an origin that the client visits exactly
 * once. After the cap is full, the library does not pool a connection for a
 * genuinely new origin. It does not grow the map further. This matches the
 * policy that this file uses for the other two caps: if it does not fit, do
 * not pool it. This is a lost optimisation, never a correctness problem. */
#define CHTTP_MAX_IDLE_ORIGINS 128
#ifdef RUNNING_UNIT_TESTS
/* This overrides the effective value of CHTTP_MAX_IDLE_ORIGINS for a test.
 * A value of 0 means that the library uses the real constant. A direct test
 * of the real cap of 128 needs 128 genuinely distinct origins. That is not
 * practical for an ordinary functional test. A test therefore makes this
 * value small. _chttpclient_set_max_idle_origins_for_tests sets it. That
 * function is near the other white-box test helpers. */
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
/* This is the time that chttp_do_internal waits for a "100 Continue"
 * interim response. The server can also answer directly with its real final
 * response. After this time, chttp_do_internal stops the wait and sends the
 * body anyway. The value matches the default of curl's own
 * CURLOPT_EXPECT_100_TIMEOUT_MS. See the doc comment of
 * chttp_request_t.expect_continue. */
#define CHTTP_100_CONTINUE_WAIT_US UINT64_C(1000000)
/* The Connection Attempt Delay of Happy Eyeballs (RFC 8305 section 5). A
 * connect to a host with several addresses starts the next address when the
 * attempts in flight have produced no result for this long. It is the
 * value that the RFC recommends and that Go's net.Dialer uses. */
#define CHTTP_CONNECT_ATTEMPT_DELAY_MS 250L
#ifdef RUNNING_UNIT_TESTS
/* While this is not 0, it replaces CHTTP_CONNECT_ATTEMPT_DELAY_MS, so a test
 * can tell a hand-over that waits for the delay from one that does not by a
 * margin that no instrumentation can close. */
static _Atomic long g_connect_attempt_delay_ms_for_tests = 0;
/* While this is above 0, each attempt delay that starts takes one from it
 * and lasts 1 ms, so a test can start a second attempt at once and still
 * give that second attempt the long delay above. */
static _Atomic int g_connect_short_delays_for_tests = 0;
/* While this is above 0, Tier 1 takes one from it for each attempt in
 * progress that completes, and treats that completion as ECONNREFUSED: an
 * attempt that fails after the connect started, as a refusal from a remote
 * host does. A loopback connect can complete inside connect(2); while this
 * is above 0, such an attempt also counts as in progress, so that the hook
 * sees its completion. */
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

/* Takes one from *counter when it is above 0, and says whether it did. */
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
/* This is a soft cap on the number of interim responses that
 * _chttp_read_message_loop discards without a report to the caller. These
 * are 1xx responses other than the one that the loop waits for. A bad or
 * malicious server can send interim responses and never stop. An endless
 * stream of "103 Early Hints" is one example. Without this cap, such a
 * server can pin a caller thread and its slot in the concurrency limiter
 * forever. The default configuration has request_timeout_us == 0, which
 * means no timeout. Every other place in this file that can repeat without
 * a bound already has a cap. Redirects, and the header count and byte
 * budgets of chttp1_parser, are the other places. This cap does the same
 * for this loop. */
#define CHTTP_MAX_INTERIM_RESPONSES 64

/* ========================================================================== */
/*                         INTERNAL TYPES                                     */
/* ========================================================================== */

/* This holds a parsed request-target. All fields are owned copies that come
 * from the mp allocator.
 *
 * A "http+unix://" URL sets is_unix and unix_socket_path in place of host,
 * port and is_ipv6. See _parse_chttp_url. For a unix target, host stays
 * NULL, port stays 0, and is_ipv6 stays false. The library always fills
 * origin_key. For a unix target it uses a distinct "unix://<path>" prefix.
 * That prefix never collides with a TCP origin_key. A TCP origin_key starts
 * with "http://" or "https://". The chmaps of the idle pool can therefore
 * key a unix-socket target in the same way as a TCP one. */
typedef struct {
  bool is_https;
  bool is_ipv6; /* host is a raw IPv6 literal with no brackets. connect()
                 * and TLS need it without brackets. But the Host header,
                 * origin_key, and the rebuild of a redirect URL all need
                 * the brackets again. */
  bool is_unix;
  char *unix_socket_path; /* owned, percent-decoded; NULL unless is_unix */
  char *host;
  uint16_t port;
  char *path_and_query;
  char *origin_key; /* "scheme://host:port" or "unix://<path>". The library
                     * uses it for SNI (TCP only) and as the key of the idle
                     * pool (both kinds). */
  char *userinfo_authorization; /* An owned "Basic <b64>" string that comes
                                 * from the "user:pass@" part of THIS URL.
                                 * It is NULL if the URL has no such part. */
} chttp_url_t;

/* A helper for a deadline in absolute time. `active == false` means that
 * there is no limit. */
typedef struct {
  bool active;
  struct timespec deadline;
} chttp_deadline_t;

/* One connection, which the idle pool can hold. The library always keeps it
 * as a value on the stack or inside another object. It never allocates one
 * on the heap by itself. The cvector of the idle pool can therefore store it
 * by value, with no extra allocation layer. */
typedef struct {
  int fd;
  ctls_conn_t *tls; /* NULL for plain HTTP */
  char *origin_key; /* owned copy, matches chttp_url_t.origin_key */
  struct timespec last_used;
  /* The value of chttpclient.tls_generation at the time of the handshake of
   * this connection. The idle pool uses the origin alone as its key. The
   * origin says nothing about the TLS policy of a pooled connection. A TLS
   * connection is therefore reusable only while this value still matches the
   * current generation of its client. See _idle_pool_take. This field has no
   * meaning for a plaintext connection, and the library never reads it
   * there. */
  uint64_t tls_generation;
} chttp_conn_t;

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  ccol_memmgmt_procs_t *mp;
  bool oom;
  /* max_size == 0 means no limit. That is the default of
   * chttpclient_set_max_response_body_size. The library checks this value on
   * every append. The first append that would push len past max_size sets
   * too_large and does not grow the buffer. The oversized body of a
   * malicious or bad server is therefore bounded long before the buffer
   * holds all of it. This field has meaning only for the buffered sink. A
   * caller that streams the body controls its own memory with the return
   * value of chttpcli_write_fn. */
  size_t max_size;
  bool too_large;
} chttp_bodybuf_t;

/* Every occurrence of each field name that one response header block holds
 * more than once, in the order of the wire. data holds one record for each
 * occurrence: a chttp_field_rec_t, then "name\0value\0" with the name in
 * lower case, padded to a multiple of 4 bytes. A name that occurs once has
 * no record at all: the header map holds its one value, and a response
 * without a repeated name allocates none of this.
 *
 * The records of one name form a list in the order of the wire, through
 * `next`. index is an open-addressing table with one slot for each repeated
 * name, which holds the first and the last record of its list. Adding an
 * occurrence, combining every name and looking one up therefore each cost
 * time in proportion to the records involved, and never a rescan of the
 * block. Offsets are stored plus one, so that zero means "none"; the caps of
 * the parser keep every offset far below UINT32_MAX. */
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

/* This drives the parse of one HTTP/1.1 response, which is one hop. The
 * library uses a new instance for every hop of a redirect chain. The headers
 * of an intermediate redirect therefore never leak into the final response.
 * There is no shared header map that the library resets in place, so there
 * is nothing to leak from. */
typedef struct {
  ccol_memmgmt_procs_t *mp;
  chmap headers; /* chmap(char* -> char*). This struct owns it until the
                  * library transfers it or destroys it. */
  chttp_field_lines_t *field_lines; /* Owned in the same way as headers.
                                     * NULL until a name repeats. */

  bool is_head_request;
  bool will_redirect;
  char *location; /* Owned. The library sets it only when will_redirect is
                   * true. */

  bool message_complete;
  bool trailing_garbage;
  bool error;     /* An allocation failed inside a callback. */
  bool aborted;   /* sink_fn returned a short count, which means that the
                   * caller that streams the body stopped the transfer. */
  bool too_large; /* The buffered body went past the cap that
                   * chttpclient_set_max_response_body_size sets.
                   * _on_headers_complete rejects a declared Content-Length
                   * at the start. _sink_buffered rejects the accumulated
                   * body as it arrives. The library never sets this flag
                   * for a request that streams its body, because such a
                   * request has no cap. See the comment of
                   * chttp_bodybuf_t.max_size. */
  int status_code;

  chttpcli_write_fn requested_sink_fn;
  void *requested_sink_ctx;
  chttpcli_write_fn sink_fn; /* The library resolves this after the headers
                              * are complete. */
  void *sink_ctx;
} chttp_parse_ctx_t;

/* ========================================================================== */
/*                         CHTTPCLI HANDLE SLOT TABLE                         */
/* ========================================================================== */

/* chttpcli is an opaque value handle. The top 32 bits are the slot index and
 * the bottom 32 bits are the generation. See the doc comment on the typedef
 * in include/chttpclient.h. The library resolves the handle through this
 * table before it touches the underlying struct chttpclient*. This lets
 * __chttpclient_destroy report two errors with ccol_fatal_err in place of a
 * use-after-free or a double free. The first error is a concurrent double
 * destroy, which races another destroy on the same still-live handle. The
 * second is a sequential one, which is a stale handle from an earlier
 * destroy that already finished. The library marks a slot not-in-use at the
 * moment that it releases the slot. It also raises the generation of the
 * slot on every reuse. A stale handle can therefore never alias a later,
 * unrelated client that occupies the same slot index.
 *
 * The lock of this table is a read-write lock and not a plain mutex.
 * _chttpcli_resolve is read-only: it bounds-checks idx, compares the
 * generation, and reads slot->ptr. It runs once for each outbound request.
 * This is true in every API tier, such as chttpclient_do, _do_async and
 * _do_pooled. _chttpcli_handle_slot_acquire and __chttpclient_destroy are
 * the only writers. Each of them runs once for the whole lifetime of a
 * client, not once for each request. This matches the identical slot-table
 * read-write locks in cthreadcomm.c and cthreadpool.c. But the fork
 * handlers of this module do not take this lock: only an application thread
 * that creates or destroys a client holds it, and fork(2) with a client
 * created before the fork is not a supported pattern. Those two files track
 * the TID of the write-lock holder and re-initialize the lock in the child.
 * None of that machinery applies here. */
typedef struct {
  struct chttpclient *ptr; /* NULL when the slot is free */
  uint32_t generation;     /* The library mints a new value on every
                               acquire. The value only grows for each slot
                               index. It starts at 0 before the first use,
                               and becomes 1 on the first acquire. */
  bool in_use;
} chttpcli_slot_t;

static struct {
  ccol_rw_lock_t rwlock;
  ccol_once_flag_t once;
  cvec slots;        /* A cvec of chttpcli_slot_t. It grows only with
                         push_back. An index is permanent after the library
                         allocates it. */
  cvec free_indices; /* A cvec of uint32_t. It is a LIFO free list that
                         gives O(1) reuse. */
  /* The library sets this when the process-exit destructor finds a client
     that is still live and leaves this table alone. The destroy that then
     releases the last slot does the release that the destructor could not
     do. The library reads and writes this field only under the write
     lock. */
  bool release_deferred;
} chttpcli_slot_table = {0};

/* These are defined with the process-exit teardown below. They are declared
   here because the final locked section of __chttpclient_destroy does the
   release that the destructor left for it. */
static bool _chttpcli_any_slot_live_locked(void);
static void _chttpcli_release_slot_table_locked(void);
static void _chttpcli_release_slot_table_if_deferred_locked(void);

/* The hot half of the table above. It maps a handle to a pointer. It also
 * holds the pin that keeps a client alive for the length of a call. It is
 * separate because a resolve runs on every request. A resolve must not write
 * anything that another thread reads. Everything else that this table does
 * is cold and stays under the read-write lock. Slot reuse and the sweep at
 * exit time are the cold parts. */
static ccol_pintable chttpcli_pintable;

static void _chttpcli_slot_table_init_globals(void) {
  if (ccol_rw_lock_init(chttpcli_slot_table.rwlock) != 0)
    ccol_fatal_err("chttpcli slot table: failed to initialize rwlock");
  chttpcli_slot_table.slots = cvector_create(sizeof(chttpcli_slot_t), NULL);
  if (!chttpcli_slot_table.slots)
    ccol_fatal_err("chttpcli slot table: failed to allocate slots vector");
  chttpcli_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!chttpcli_slot_table.free_indices)
    ccol_fatal_err("chttpcli slot table: failed to allocate free-index vector");
}

struct chttpclient {
  ccol_mutex_t lock;
  ccol_cond_var_t available;

  /* The concurrency limiter. It bounds the number of requests that are in
   * flight at the same time. */
  size_t pool_cap;
  size_t configured_pool_size;
  size_t in_flight_count;
  bool pool_initialized;
  bool destroying;

  /* The keep-alive idle pool. It is a chmap(char *origin_key -> cvec of
   * chttp_conn_t). It is independent of the concurrency limiter above. */
  chmap idle_pools;
  size_t idle_total_count;

  /* The keep-alive idle pool of Tier 2. It is a chmap(char *origin_key ->
   * cvec of chttp_async_ctx_t*). It is separate from idle_pools above,
   * because the stored value type is different. idle_pools holds a plain fd
   * that this thread owns. This pool holds a connection that is attached to
   * the shared async reactor. See the "ASYNC IDLE POOL" section further
   * down for the full design. The library broadcasts idle_async_drained
   * whenever idle_total_count_async reaches 0. __chttpclient_destroy can
   * therefore wait for every in-flight teardown of an idle connection.
   * It waits for each one to finish before it frees this struct. Its own
   * drain starts some of those teardowns. A natural death of a connection
   * starts the others. */
  chmap idle_pools_async;
  size_t idle_total_count_async;
  ccol_cond_var_t idle_async_drained;

  /* This counts the active Tier 2 and Tier 3 async chains of THIS client.
   * An active chain is one that the idle pool does not hold yet.
   * _async_chain_create adds one for each chain. _async_chain_release
   * subtracts one after the refcount of a chain reaches zero. At that
   * point the library tears the last hop of the chain down, or that hop
   * joins the idle pool above. This count is independent of
   * in_flight_count above, which covers Tier 1 only. It is also
   * independent of idle_total_count_async, because an idle pooled
   * connection is the OPPOSITE of an in-flight one.
   *
   * A DEDICATED leaf lock and condition variable guard this count. The
   * library never uses cli->lock or cli->available for it, and that choice
   * is deliberate. _async_chain_release is the only place that subtracts
   * from this count. It runs from inside a ccol_event_loop dispatch callback
   * as often as it runs from a safe synchronous context. In the callback
   * case it already holds the dispatch_lock of that registration. Use of
   * cli->lock here would add a new dispatch_lock -> cli->lock order that
   * this module otherwise never needs. That gives no benefit over a lock
   * that no code ever holds across another call.
   *
   * __chttpclient_destroy waits on async_count_drained until this count
   * reaches zero, and only then frees cli. This closes a real
   * use-after-free. Code dereferences chain->cli and ctx->cli through the
   * whole life of an active hop, for cli->lock, cli->idle_pools_async and
   * cli->m_procs. That happens long after chttpclient_do_async/_streaming
   * returns to the caller. */
  ccol_mutex_t async_count_lock;
  ccol_cond_var_t async_count_drained;
  size_t async_in_flight_count;

  uint64_t connect_timeout_us;
  uint64_t request_timeout_us;
  /* This caps the size of a buffered response body in all three tiers. A
   * value of 0 is the default and means no limit. See the doc comment of
   * chttpclient_set_max_response_body_size. The library reads it under
   * cli->lock into a snapshot for each request or each chain. That snapshot
   * is a local of chttp_do_internal, or it is
   * chttp_async_chain_t.max_response_body_size. This is the same treatment
   * that connect_timeout_us and request_timeout_us above get. The library
   * never reads this field again while a request is in flight. */
  size_t max_response_body_size;
  chttp_tls_config_t tls;
  char *owned_cert_path;
  char *owned_key_path;
  char *owned_ca_bundle_path;
  ctls_ctx_t *tls_ctx; /* The library rebuilds this on every call to
                          chttpclient_set_tls. */
  bool tls_ctx_usable; /* This is false if the library cannot read the
                          configured cert, key or ca paths. See
                          _rebuild_tls_ctx_locked. */

  ccol_memmgmt_procs_t *m_procs;

  /* The handle of this client. An unpin can therefore find the slot that
   * holds its pin, and the caller does not need to carry one. The library
   * writes this field once, before it publishes the handle, and never
   * again. This field is at the end and not among the fields above. No
   * field that a request path already reads moves. */
  chttpcli self_handle;

  /* Every call to _rebuild_tls_ctx_locked raises this value. Each TLS
   * configuration of this client therefore has its own identity. Both idle
   * pools use the origin alone as their key. The origin is
   * "scheme://host:port" or "unix://<path>". It carries nothing about the
   * TLS policy of a pooled connection. Without this field, the library
   * keeps answering over connections that it made under the old
   * configuration for up to CHTTP_IDLE_MAX_AGE_MS. A chttpclient_set_tls
   * call can pin a private CA, turn verification back on, or
   * rotate the client certificate. Every pooled TLS connection carries the
   * generation of its handshake. The library refuses to reuse it after that
   * value stops matching. The library reads and writes this field under
   * cli->lock, like every other field here. It starts at 1. A connection
   * that carries 0 never had a handshake, so it can never match. */
  uint64_t tls_generation;
};

/* ========================================================================== */
/*                    CHTTPCLI HANDLE RESOLVE / UNPIN                         */
/* ========================================================================== */

/* This resolves h and pins the result against a concurrent destroy. It
 * returns NULL if h is 0 or garbage. It also returns NULL if h names a slot
 * that is free now. It returns NULL for a slot that the library already
 * reused, because that slot gives the wrong generation. On success the
 * caller MUST call _chttpcli_resolve_unpin(result) exactly once. The caller
 * does this as soon as its own tier-specific protection takes over. That
 * protection is in_flight_count or async_in_flight_count. If the call is
 * short and does not block, the caller does it at once. */
static struct chttpclient *_chttpcli_resolve(chttpcli h) {
  /* There is no lock here and no shared write. Every public entry point of
   * this module runs this function. A write to memory that another thread
   * reads would therefore cost time on every single request. The pin index
   * is a zeroed static. A handle that arrives before the library creates any
   * client finds no chunk and resolves to NULL. That is the same answer that
   * the index gives for any handle that it does not know. */
  return (struct chttpclient *)ccol_pintable_pin(&chttpcli_pintable, h);
}

static void _chttpcli_resolve_unpin(struct chttpclient *raw) {
  /* This releases the pin and touches nothing else. A lock on this client
   * here would make every request take that lock twice. The second time
   * would come immediately after the first unlock. That is how a convoy
   * keeps itself alive when many callers run at the same time. There is also
   * no wakeup to deliver. __chttpclient_destroy polls the pin count. It does
   * not sleep on a condition variable and wait for this function.
   *
   * The pin is in the slot and not in raw. That is what makes a lock-free
   * release safe. This call dereferences no part of the object. A destroy
   * that sees the count reach zero can therefore free raw at once, and it
   * races nothing here.
   *
   * The read of raw->self_handle before the release is safe, because the pin
   * is still held at that point. After the release the library can free the
   * client at any instant. Nothing may touch raw after this call. */
  ccol_pintable_unpin(&chttpcli_pintable, raw->self_handle);
}

/* This allocates a new slot for cli, or reuses a freed one. It returns the
 * handle, or 0 if the allocation fails. ccol_create_chttpclient_mp calls it
 * once, after the rest of the object is fully constructed. */
static chttpcli _chttpcli_handle_slot_acquire(struct chttpclient *cli) {
  ccol_call_once(chttpcli_slot_table.once, _chttpcli_slot_table_init_globals);
  ccol_rw_lock_wrlock(chttpcli_slot_table.rwlock);
  uint32_t idx;
  chttpcli_slot_t *slot;
  if (cvector_elem_count(chttpcli_slot_table.free_indices) > 0) {
    cvector_pop_back(chttpcli_slot_table.free_indices, &idx);
    slot = (chttpcli_slot_t *)cvector_at(chttpcli_slot_table.slots, idx);
  } else {
    /* The library can never publish a slot whose index is beyond what the
     * pin table can hold. It therefore refuses such a slot here. It does not
     * claim the slot and then roll it back. A rollback would put an index
     * that no later publish can use onto the free list that every acquire
     * pops from. This is an ordinary failure. A caller already has to treat
     * a table that cannot grow in that way. */
    if (cvector_elem_count(chttpcli_slot_table.slots) >= CCOL_PIN_MAX_SLOTS) {
      ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
      return 0;
    }
    chttpcli_slot_t fresh = {0};
    if (cvector_push_back(chttpcli_slot_table.slots, &fresh) != ccol_success) {
      ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
      return 0; /* ordinary, non-fatal OOM */
    }
    idx = (uint32_t)cvector_elem_count(chttpcli_slot_table.slots) - 1;
    slot = (chttpcli_slot_t *)cvector_at(chttpcli_slot_table.slots, idx);
  }
  slot->generation++;
  if (slot->generation == 0)
    slot->generation++; /* This skips the one value that collides with
                         * CHTTPCLI_INVALID after about 2^32 reuses of this
                         * exact slot index. The library closes this case
                         * and leaves no residual risk. */
  chttpcli h = ((chttpcli)idx << 32) | (chttpcli)slot->generation;

  /* The library writes this before it publishes the handle. A resolver that
   * finds this client therefore also finds the handle that its own unpin
   * needs. */
  cli->self_handle = h;

  /* A publish can allocate a first-use chunk or a stripe block. A failure
   * would leave a handle that no call can resolve. The slot therefore goes
   * back on the free list. The library deliberately does not write slot->ptr
   * and slot->in_use until after the publish succeeds. This path therefore
   * has nothing to undo except the index. */
  if (!ccol_pintable_publish(&chttpcli_pintable, idx, slot->generation, cli)) {
    cli->self_handle = 0;
    cvector_push_back(chttpcli_slot_table.free_indices, &idx);
    ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
    return 0;
  }

  slot->ptr = cli;
  slot->in_use = true;
  ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
  return h;
}

/* ========================================================================== */
/*                         DEFAULT CLIENT                                     */
/* ========================================================================== */

/* client is _Atomic. __chttpclient_destroy can therefore clear it safely
 * with a compare-and-swap when the handle that it gets is this singleton.
 * See the comment on this in __chttpclient_destroy. chttp_default_client
 * reads it with a plain atomic_load. This matches the existing
 * _Atomic(ccol_event_reg) pattern of this file, and it adds no new lock for
 * one value. once never resets. This module deliberately never
 * re-initializes the default client. See the doc comment of
 * chttp_default_client. */
static struct {
  _Atomic(chttpcli) client;
  ccol_once_flag_t once;
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

/* This percent-encodes a unix socket path into a new NUL-terminated string
 * that the library allocates. The library uses that string to rebuild a
 * "http+unix://<path>" URL string. origin_key is one such string. A redirect
 * target that the library resolves against a unix-socket base is another.
 * The encoding matches what a real client library applies to any authority
 * component of a URL. It escapes every byte outside an unreserved or
 * sub-delim set. This includes '/'. The whole purpose of this scheme is to
 * pack a path that itself contains '/' into one authority component. */
/* This does the work for _percent_encode_unix_path. It takes `len` as an
 * explicit parameter and does not compute it with strlen(path). A white-box
 * test can therefore exercise the overflow guard below directly, with a
 * false `len`. The test does not need to build a path string of many
 * exabytes. See _chttp_percent_encode_unix_path_overflow_guard_for_tests
 * below. */
static char *_percent_encode_unix_path_len(ccol_memmgmt_procs_t *mp,
                                           const char *path, size_t len) {
  static const char *unreserved =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
  /* This guards the computation of the needed size below against an
   * overflow. It matches the SIZE_MAX-relative guard that the other growable
   * and percent-encoded buffers of this file already use for the same class
   * of computation. chttp_base64_encode_mp and _ob_append are those buffers.
   * Without this guard, a path long enough to make len * 3 + 1 wrap past
   * SIZE_MAX makes this function allocate too little space for `out`. The
   * loop below then writes past the end of `out`. No real input reaches this
   * guard, because it needs a path string of many exabytes in memory before
   * any call to this function. But this project treats an unguarded overflow
   * in a size computation as a real bug. The size of the input that triggers
   * it does not matter. */
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

/* This percent-encodes a unix socket path into a new NUL-terminated string
 * that the library allocates. The library uses that string to rebuild a
 * "http+unix://<path>" URL string. origin_key is one such string. A redirect
 * target that the library resolves against a unix-socket base is another.
 * The encoding matches what a real client library applies to any authority
 * component of a URL. It escapes every byte outside an unreserved or
 * sub-delim set. This includes '/'. The whole purpose of this scheme is to
 * pack a path that itself contains '/' into one authority component. */
static char *_percent_encode_unix_path(ccol_memmgmt_procs_t *mp,
                                       const char *path) {
  return _percent_encode_unix_path_len(mp, path, strlen(path));
}

#ifdef RUNNING_UNIT_TESTS
/*
 * A white-box test helper. It exposes the overflow guard of
 * _percent_encode_unix_path_len directly. It follows the same pattern as
 * _chttp_ob_append_overflow_guard_for_tests, which covers this exact class
 * of guard. fake_len stands in for strlen(path). A test therefore never has
 * to build a real path string of many exabytes.
 *
 * Call this function ONLY with a fake_len that is greater than the threshold
 * of the guard, which is (SIZE_MAX - 1) / 3. The guard then rejects the call
 * before the loop runs. `path` therefore only has to be readable for the
 * small, real prefix that a test passes. The size that fake_len claims does
 * not matter.
 *
 * Do not call this function with a fake_len that is not greater than the
 * threshold. The loop below then indexes into `path` for up to fake_len
 * bytes. That is a real out-of-bounds read for any ordinary small test
 * buffer. This helper exists only to exercise the rejection path. It does
 * not exercise the ordinary success path. Every other unix-socket test that
 * is built on _chttp_resolve_redirect_url_for_tests already covers that
 * path. This helper is not part of the public API. A gate keeps this symbol
 * out of a production build of libccollections.so. Every other white-box
 * helper in this file has the same gate.
 */
bool _chttp_percent_encode_unix_path_overflow_guard_for_tests(
    ccol_memmgmt_procs_t *mp, const char *path, size_t fake_len) {
  char *r = _percent_encode_unix_path_len(mp, path, fake_len);
  bool rejected = (r == NULL);
  _ccol_mem_free(mp, r);
  return rejected;
}
#endif /* RUNNING_UNIT_TESTS */

/* This percent-decodes [start, start+len) into a new NUL-terminated string
 * that the library allocates. The library uses it only for userinfo
 * components. It never decodes path or query bytes. Those go to the server
 * with their escapes intact; see _build_request_target. This function rejects a
 * malformed escape, which is a '%' that 2 hex digits do not follow. It also
 * rejects a decoded NUL byte inside the component. A silent truncation of a
 * password at a NUL would build a slightly wrong Authorization header in
 * place of a clear error. */
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

/* This is true for a byte that RFC 3986 lets appear literally in the path
 * or the query of a request-target: unreserved, sub-delims, ':', '@', '/'
 * and '?'. '%' is handled on its own by the caller. */
static bool _is_target_literal_byte(unsigned char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9'))
    return true;
  /* The unreserved marks, then the sub-delims, then ':', '@', '/', '?'.
   * The test for 0 keeps strchr from matching the terminator. */
  return c != 0 && strchr("-._~!$&'()*+,;=:@/?", (int)c) != NULL;
}

/*
 * This builds the request-target that the library writes onto the request
 * line from the path and query of a URL, pq[0..pq_len). The result is always
 * a valid RFC 3986 origin-form, "/path[?query]". The library applies it to
 * the URL of the caller and to every resolved redirect target, because both
 * go through the URL parser.
 *
 * A byte that RFC 3986 does not allow literally in a path or a query is
 * percent-encoded as "%XX" with upper-case hex digits. That covers the space,
 * the control bytes other than CR and LF, DEL, every byte of 0x80 and
 * above, and the characters that RFC 3986 excludes, such as '"', '<', '>',
 * '\', '^', '`', '{', '|', '}', '[' and ']'. An existing "%XX" escape
 * stays as it is, so nothing is ever encoded twice. A '%' that two hex
 * digits do not follow is not a valid URL, and the result is
 * ccol_http_invalid_url; Go's net/url rejects the same input. The caller
 * rejects CR and LF before this runs.
 *
 * A target that does not start with '/' gets one in front. That is the empty
 * path, or a query with no path.
 */
static ccol_retval_t _build_request_target(ccol_memmgmt_procs_t *mp,
                                           const char *pq, size_t pq_len,
                                           char **out) {
  *out = NULL;
  bool add_slash = !(pq_len > 0 && pq[0] == '/');
  size_t need = add_slash ? 1 : 0;
  for (size_t i = 0; i < pq_len; i++) {
    unsigned char c = (unsigned char)pq[i];
    /* need grows by at most 3 for each byte, and pq_len bytes are already
     * in memory, so this guard is the only overflow check that the sum
     * needs. */
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
 * This parses the authority component of a "http+unix://" URL. p points just
 * past the "http+unix://" prefix, at a percent-encoded filesystem path. One
 * example is "%2Fvar%2Frun%2Fapp.sock". This matches the convention of
 * Python's requests-unixsocket. The library recognises "http+unix://" only.
 * It does not recognise "https+unix://", because TLS over a local socket has
 * no real use case. That prefix matches no known prefix at all, so it falls
 * through to the ccol_http_invalid_url return of the caller. This scheme has
 * no support for userinfo, which is a "user:pass@" part. There is no
 * established convention that joins the two. Also, the percent-encoded path
 * itself can legitimately contain '@' bytes after the decode.
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

  /* This is the same refusal of a raw CR or LF byte as the identical check
   * in _parse_chttp_url. See the comment there. */
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

/* This returns an owned "[host]" for an IPv6 literal. For any other host it
 * returns a plain strdup. The library uses it wherever a host must go through
 * a URL string and come back. origin_key is one such place. A rebuilt
 * redirect URL is another. connect() and TLS always use chttp_url_t.host
 * directly, which keeps no brackets. */
static char *_format_bracketed_host(ccol_memmgmt_procs_t *mp, const char *host,
                                    bool is_ipv6) {
  if (!is_ipv6) return ccol_strdup(mp, host);
  size_t hlen = strlen(host);
  /* This guards hlen + 3 against an overflow. Every other allocation that
   * computes a size in the URL and redirect helpers of this file has the
   * same guard. _percent_encode_unix_path_len, _merge_ref_path and
   * _concat_len are those helpers. They all guard this same class of
   * computation. It does not matter that no real input is large enough to
   * trigger it. This project has a standing policy for this. An unguarded
   * overflow in a size computation is a real bug, whatever size of input it
   * needs. Without this guard, an hlen of SIZE_MAX - 2 or more wraps the
   * allocation size. `out` is then too small, and the memcpy below writes
   * past its end. */
  if (hlen > SIZE_MAX - 3) return NULL;
  char *out = (char *)_ccol_mem_alloc(mp, hlen + 3);
  if (!out) return NULL;
  out[0] = '[';
  memcpy(out + 1, host, hlen);
  out[hlen + 1] = ']';
  out[hlen + 2] = '\0';
  return out;
}

/* unreserved and sub-delims of RFC 3986 SS2.2 and SS2.3, in ASCII and
 * independent of the locale. */
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

/* Reports whether s[0..len) is a reg-name of RFC 3986 SS3.2.2 that is not
 * empty: unreserved, sub-delims and pct-encoded only. An IPv4address is a
 * reg-name as far as the characters go. Everything else (whitespace, a
 * control byte, a byte at or above 0x80, a backslash, a '[' or a '%' that
 * two hex digits do not follow) names an authority that recipients read
 * differently, and it would reach the Host header, the key of the idle
 * pool, SNI and the resolver as it is. A name outside ASCII travels in its
 * ASCII (punycode, RFC 5891) form, which the caller supplies. */
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

/* Reports whether s[0..len), the text between the brackets of an
 * IP-literal, is an IPv6address or an IPvFuture of RFC 3986 SS3.2.2:
 *
 *   IPvFuture = "v" 1*HEXDIG "." 1*( unreserved / sub-delims / ":" )
 *
 * An IPv6address is checked by inet_pton(), whose text form (RFC 4291
 * SS2.2) is the IPv6address rule of RFC 3986. A zone identifier
 * (RFC 6874) is not part of that rule, and it is refused. */
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
  memcpy(text, s, len);
  text[len] = '\0';
  struct in6_addr addr;
  return inet_pton(AF_INET6, text, &addr) == 1;
}

/* Reports whether s[0..len) is userinfo of RFC 3986 SS3.2.1: unreserved,
 * pct-encoded, sub-delims and ":". The parser also accepts an unescaped
 * '@' there, because it splits the authority at the LAST '@' and a
 * password that is not fully encoded can hold one; Go's net/url accepts
 * the same. Every other byte, a backslash included, is refused: with a
 * backslash before the '@', this parser names the host after the '@' and a
 * WHATWG reader, which treats a backslash as '/', names the one before it.
 * The '%' escapes are checked by the decode that follows. */
static bool _uri_userinfo_is_valid(const char *s, size_t len) {
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (!_uri_unreserved_or_sub_delim(c) && c != ':' && c != '@' && c != '%')
      return false;
  }
  return true;
}

/*
 * The URL parser. It recognises "http://" and "https://". It recognises an
 * optional "user:pass@" userinfo prefix. It turns that prefix into a "Basic
 * <base64>" Authorization value that is ready to send. The forms "user@" and
 * ":pass@" are also valid RFC 3986 syntax, and the parser recognises both.
 * The parser recognises a plain reg-name or IPv4 host. It also recognises a
 * bracketed IPv6 literal such as "[::1]". It recognises an optional ":port".
 * It recognises a path and query, and it discards any trailing "#fragment".
 * A client never sends a fragment to a server (RFC 3986 SS3.5). A fragment
 * in the request line would therefore be a real correctness bug. It is not a
 * documented choice of scope.
 *
 * The host must match the uri-host rule of RFC 3986 SS3.2.2, and the
 * userinfo its userinfo rule (with '@' also accepted); anything else is
 * ccol_http_invalid_url. See _uri_reg_name_is_valid,
 * _uri_ip_literal_is_valid and _uri_userinfo_is_valid. A host that matches
 * the grammar and names nothing fails at connect time, as
 * ccol_http_host_resolution_failed.
 *
 * The path and the query become the request-target through
 * _build_request_target, which percent-encodes every byte that may not
 * appear literally in them and refuses a '%' that two hex digits do not
 * follow. A redirect target goes through this same parser, so the same rule
 * covers a Location header.
 *
 * The parser also recognises
 * "http+unix://<percent-encoded-path>[/path][?query]". See
 * _parse_chttp_unix_url. It does not recognise "https+unix://". That prefix
 * falls through to the ccol_http_invalid_url below, like any other scheme
 * that the parser does not know.
 */
static ccol_retval_t _parse_chttp_url(ccol_memmgmt_procs_t *mp, const char *url,
                                      chttp_url_t *out) {
  memset(out, 0, sizeof(*out));
  if (!url) return ccol_http_invalid_url;

  if (strncasecmp(url, "http+unix://", 12) == 0)
    return _parse_chttp_unix_url(mp, url + 12, out);

  bool https;
  const char *p;
  if (strncasecmp(url, "http://", 7) == 0) {
    https = false;
    p = url + 7;
  } else if (strncasecmp(url, "https://", 8) == 0) {
    https = true;
    p = url + 8;
  } else {
    return ccol_http_invalid_url;
  }

  /* Authority = [ userinfo "@" ] host [ ":" port ]. The first '/', '?' or
   * '#' ends it. The end of the string also ends it. */
  const char *authority_end = p;
  while (*authority_end && *authority_end != '/' && *authority_end != '?' &&
         *authority_end != '#')
    authority_end++;

  /* This finds the last unescaped '@' in the authority. It therefore accepts
   * an unescaped '@' inside a password that is not fully encoded. Common
   * real parsers accept the same input. */
  const char *last_at = NULL;
  for (const char *s = p; s < authority_end; s++)
    if (*s == '@') last_at = s;

  char *userinfo_authorization = NULL;
  const char *host_scan_start = p;
  if (last_at) {
    /* The parser finds the delimiters '@' and ':' in the RAW string, which
     * is still percent-encoded. It then decodes each half on its own. A
     * decode first and a search second would split on a decoded '@' or ':'
     * that is meant to be literal content of the password. That split is
     * wrong. */
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

    /* RFC 7617 SS2 makes the colon the only delimiter of the encoded
     * "user-id:password" string. A user-id that contains a colon can
     * therefore not travel as itself. A recipient splits at the first colon
     * and reads a different identity than the one that the URL names. This
     * is a property of the URL and not a failure of a resource. The library
     * reports it as such. It does not fold it into the allocation-failure
     * path below. */
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
    /* The literal ends inside the authority. A ']' further on, in the path
     * or the query, does not close it: "http://[a/b]/c" has no valid
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
  /* The host must be a uri-host of RFC 3986 SS3.2.2. It goes as it is into
   * the "host: " header line that _serialize_request builds, into the key
   * of the idle pool, into SNI and into the resolver. A byte outside the
   * grammar there either injects header lines (CR, LF), splits one name
   * into two tokens (SP, HTAB), or names a host that another reader of the
   * same URL resolves differently. */
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
    /* An empty port, as in "http://host:/", names the default port of the
     * scheme (RFC 3986 section 3.2.3), which curl and Go's net/url also
     * read it as. Port 0 names no port that a connect can reach. */
    if (pd > 0 && pv == 0) {
      _ccol_mem_free(mp, userinfo_authorization);
      return ccol_http_invalid_url;
    }
    /* The digit loop above stops at the first byte that is not a digit,
     * whatever that byte is. Without this check, garbage right after a valid
     * port falls through into the path and query computation below.
     * "http://host:80abc/get" is one such URL. The parser then treats
     * "abc/get" as the path. The library sends the request to a different
     * target than the one that the URL string names, and it reports no
     * error. */
    if (*q != '\0' && *q != '/' && *q != '?' && *q != '#') {
      _ccol_mem_free(mp, userinfo_authorization);
      return ccol_http_invalid_url;
    }
    if (pd > 0) port = (uint16_t)pv;
  }

  /* A fragment is a hard delimiter with no conditions. No escape rules apply
   * to '#' itself, because the start of a fragment is never quoted. */
  const char *pq = (*q && *q != '#') ? q : "/";
  size_t pq_raw_len = strlen(pq);
  const char *frag = memchr(pq, '#', pq_raw_len);
  size_t pq_len = frag ? (size_t)(frag - pq) : pq_raw_len;

  /* A raw CR or LF byte in the path or the query is a malformed URL, and
   * the library refuses it rather than encode it. That keeps the same rule
   * as the host check above: a URL string that carries a line break was
   * built to inject header lines or a smuggled second request. The
   * percent-encoded form of such a byte is different. It is ordinary path
   * or query content, and _build_request_target keeps it as it is. Every
   * other byte that may not appear literally on the request line is
   * percent-encoded there. */
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

  /* pq can be empty, or start with '?', which is a query with no path
   * component. _build_request_target adds the leading '/' then. */
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
  /* A host name compares without regard to case (RFC 4343), and so does the
   * hex of an IPv6 literal. The key of the idle pool is therefore in lower
   * case, so that "Example.com" and "example.com" share their connections.
   * The scheme is lower case already and the port is digits. host itself
   * keeps the spelling of the URL for the Host header. A trailing dot stays
   * in the key: "a." is an absolute name and "a" can resolve through a
   * search domain to another host, so the two are not one origin. */
  for (char *c = origin_key; *c; c++) *c = (char)tolower((unsigned char)*c);

  out->is_https = https;
  out->is_ipv6 = is_ipv6;
  out->host = host;
  out->port = port;
  out->path_and_query = path_and_query;
  out->origin_key = origin_key;
  out->userinfo_authorization = userinfo_authorization;
  return ccol_success;
}

/* RFC 3986 SS5.2.4 "Remove Dot Segments". This works on a path only. It
 * never works on a query string. A literal ".." or "." inside query bytes
 * must never become path navigation. A caller removes any "?query" before
 * the call and adds it again after the call. */
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

/* RFC 3986 SS5.3 "merge". This works on the path component only. The caller
 * joins the query string of the reference back on after the removal of the
 * dot segments runs. See the comment of _remove_dot_segments for the
 * reason. */
static char *_merge_ref_path(ccol_memmgmt_procs_t *mp,
                             const char *base_path_and_query,
                             const char *ref_path, size_t ref_path_len) {
  const char *base_query = strchr(base_path_and_query, '?');
  size_t base_path_len = base_query ? (size_t)(base_query - base_path_and_query)
                                    : strlen(base_path_and_query);

  size_t dir_len;
  if (ref_path_len == 0) {
    /* A reference that is a query only, such as "?x", reuses the base path
     * exactly as it is. */
    dir_len = base_path_len;
  } else {
    const char *last_slash = NULL;
    for (size_t i = 0; i < base_path_len; i++)
      if (base_path_and_query[i] == '/') last_slash = &base_path_and_query[i];
    dir_len = last_slash ? (size_t)(last_slash - base_path_and_query) + 1 : 0;
  }

  /* This guards the computation of the needed size below against an
   * overflow. _ob_append and _sink_buffered have the same guard for the same
   * class of computation. dir_len and ref_path_len have INDEPENDENT sizes.
   * One is a redirect base path. The other is the path of a Location
   * reference. A single strlen() result must be very large to come near
   * SIZE_MAX. But the sum of these two does not need either one alone to be
   * that large. Without this guard, the allocation makes `out` too small,
   * and the memcpy calls below write past its end. */
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
 * A white-box test helper. It exposes the overflow guard of _merge_ref_path
 * directly. It follows the same pattern as
 * _chttp_percent_encode_unix_path_overflow_guard_for_tests. _merge_ref_path
 * already takes ref_path_len as an explicit parameter. This helper can
 * therefore pass fake_ref_path_len straight through, and it needs no other
 * change.
 *
 * Call this function ONLY with a fake_ref_path_len that is greater than the
 * threshold of the guard. The guard then rejects the call before either
 * memcpy call runs. See the doc comment of that other helper for the
 * out-of-bounds read that a false length without a rejection creates here.
 *
 * This helper is not part of the public API. A gate keeps this symbol out of
 * a production build of libccollections.so. Every other white-box helper in
 * this file has the same gate.
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

/* This joins a[0..a_len) and b[0..b_len) into one new NUL-terminated string
 * that the library allocates. a_len and b_len are explicit parameters, and
 * this function does not compute them with strlen. A white-box test can
 * therefore exercise the overflow guard below directly, with a false length.
 * The test does not need to build a string of many exabytes. See
 * _chttp_concat_len_overflow_guard_for_tests below. _resolve_redirect_url
 * uses this function to join a resolved path back to the query string of a
 * reference. */
static char *_concat_len(ccol_memmgmt_procs_t *mp, const char *a, size_t a_len,
                         const char *b, size_t b_len) {
  /* This guards the computation of the needed size below against an
   * overflow. _merge_ref_path just above has the same guard. a_len and b_len
   * have independent sizes. Their sum can come near SIZE_MAX although
   * neither one alone is that large. Without this guard, the allocation
   * makes `out` too small, and the memcpy calls below write past its end. */
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
 * A white-box test helper. It exposes the overflow guard of _concat_len
 * directly. Call it ONLY with a fake_a_len and fake_b_len pair that is
 * greater than the threshold of the guard. The guard then rejects the call
 * before either memcpy call runs. See the doc comment of
 * _chttp_merge_ref_path_overflow_guard_for_tests for the out-of-bounds read
 * that a false length without a rejection creates here. This helper is not
 * part of the public API. A gate keeps this symbol out of a production build
 * of libccollections.so. Every other white-box helper in this file has the
 * same gate.
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
 * This is true if `s` starts with an RFC 3986 SS3.1 "scheme ':'", which is
 * ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":". The scan stops at the
 * first '/', '?' or '#'. A colon after any of those bytes is ordinary path,
 * query or fragment content. It is never a scheme delimiter. For example,
 * "/a:b" and "?a:b" have no scheme.
 *
 * _resolve_redirect_url uses this to find a Location value that is an
 * absolute-URI reference with a scheme that this client does not handle
 * separately. The client handles http, https and http+unix with a plain
 * prefix check. RFC 3986 SS5.2.2 says that ANY reference with a scheme is
 * absolute, which is T = R. This is true whether or not the client knows how
 * to fetch that scheme.
 */
static bool _location_has_scheme(const char *s) {
  if (!isalpha((unsigned char)s[0])) return false;
  for (const char *p = s + 1;; p++) {
    if (*p == ':') return true;
    if (!(isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.'))
      return false;
  }
}

/*
 * This resolves a Location header against the URL of the current hop. It
 * follows RFC 3986 SS5.2-5.3. It supports absolute URLs. It supports
 * protocol-relative references such as "//host/path". It supports
 * absolute-path references such as "/foo". It supports general
 * relative-path references such as "foo", "../foo", "./foo" and "?query".
 * It returns NULL only for a location that is NULL or empty, and the caller
 * treats that as ccol_http_transfer_aborted. Every other Location value with
 * plausible syntax resolves to SOME absolute URL. A real browser or curl
 * does the same.
 *
 * _parse_chttp_url always parses the result again on the next hop. This
 * function therefore needs to know nothing about the carry-forward of
 * userinfo or credentials. The hop loop of each tier handles that. This is
 * also what makes an absolute-URI reference with an unknown scheme come out
 * right. See _location_has_scheme below. This function needs to know nothing
 * about which schemes _parse_chttp_url accepts. It returns the reference
 * exactly as it is. The _parse_chttp_url of the next hop then rejects it
 * with the same ccol_http_invalid_url that an unsupported top-level request
 * URL already gets. This function therefore never resolves it wrongly and
 * without a report.
 *
 * The removal of dot segments (RFC 3986 SS5.2.4) applies to the path
 * component only. Every branch below that can make a query string splits
 * that string off first. It then appends the string again, exactly as it is.
 * A query value that contains "/", ".." or "." bytes therefore never becomes
 * path navigation.
 */
static char *_resolve_redirect_url(ccol_memmgmt_procs_t *mp,
                                   const chttp_url_t *base,
                                   const char *location) {
  if (!location || !*location) return NULL;
  if (strncasecmp(location, "http://", 7) == 0 ||
      strncasecmp(location, "https://", 8) == 0 ||
      strncasecmp(location, "http+unix://", 12) == 0) {
    return ccol_strdup(mp, location);
  }
  /* This handles an absolute-URI reference whose scheme is none of the three
   * above. "g:h", "mailto:x@y" and "ftp://host/path" are examples. RFC 3986
   * SS5.2.2 says that T = R as soon as R has a scheme, with no condition.
   * Such a reference must therefore never fall through to the code for a
   * relative reference below. That code would merge the whole
   * "scheme:opaque" string onto the path of the CURRENT origin, as if it
   * were a same-origin relative path. For example, "g:h" against
   * "http://a/b/c/d;p?q" would resolve to "http://a/b/c/g:h" with no error
   * report. The correct answer is the absolute reference "g:h". This
   * function returns the reference exactly as it is, like the three known
   * scheme prefixes just above. See the doc comment of this function for the
   * reason why the _parse_chttp_url of the next hop is what rejects it. */
  if (_location_has_scheme(location)) return ccol_strdup(mp, location);

  const char *scheme = base->is_https ? "https" : "http";

  if (location[0] == '/' && location[1] == '/') {
    /* This handles a protocol-relative reference. Such a reference names a
     * network host to redirect to with no ambiguity. That host can be a
     * different one. This is true whether or not the base is a unix-socket
     * target. The reference takes the scheme of the base. For a unix base
     * that scheme is always "http", because is_https is always false there.
     * The rest is already a well-formed authority and path for the next
     * _parse_chttp_url call. This branch does not remove dot segments. The
     * branch above for a fully absolute URL does not remove them either. */
    int needed = snprintf(NULL, 0, "%s:%s", scheme, location);
    if (needed < 0) return NULL;
    char *out = (char *)_ccol_mem_alloc(mp, (size_t)needed + 1);
    if (!out) return NULL;
    snprintf(out, (size_t)needed + 1, "%s:%s", scheme, location);
    return out;
  }

  /* RFC 3986 SS3.5 says that a client never sends a fragment to a server.
   * The reference-resolution algorithm of SS5.3 also keeps a fragment out of
   * R.path and R.query. This code must therefore discard the fragment here,
   * before any merge or removal of dot segments runs. It must not leave the
   * removal to the _parse_chttp_url call of the next hop. The absolute-URL
   * and protocol-relative branches above can do that, but this branch
   * cannot. Without this removal, _remove_dot_segments below walks the bytes
   * of a fragment that contains "/../", such as "g#/../h", as real path
   * navigation. It then resolves to the wrong target with no error report. A
   * reference that is a fragment only, such as "#s", goes into the merge
   * branch with a ref_path that is not empty. The correct answer is to reuse
   * the base path exactly as it is. No escape rules apply to '#' itself.
   * _parse_chttp_url treats a fragment delimiter in the same way. The first
   * raw '#' therefore always starts the fragment. */
  const char *frag = strchr(location, '#');
  size_t loc_len_nf = frag ? (size_t)(frag - location) : strlen(location);
  char *location_nf = (char *)_ccol_mem_alloc(mp, loc_len_nf + 1);
  if (!location_nf) return NULL;
  memcpy(location_nf, location, loc_len_nf);
  location_nf[loc_len_nf] = '\0';
  location = location_nf;

  /* For a unix-socket base, an absolute-path or relative-path reference
   * rebuilds "http+unix://<percent-encoded-path>" plus the resolved path. It
   * does not rebuild "scheme://host:port" plus the path. A unix target has
   * no host and no port to rebuild from. */
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
    /* The reference is a fragment only, such as "#s". RFC 3986 SS5.3 leaves
     * both R.path and R.query undefined in that case. The result is
     * T.path = Base.path and T.query = Base.query. This branch therefore
     * reuses the whole base->path_and_query exactly as it is. It does not
     * reuse only the directory part. The general code for ref_path_len == 0
     * further below would reuse only the PATH of the base and drop its
     * query. That code exists for a real query-only reference such as "?y",
     * where R.query IS defined. Such a reference gives its own query to take
     * the place of the query of the base. The base is already fully resolved
     * and normalised. This branch therefore needs no merge and no removal of
     * dot segments. */
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

  /* This splits R.query off R.path ONCE, at the start. Both branches below
   * share the result. remove_dot_segments (RFC 3986 SS5.2.4) must work on
   * the path component only. It must never work on query bytes. A literal
   * ".." or "." inside a query value must never become path navigation. For
   * example, a query that contains "/../" would make the removal of dot
   * segments walk backwards and delete path segments that it must not touch.
   * Both branches append ref_query again, unchanged, after the removal of
   * the dot segments runs. There may be no ref_query at all. */
  const char *ref_query = strchr(location, '?');
  size_t ref_path_len =
      ref_query ? (size_t)(ref_query - location) : strlen(location);

  char *new_path = NULL;
  if (location[0] == '/') {
    /* This handles an absolute-path reference. T.path is
     * remove_dot_segments(R.path) directly. It needs no merge against the
     * base path. */
    char *path_only = (char *)_ccol_mem_alloc(mp, ref_path_len + 1);
    if (path_only) {
      memcpy(path_only, location, ref_path_len);
      path_only[ref_path_len] = '\0';
      new_path = _remove_dot_segments(mp, path_only);
      _ccol_mem_free(mp, path_only);
    }
  } else if (ref_path_len == 0) {
    /* RFC 3986 SS5.3 says that R.path == "" means T.path = Base.path EXACTLY
     * AS IT IS, with no merge and no removal of dot segments. A query-only
     * reference such as "?y" has this shape. This code does not assume that
     * the base is already normalised. This client never removes the dot
     * segments of the original request URL of the caller either. See
     * _parse_chttp_url. Take a "clean" form of that URL at this one
     * reference shape. It would redirect to a different path than the one
     * that the original request used. It would also report nothing. No
     * server asked for that change. It would also differ from the RFC and
     * from reference implementations such as Python's
     * urllib.parse.urljoin. Both leave a dotted base path such as
     * "/a/../b" unchanged for a query-only reference. The fragment-only
     * branch above does the same, for the same reason. The
     * ref_path_len == 0 code of _merge_ref_path already makes
     * exactly this copy: dir_len covers the whole base path, and it appends
     * nothing. The _remove_dot_segments call that the branch below runs
     * afterwards is what would normalise that copy again. This is why this
     * case needs its own branch. It is not enough to skip that one call
     * inline. */
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
    /* The overflow guard of _concat_len makes this safe against
     * path_len + query_len that wraps SIZE_MAX. See the doc comment of that
     * function. This code treats the rejection as an ordinary allocation
     * failure, in the !with_query branch. The real out-of-memory case just
     * below it gets the same treatment. */
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

/*
 * This is true when the library may fetch a redirect target over the
 * transport that the target names. The library resolves that target against
 * `base`. A redirect must never add the AF_UNIX transport and must never point
 * that transport at another socket. Those two are unconditional. A redirect
 * that moves a chain off TLS onto a plaintext connection is refused only when
 * the caller asked for that with the prevent_tls_downgrade_on_redirect field
 * of chttp_request_t. _parse_chttp_url sends a "http+unix://" URL to a
 * connect() against a filesystem path. The library therefore honours such a
 * target only when the hop that it came from was itself bound to that exact
 * same socket path. Every other target goes to the parse of the next hop,
 * exactly as it arrives here. http, https, and any scheme that
 * _parse_chttp_url does not know are those other targets.
 *
 * Without this check, a response from any ordinary http or https server
 * points the next hop at any local socket. One such response is
 * "Location: http+unix://%2Fvar%2Frun%2Fdocker.sock/containers/json". That
 * server also controls the request target. This gives the server a
 * server-side request forgery primitive against every AF_UNIX service that
 * the calling process can reach. Any application that fetches a URL whose
 * responses it does not fully control can reach that primitive.
 *
 * The screen is a prefix test, because "http+unix://" is the only spelling
 * that _parse_chttp_url treats as a unix target. The test ignores case, for
 * the same reason that function ignores it. The library parses a target that
 * passes the screen. The comparison is therefore against the DECODED socket
 * path, and not against one of its many equal percent-encodings. The library
 * lets a target that fails to parse through unchanged. The _parse_chttp_url
 * of the next hop rejects it before any connect runs.
 *
 * The library applies this test to each hop, against the hop that the
 * Location came from. This also settles a chain of any length. A chain that
 * leaves the unix transport can never enter it again. A chain that is on
 * that transport can only stay on the same socket. With
 * prevent_tls_downgrade_on_redirect set, a chain that starts on https stays
 * on https for every hop.
 */
static bool _redirect_transport_allowed(ccol_memmgmt_procs_t *mp,
                                        const chttp_url_t *base,
                                        const char *next_url,
                                        bool prevent_downgrade) {
  if (strncasecmp(next_url, "http+unix://", 12) != 0) {
    /* An OPT-IN refusal of a downgrade from https to http.
     *
     * The default follows such a redirect. That matches curl, whose
     * CURLOPT_REDIR_PROTOCOLS permits both schemes, and the Go net/http
     * client. A caller porting from either gets the behaviour they already
     * rely on, and does not meet a refusal that no other client would give.
     *
     * The cost of that default is real, which is why the opt-in exists. The
     * peer that sent the Location chooses the downgrade, and once a chain
     * leaves TLS that hop and every later hop travel in clear. A caller who
     * sets the prevent_tls_downgrade_on_redirect field of chttp_request_t
     * gets a loud refusal instead: ccol_http_invalid_url, with no request
     * sent over the plaintext connection.
     *
     * An UPGRADE in the other direction, from http to https, is never
     * refused. It can only add protection.
     *
     * A target that does not parse goes through unchanged, exactly as the
     * unix screen below lets one through: the _parse_chttp_url of the next hop
     * rejects it with ccol_http_invalid_url before any connect runs. A target
     * with a scheme that this client does not know reaches the same place. */
    if (!prevent_downgrade || !base->is_https) return true;
    chttp_url_t next;
    if (_parse_chttp_url(mp, next_url, &next) != ccol_success) return true;
    bool allowed = next.is_https;
    _url_free(mp, &next);
    return allowed;
  }
  if (!base->is_unix || !base->unix_socket_path) return false;
  chttp_url_t next;
  if (_parse_chttp_url(mp, next_url, &next) != ccol_success) return true;
  bool allowed = next.is_unix && next.unix_socket_path &&
                 strcmp(next.unix_socket_path, base->unix_socket_path) == 0;
  _url_free(mp, &next);
  return allowed;
}

/* These are white-box test helpers. They expose _parse_chttp_url and
 * _resolve_redirect_url. chttp_url_t is a file-local type. These helpers
 * therefore flatten the result into out parameters, or into a plain string.
 * An mp of NULL means that plain malloc gives every result. See
 * _ccol_mem_alloc. Test code frees them with plain free(). These helpers are
 * not part of the public API. A gate keeps these symbols out of a production
 * build of libccollections.so. Every other white-box helper in this file has
 * the same gate. */
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
  char *result = _resolve_redirect_url(mp, &base, location);
  _url_free(mp, &base);
  return result;
}

/* This resolves `location` against `base_url` exactly as a hop does. It
 * gives the resolved absolute URL back through *resolved_out. That value is
 * NULL if the reference does not resolve at all. The caller frees it with
 * plain free(). prevent_downgrade is the per-request opt-in that
 * chttp_request_t spells prevent_tls_downgrade_on_redirect. The return value
 * says whether _redirect_transport_allowed accepts the transport of that
 * target. */
bool _chttp_redirect_transport_allowed_for_tests(const char *base_url,
                                                 const char *location,
                                                 bool prevent_downgrade,
                                                 char **resolved_out) {
  ccol_memmgmt_procs_t *mp = NULL;
  chttp_url_t base;
  if (resolved_out) *resolved_out = NULL;
  ccol_retval_t rv = _parse_chttp_url(mp, base_url, &base);
  if (rv != ccol_success) return false;
  char *resolved = _resolve_redirect_url(mp, &base, location);
  bool allowed = resolved && _redirect_transport_allowed(mp, &base, resolved,
                                                         prevent_downgrade);
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

chttp_request_t *chttp_request_new_mp(chttp_method_t method, const char *url,
                                      const chttp_request_body_t *body,
                                      ccol_memmgmt_procs_t *mprocs,
                                      char **err_str) {
  if (!url) {
    if (err_str) *err_str = CCOL_ERR_STR("url must not be NULL");
    return NULL;
  }
  /* A body->data of NULL together with a body->len that is not zero is an
   * inconsistent body descriptor. There is nothing to copy body->len bytes
   * from. The body-copy block below runs only when body->data is not NULL.
   * Without this check, the library treats that pair as "no body" and
   * reports nothing to the caller. chttp_base64_encode_mp rejects the same
   * pair of a NULL data pointer and a length that is not zero. A caller with
   * a real bug, such as a wrong length next to a null buffer, needs a
   * ccol_invalid_args that it can diagnose. It does not need a request that
   * goes out with no body and no report. */
  if (body && !body->data && body->len > 0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("body->data must not be NULL when body->len > 0");
    return NULL;
  }
  if (mprocs && !ccol_verify_memmgmt_procs(mprocs, err_str)) return NULL;

  ccol_memmgmt_procs_t *mp = NULL;
  if (mprocs) {
    mp = (ccol_memmgmt_procs_t *)mprocs->malloc(sizeof(ccol_memmgmt_procs_t));
    if (!mp) {
      if (err_str) *err_str = CCOL_ERR_STR("failed to allocate mprocs");
      return NULL;
    }
    memcpy(mp, mprocs, sizeof(ccol_memmgmt_procs_t));
  }

  chttp_request_t *req =
      (chttp_request_t *)_ccol_mem_alloc(mp, sizeof(chttp_request_t));
  if (!req) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate request");
    if (mp) mp->free(mp);
    return NULL;
  }
  memset(req, 0, sizeof(*req));
  req->method = method;
  req->_m_procs = mp;

  req->url = ccol_strdup(mp, url);
  if (!req->url) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to copy url");
    _ccol_mem_free(mp, req);
    if (mp) mp->free(mp);
    return NULL;
  }

  if (body && body->data && body->len > 0) {
    void *body_copy = _ccol_mem_alloc(mp, body->len);
    if (!body_copy) {
      if (err_str) *err_str = CCOL_ERR_STR("failed to copy body");
      _ccol_mem_free(mp, req->url);
      _ccol_mem_free(mp, req);
      if (mp) mp->free(mp);
      return NULL;
    }
    memcpy(body_copy, body->data, body->len);
    req->body.data = body_copy;
    req->body.len = body->len;
  }

  /* The library copies content_type whenever the caller gives one. It does
   * not matter whether body->len > 0. A body of zero length with an explicit
   * content type is a legitimate request shape. An empty JSON payload from
   * CHTTP_JSON_BODY("", 0) is one example. _serialize_request adds a
   * Content-Type header by itself, and its documentation says that it
   * honours req->body.content_type whatever the length of the body is. This
   * copy must therefore not go inside the body->len > 0 branch above. There
   * it would discard content_type for exactly that shape, with no report. */
  if (body && body->content_type) {
    req->body.content_type = ccol_strdup(mp, body->content_type);
    if (!req->body.content_type) {
      if (err_str) *err_str = CCOL_ERR_STR("failed to copy content_type");
      _ccol_mem_free(mp, (void *)req->body.data);
      _ccol_mem_free(mp, req->url);
      _ccol_mem_free(mp, req);
      if (mp) mp->free(mp);
      return NULL;
    }
  }

  return req;
}

ccol_retval_t chttp_request_set_header(chttp_request_t *req, const char *name,
                                       const char *value) {
  if (!req || !name || !value) return ccol_invalid_args;
  /* A field-name must be a token that is not empty and that holds tchar
   * bytes only (RFC 7230 SS3.2.6). chttpsvr_resp_set_header applies the same
   * check on the server side, in chttpserver.c. chttp1_parser.c applies it
   * to the method and the header names of bytes that arrive off the wire. An
   * empty name builds a malformed "name: value\r\n" wire line. So does a
   * name with a byte outside that set, such as a space or a literal ':'.
   * This is true even when the name holds no CR or LF of its own. */
  if (!*name) return ccol_invalid_args;
  for (const char *p = name; *p; p++) {
    if (!chttp1_is_tchar((unsigned char)*p)) return ccol_invalid_args;
  }
  /* _serialize_request writes name and value onto the wire exactly as they
   * are, as "name: value\r\n". It applies no further escape. Some callers
   * put data that they do not trust into a request header. A forwarded
   * bearer token or a proxied header is such data. A CR or LF byte inside
   * that data lets whoever controls it inject any number of extra header
   * lines. It also lets them split the request into two. This is classic
   * HTTP request splitting, which is also called CRLF injection. This
   * function rejects such a byte at once. Every documented path that sets a
   * header goes through this one function. _serialize_request carries the
   * same check again. That second check is a backstop for the header map.
   * Any caller that holds the internal chmap handle can build and assign
   * that map directly. chttp_run_query and the tests do this. */
  if (strpbrk(name, "\r\n") || strpbrk(value, "\r\n")) return ccol_invalid_args;
  /* chttpclient.c implements no transfer-coding for a request body. It
   * implements neither chunked nor any other one. It always frames a request
   * that carries a body with Content-Length. It appends the exact bytes of
   * req->body.data. The library can therefore never honour a
   * "Transfer-Encoding" header that a caller sets. _serialize_request builds
   * its own Content-Length header. It gates that only on the absence of an
   * explicit content-length header, never on the absence of an explicit
   * transfer-encoding header. Silent acceptance would therefore add a
   * Content-Length header next to the header of the caller. The request
   * would then declare BOTH framings at once over a body that carries no
   * chunk encoding. RFC 7230 SS3.3.3 describes this ambiguous framing.
   * chttp1_parser.c in this codebase rejects exactly that shape when it
   * parses an incoming message. See its F_CONTENT_LENGTH and F_CHUNKED
   * conflict check. This function rejects the header here. It does not drop
   * it without a report, and it does not send it as it is. The CRLF check
   * above uses the same rule: fail loudly, and do not build a malformed wire
   * message. */
  if (strcasecmp(name, "transfer-encoding") == 0) return ccol_invalid_args;

  if (!req->headers) {
    char *err = NULL;
    chmap hm =
        chmap_create_full(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                          ccol_string, req->_m_procs, NULL, NULL, &err);
    if (!hm) return ccol_not_enough_memory;
    req->headers = hm;
  }

  size_t nlen = strlen(name);
  char *lower = (char *)_ccol_mem_alloc(req->_m_procs, nlen + 1);
  if (!lower) return ccol_not_enough_memory;
  for (size_t i = 0; i <= nlen; i++)
    lower[i] = (char)tolower((unsigned char)name[i]);

  cmap_pair kp = {.ptr = lower, .size = nlen + 1};
  cmap_pair vp = {.ptr = (void *)value, .size = strlen(value) + 1};

  ccol_retval_t rv = chmap_insert_elem((chmap)req->headers, &kp, &vp);
  _ccol_mem_free(req->_m_procs, lower);
  if (rv == ccol_key_already_present) rv = ccol_success;
  return rv;
}

const char *chttp_request_get_header(const chttp_request_t *req,
                                     const char *name) {
  if (!req || !name || !req->headers) return NULL;

  size_t nlen = strlen(name);
  char *lower = (char *)_ccol_mem_alloc(req->_m_procs, nlen + 1);
  if (!lower) return NULL;
  for (size_t i = 0; i <= nlen; i++)
    lower[i] = (char)tolower((unsigned char)name[i]);

  cmap_pair kp = {.ptr = lower, .size = nlen + 1};
  const cmap_pair *vp = NULL;
  ccol_retval_t rv = chmap_get_elem_ref((chmap)req->headers, &kp, &vp);
  _ccol_mem_free(req->_m_procs, lower);

  if (rv != ccol_success || !vp) return NULL;
  return (const char *)vp->ptr;
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

/* A deadline timeout_us microseconds from now on CLOCK_MONOTONIC. 0 gives an
 * inactive deadline, which means no limit. A value too large for the clock
 * saturates to the latest representable time and never wraps into the
 * past. */
static chttp_deadline_t _deadline_make(uint64_t timeout_us) {
  chttp_deadline_t d = {0};
  if (timeout_us == 0) return d; /* inactive, which means no limit */
  d.active = true;
  ccol_deadline_after_us(timeout_us, &d.deadline);
  return d;
}

/*
 * This returns false if the deadline passed at `now`. The caller then treats
 * the result as an immediate ccol_timed_out. In every other case it sets
 * *out_ms to the milliseconds that remain, rounded UP. A deadline that lies
 * a fraction of a millisecond ahead therefore gives 1 and not 0, and a wait
 * of *out_ms milliseconds never ends before the deadline itself: a poll(2)
 * that times out after that wait always finds the deadline passed. It sets
 * *out_ms to -1 if there is no active deadline, which tells the caller to
 * block forever.
 */
static bool _deadline_remaining_ms_at(const chttp_deadline_t *d,
                                      const struct timespec *now, int *out_ms) {
  if (!d->active) {
    *out_ms = -1;
    return true;
  }
  /* The arithmetic runs in int64_t, whatever the widths of long and time_t
   * are, and it saturates before it multiplies. A timeout of years, or a
   * deadline that saturated at the largest time_t, otherwise overflows the
   * product into a negative count, which reads as a deadline
   * that already passed. The nanosecond difference lies strictly between
   * -1 and 1 second, so a negative second count always means a deadline
   * in the past, and a count above INT_MAX / 1000 always means more than
   * INT_MAX milliseconds. */
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
/* A white-box accessor. It runs _deadline_remaining_ms against a deadline
 * add_sec seconds after the current monotonic time, so a test can put a
 * deadline at any distance, including one whose product in milliseconds
 * does not fit a long. */
bool _chttp_deadline_remaining_ms_for_tests(long long add_sec, int *out_ms) {
  chttp_deadline_t d = {0};
  d.active = true;
  clock_gettime(CLOCK_MONOTONIC, &d.deadline);
  d.deadline.tv_sec = (time_t)((long long)d.deadline.tv_sec + add_sec);
  return _deadline_remaining_ms(&d, out_ms);
}

/* A white-box accessor. It runs _deadline_remaining_ms_at against a
 * deadline that lies ahead_ns nanoseconds after a fixed instant, evaluated
 * at that instant, so a test can check the rounding with no race against
 * the clock. */
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

/* While this is true, every read point of every tier waits until its fd is
 * readable and then finds its overall deadline passed. A test can therefore
 * put "the deadline passed while the whole response was already buffered"
 * on each tier at the same point, with no race against a clock or against
 * the deadline sweep. */
static _Atomic bool g_expire_at_read_for_tests = false;

void _chttp_set_expire_at_read_for_tests(bool on) {
  atomic_store(&g_expire_at_read_for_tests, on);
}
#endif

/*
 * The rule of every tier for a response that is still being read: before
 * each read of response bytes, the overall deadline of the request is
 * checked, and a deadline that has passed ends the request with
 * ccol_timed_out. That holds even when the rest of the response has already
 * arrived and the next read would complete it. A response that a read
 * completed before the deadline passed is a success. Tier 1 applies this at
 * the top of the read loop of _chttp_read_message, Tier 2 at the start of
 * each read dispatch and before each further read of its TLS drain.
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

/* This returns whichever of a and b passes first. An "inactive" deadline
 * means no limit, so it never wins over an active one. The library uses it
 * to bound the interim wait for "Expect: 100-continue". That wait is bounded
 * by its own CHTTP_100_CONTINUE_WAIT_US budget and by what remains of the
 * overall deadline of the request. This needs no second, parallel
 * computation of the remaining milliseconds. The connect and overall pair of
 * _tcp_connect uses that other shape. It does not fit here, because this
 * wait is inside one _chttp_read_message call, and that call accepts one
 * chttp_deadline_t only. */
static chttp_deadline_t _deadline_earlier(chttp_deadline_t a,
                                          chttp_deadline_t b) {
  if (!a.active) return b;
  if (!b.active) return a;
  if (a.deadline.tv_sec != b.deadline.tv_sec)
    return (a.deadline.tv_sec < b.deadline.tv_sec) ? a : b;
  return (a.deadline.tv_nsec <= b.deadline.tv_nsec) ? a : b;
}

/* This initializes *cv against CLOCK_MONOTONIC. Every deadline that this
 * file computes uses the same clock, through _deadline_make and
 * clock_gettime(CLOCK_MONOTONIC, ...). If the platform has no condattr
 * support, this function falls back to the default clock of the platform.
 * Without this, a ccol_cond_var_timedwait call takes a timespec from
 * _deadline_make. It then compares that value from the monotonic clock
 * against a condition variable that uses the wall clock inside. That clock
 * is CLOCK_REALTIME by default. The wait then returns ETIMEDOUT at once, or it
 * never honours the deadline at all. _client_deadline_init_globals of
 * client_deadline_bundle initializes its condition variable in the same way,
 * for the same reason. */
static int _ccol_cond_var_init_monotonic(ccol_cond_var_t *cv) {
  ccol_cond_var_attr_t cv_attr;
  if (ccol_cond_var_attr_init(cv_attr) == 0) {
    ccol_cond_var_attr_setclock(cv_attr, CLOCK_MONOTONIC);
    int rv = ccol_cond_var_init_ca(*cv, cv_attr);
    ccol_cond_var_attr_destroy(cv_attr);
    return rv;
  }
  return ccol_cond_var_init(*cv);
}

/* ========================================================================== */
/*                         LOW-LEVEL SOCKET / TLS I/O                         */
/* ========================================================================== */

#ifdef RUNNING_UNIT_TESTS
/*
 * This reproduces, on demand, the one TLS condition that a test cannot cause
 * against a peer that cooperates. That condition is an SSL_write that must
 * READ before it can make progress, or an SSL_read that must WRITE before it
 * can make progress. A TLS 1.2 renegotiation causes it. So does a TLS 1.3
 * KeyUpdate that arrives at the wrong moment. ctls.c reports both as -1 with
 * EWOULDBLOCK. It publishes the real direction separately, through
 * ctls_conn_wants_write(). An I/O loop that always waits in its own home
 * direction therefore spins on a level-triggered reactor. Or it waits for a
 * readiness event that never arrives.
 *
 * While a mode is armed, the library reports the first TLS I/O attempt in
 * the named home direction as EWOULDBLOCK and does not run it.
 * _conn_tls_wants_write then answers with the OPPOSITE direction. Only a
 * real wait or registration by this module in that opposite direction
 * releases the block. That release is a state transition and not a clock. An
 * implementation that keeps a wait in its home direction never releases the
 * block, and it runs out of deadline.
 */
#define CHTTP_TLS_DIR_INJECT_OFF 0
#define CHTTP_TLS_DIR_INJECT_WRITE_WANTS_READ 1
#define CHTTP_TLS_DIR_INJECT_READ_WANTS_WRITE 2

static _Atomic int g_tls_dir_inject_mode = CHTTP_TLS_DIR_INJECT_OFF;
static _Atomic bool g_tls_dir_blocked = false;
static _Atomic bool g_tls_dir_released = false;
/* This is a safety net against a hang only. It is never the mechanism for
 * correctness. The deadline of an implementation that keeps a wait in
 * its home direction must catch it, and the deadline of Tier 1 does. A Tier
 * 2 hop whose registration is pinned to the wrong direction has no deadline
 * of its own after the sweep shuts its fd down. Such a hop would read again
 * forever. After this number of refused attempts, the injection stops its
 * refusals. It does NOT mark itself released. A test that checks the
 * released state or the interest state therefore still fails. It does not
 * hang the whole binary. */
#define CHTTP_TLS_DIR_INJECT_MAX_BLOCKS 256
static _Atomic unsigned g_tls_dir_block_attempts = 0;
static _Atomic unsigned g_tls_dir_read_interest_while_writing = 0;
static _Atomic unsigned g_tls_dir_write_interest_while_reading = 0;

/* This is true when a TLS I/O attempt whose home direction is `home_is_write`
 * must not run. The library then reports it as blocked and asks for the
 * other direction. */
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

/* This records a wait or a registration in the direction of
 * `waiting_for_write` while an injected block is outstanding. It returns true
 * when that direction is the one that the injection asked for. That return
 * releases the block. For the poll-based waits of Tier 1 it also stands in
 * for the arrival of the bytes of the peer. The test therefore never depends
 * on a real renegotiation record. This function does not match a caller that
 * waits in its home direction. Such a caller waits for real, exactly as it
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
/* While this is not 0, the first ctls_conn_read of each read dispatch of
 * Tier 2 asks for at most this many bytes. A test can therefore leave part
 * of a record inside the TLS layer on purpose, which a real peer does only
 * through record sizes that the test does not control. */
static _Atomic size_t g_async_tls_first_read_cap_for_tests = 0;

void _chttp_set_async_tls_first_read_cap_for_tests(size_t cap) {
  atomic_store(&g_async_tls_first_read_cap_for_tests, cap);
}
#endif

/* This gives the direction that a blocked TLS call needs. It is a thin layer
 * over ctls_conn_wants_write. A test build can therefore inject the
 * condition that the comment above describes. An ordinary build compiles to
 * the bare call. */
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
 * poll() can legitimately return -1 with EINTR. This happens if a signal
 * arrives before any fd becomes ready. It is more frequent under tools such
 * as valgrind, which use signals inside. But it is a real possibility in any
 * process. This function retries inside itself. It tracks the time that
 * passes against the original timeout budget. Repeated interruptions can
 * therefore not extend the wait that the caller asked for.
 *
 * A POLLERR or POLLNVAL on the fd returns error_rv. A wait of the connect
 * phase (the connect itself and the TLS handshake) passes
 * ccol_http_connection_failed. A wait on an established connection, while a
 * request is sent or a response is read, passes ccol_http_transfer_aborted,
 * which is what Tier 2 and Tier 3 report for the same event. */
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
    int rc = poll(&pfd, 1, remaining);
    if (rc < 0) {
      if (errno == EINTR) {
        if (has_timeout) {
          struct timespec now;
          clock_gettime(CLOCK_MONOTONIC, &now);
          int64_t elapsed_ms =
              ((int64_t)now.tv_sec - (int64_t)start.tv_sec) * 1000 +
              ((int64_t)now.tv_nsec - (int64_t)start.tv_nsec) / 1000000;
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
  /* This is the direction to wait on before the NEXT attempt. It starts as
   * POLLOUT, which matches the contract of a plain send(2). But the library
   * derives it again after every EWOULDBLOCK from a TLS connection. An
   * EWOULDBLOCK from ctls_conn_write() does not always mean "wait for
   * writable". OpenSSL can need to READ before this exact call can make
   * progress. A TLS 1.2 renegotiation or a TLS 1.3 KeyUpdate causes that.
   * See the doc comment of ctls_conn_wants_write. A poll on POLLOUT there
   * with no condition is a busy loop and not a wait. The socket is still
   * writable, so poll(2) returns at once. The next write attempt then blocks
   * on the same missing read. This burns the thread at 100% CPU. The default
   * request_timeout_us is 0, so there is no deadline to end it either. */
  short want_events = POLLOUT;
  while (sent < len) {
    int wait_ms;
    if (!_deadline_remaining_ms(overall, &wait_ms)) return ccol_timed_out;
    ccol_retval_t prv =
        _conn_wait(conn->fd, want_events, wait_ms, ccol_http_transfer_aborted);
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
    /* The write made progress. Whatever blocked the previous attempt is
     * therefore satisfied. The next attempt starts from the home direction
     * of this call again. It derives the direction again only if it blocks
     * in its turn. */
    want_events = POLLOUT;
  }
  return ccol_success;
}

/* This applies TCP_NODELAY to fd as a best effort. A failure here is never
 * fatal to the connection itself. It only loses an optimisation of the
 * latency. The Tier 1 and Tier 2 connect paths share this function, so both
 * apply the option in the same way. Neither sets its own socket options. The
 * option has no meaning for a unix domain socket, which has no TCP layer. A
 * caller therefore calls this only for an AF_INET or AF_INET6 connection. */
static void _apply_tcp_nodelay(int fd) {
  int one = 1;
  (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

/* One resolved address of a TCP target, copied out of the getaddrinfo()
 * result so that it can outlive that result. */
typedef struct {
  struct sockaddr_storage addr;
  socklen_t addr_len;
  int protocol;
} chttp_resolved_addr_t;

/* The candidate addresses of one TCP target. _addr_list_resolve keeps the
 * order of getaddrinfo(), which is the destination address selection of
 * RFC 6724 that the resolver of the C library applies, and
 * _addr_list_interleave then alternates the address families, starting
 * with the family of the first address (RFC 8305 section 4). An IPv6
 * address comes first whenever the resolver prefers IPv6, which is its
 * default where the host has IPv6 connectivity.
 *
 * Every tier connects through one policy, Happy Eyeballs (RFC 8305). The
 * first candidate starts at once. The next one starts when the attempts in
 * flight have given no result for CHTTP_CONNECT_ATTEMPT_DELAY_MS, or at
 * once when an attempt fails. The first attempt that connects wins, and
 * every other attempt is closed. The connect deadline is ONE budget for the
 * whole race and is never renewed for a candidate. When it expires, the
 * race ends with ccol_timed_out. The race fails with
 * ccol_http_connection_failed only after every candidate failed.
 *
 * An address that does not answer at all, an IPv6 route that drops its
 * packets for example, therefore costs a quarter of a second and not the
 * whole connect timeout. A server that listens on 127.0.0.1 alone is also
 * reached as "localhost", which a standard /etc/hosts lists as ::1 ahead of
 * 127.0.0.1: ::1 refuses at once, and 127.0.0.1 starts right away. */
typedef struct {
  size_t count;
  chttp_resolved_addr_t addrs[];
} chttp_addr_list_t;

/* This resolves host:port into a new list that the library allocates from
 * mp. It returns ccol_http_host_resolution_failed when the name resolves to
 * nothing. */
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

/* This reorders l so that the address families alternate, starting with
 * the family of the first address, and keeps the order of the resolver
 * inside each family (RFC 8305 section 4, with a First Address Family
 * Count of 1). A list holds a handful of addresses, so the quadratic
 * rotation costs nothing that matters, and it needs no allocation. */
static void _addr_list_interleave(chttp_addr_list_t *l) {
  if (l->count < 3)
    return; /* Two addresses already alternate or share
             * one family. */
  sa_family_t first = l->addrs[0].addr.ss_family;
  for (size_t i = 1; i < l->count; i++) {
    bool want_first = (i % 2) == 0;
    size_t j = i;
    while (j < l->count &&
           ((l->addrs[j].addr.ss_family == first) != want_first))
      j++;
    if (j == l->count)
      break; /* One family is used up; the rest keeps its
              * order. */
    if (j != i) {
      chttp_resolved_addr_t moved = l->addrs[j];
      memmove(&l->addrs[i + 1], &l->addrs[i], (j - i) * sizeof(l->addrs[0]));
      l->addrs[i] = moved;
    }
  }
}

/* This starts a non-blocking connect to a. It returns the fd, with
 * *in_progress true when the connect is pending (EINPROGRESS) and false when
 * it is already complete, or -1 when socket() or connect() fails at once.
 *
 * Every socket of this module is close-on-exec. A child that the process
 * spawns (system(), posix_spawn(), fork() and exec()) must not inherit a
 * connection: the peer would then see no FIN when this process closes it,
 * for as long as the child lives, and the child could read and write the
 * stream. */
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
/* A white-box hook. An attempt on an address in this set gets, in place of
 * a connecting TCP socket, one end of a Unix socket pair whose send buffer
 * is full and whose peer stays open and silent: it never becomes writable
 * and never reports an error, and a shutdown(2) of it reports EOF in both
 * directions, which is exactly how a TCP connect whose SYN is dropped
 * behaves (Linux tests build that shape from a listener with a full accept
 * queue; the BSDs and macOS answer such a SYN). The hook keeps every peer
 * end open until the set is emptied. A NULL address empties the set, and so
 * does clearing the address override. */
enum { SILENT_ADDRS_FOR_TESTS = 4, SILENT_PEERS_FOR_TESTS = 32 };
static struct sockaddr_storage g_silent_addrs_for_tests[SILENT_ADDRS_FOR_TESTS];
static socklen_t g_silent_lens_for_tests[SILENT_ADDRS_FOR_TESTS];
static atomic_int g_silent_count_for_tests;
/* The peer ends, each stored as fd + 1 so that 0 means an empty slot. */
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

/* A white-box hook. While it is non-NULL, _addr_list_resolve_for_target
 * answers every TCP resolution from this list, in this order, in place of
 * getaddrinfo(). A test can therefore present a candidate list whose first
 * address refuses, whatever the /etc/hosts of the machine says. */
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
/* A white-box hook. It builds a list whose address i has the family
 * families[i] and the port i, runs _addr_list_interleave over it, and
 * writes the resulting order of the original indexes to order_out. */
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

/* This is the resolution step that every tier calls. A test build can
 * replace its answer; see _chttp_set_addr_override_for_tests. */
static ccol_retval_t _addr_list_resolve_for_target(ccol_memmgmt_procs_t *mp,
                                                   const char *host,
                                                   uint16_t port,
                                                   chttp_addr_list_t **out) {
#ifdef RUNNING_UNIT_TESTS
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

/* This returns the milliseconds from now until `at`, rounded up, or 0 when
 * `at` has passed. */
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

/* This resolves host:port and connects with the Happy Eyeballs race of
 * chttp_addr_list_t. It combines the deadline of the connect and the
 * overall deadline of the request. The tighter of the two wins. */
static ccol_retval_t _tcp_connect(ccol_memmgmt_procs_t *mp, const char *host,
                                  uint16_t port, chttp_deadline_t *connect_dl,
                                  chttp_deadline_t *overall, int *fd_out) {
  chttp_addr_list_t *list = NULL;
  ccol_retval_t rrv = _addr_list_resolve_for_target(mp, host, port, &list);
  if (rrv != ccol_success) return rrv;
  _addr_list_interleave(list);

  /* One slot for each attempt that can be in flight at once. A resolver
   * rarely gives more than a few addresses, and those fit on the stack. */
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
    /* Start the next candidate when nothing is in flight, or when the
     * attempts in flight have had their delay. A candidate that fails at
     * once gives way to the one after it at once. */
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
      /* A failed attempt hands over to the next candidate at once. */
      next_start = now;
    }
    npend = kept;
    if (winner >= 0) {
      result = ccol_success;
      break;
    }
  }

  /* Every attempt that did not win is closed. */
  for (size_t i = 0; i < npend; i++) close(pfds[i].fd);
  if (pfds != stack_pfds) _ccol_mem_free(mp, pfds);
  _ccol_mem_free(mp, list);
  if (result == ccol_success) {
    _apply_tcp_nodelay(winner);
    *fd_out = winner;
  }
  return result;
}

/* This connects to a unix domain stream socket at path. It honours the same
 * connect deadline and overall deadline as _tcp_connect. There is no
 * TCP_NODELAY and no DNS here. A unix domain connect() almost never returns
 * EINPROGRESS on Linux, because the kernel services the accept queue
 * synchronously. But this function keeps the non-blocking socket and the
 * poll() step. This keeps it portable, and it honours the deadline even in
 * that rare case. */
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

  int wait_ms, overall_ms;
  if (!_deadline_remaining_ms(connect_dl, &wait_ms)) {
    close(fd);
    return ccol_timed_out;
  }
  if (!_deadline_remaining_ms(overall, &overall_ms)) {
    close(fd);
    return ccol_timed_out;
  }
  ccol_retval_t prv = _conn_wait(fd, POLLOUT, _combine_ms(wait_ms, overall_ms),
                                 ccol_http_connection_failed);
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

/* This drives the client-mode TLS handshake to its end. It honours both the
 * connect deadline and the overall deadline. The time of the TLS handshake
 * counts against the connect timeout, which matches the behaviour of
 * curl. */
static ccol_retval_t _tls_handshake(chttp_conn_t *conn,
                                    chttp_deadline_t *connect_dl,
                                    chttp_deadline_t *overall) {
  for (;;) {
    ctls_handshake_result_t r = ctls_conn_handshake_step(conn->tls);
    if (r == CTLS_HANDSHAKE_DONE) return ccol_success;
    if (r == CTLS_HANDSHAKE_ERROR) {
      /* By the convention of OpenSSL, 0 is X509_V_OK. ctls.h deliberately
       * does not show the OpenSSL headers to a caller. This code therefore
       * compares the raw value directly, and not through the X509_V_OK
       * symbol. */
      long vr = ctls_conn_verify_result(conn->tls);
      return (vr != 0) ? ccol_http_tls_cert_verification_failed
                       : ccol_http_tls_handshake_failed;
    }

    short ev = (r == CTLS_HANDSHAKE_WANT_WRITE) ? POLLOUT : POLLIN;
    int wait_ms, overall_ms;
    if (!_deadline_remaining_ms(connect_dl, &wait_ms)) return ccol_timed_out;
    if (!_deadline_remaining_ms(overall, &overall_ms)) return ccol_timed_out;
    ccol_retval_t prv =
        _conn_wait(conn->fd, ev, _combine_ms(wait_ms, overall_ms),
                   ccol_http_connection_failed);
    if (prv != ccol_success) return prv;
  }
}

/* This creates the client side of a TLS connection over fd, and gives the
 * code that the request reports when that fails. ctls_conn_create_client
 * refuses to verify a name when it has none. That is not a shortage of
 * memory: the host of the URL cannot be checked against a certificate, which
 * makes the URL unusable for https, so it reports ccol_http_invalid_url.
 * ctls describes a failure only through its static err_str, so the text of
 * that refusal is what this function reads. Every other failure of ctls
 * here is an allocation inside OpenSSL or ctls, which is
 * ccol_not_enough_memory, or arguments that ctls refuses, which is an
 * internal fault of this module. */
static ccol_retval_t _tls_client_create_failure_code(const char *err) {
  if (!err) return ccol_not_enough_memory;
  if (strstr(err, "verification requested with no hostname"))
    return ccol_http_invalid_url;
  if (strstr(err, "invalid arguments")) return ccol_unexpected_failure;
  return ccol_not_enough_memory;
}

/* A fully qualified name may end in one dot, as in "www.example.com.". The
 * dot pins the name to the root of the DNS, and the connect keeps it for
 * that reason. A certificate never names a host with that dot, and the SNI
 * extension forbids it (RFC 6066 section 3). This function therefore strips
 * exactly one trailing dot for the name that TLS sends and verifies, as curl
 * and Go's crypto/tls do. The Host header keeps the name as the URL gives
 * it. A DNS name is at most 253 characters, and 254 with its dot, which the
 * buffer holds; a longer host is no DNS name, and it goes to TLS unchanged,
 * where no certificate can match it. */
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
/* A white-box helper. It runs _tls_client_create over one end of a fresh
 * socket pair, with a TLS context of its own, and gives back the code that
 * a request would report. No URL that the parser accepts has an empty
 * host, so this is the only way to reach the refusal of ctls. */
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

/* A white-box helper. It runs a full handshake over a fresh socket pair.
 * The client side is _tls_client_create for host, with name verification,
 * and it trusts ca_path alone. The server side presents default_cert as its
 * default certificate, and named_cert under the SNI name named_as. It gives
 * back true when both sides finish the handshake. */
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

/* tls_generation is the snapshot that the caller takes of
 * chttpclient.tls_generation. The caller takes it under cli->lock, together
 * with tls_ctx itself. This function stamps it onto the connection that it
 * makes. The idle pool can later tell whether the handshake of that
 * connection ran under the configuration that is still in force. See the
 * field comment of chttpclient.tls_generation. The caller passes this value
 * in, and this function does not read it here. This function deliberately
 * knows nothing about the client that owns the connection that it opens. */
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
 * This acquires a slot of the concurrency limiter. It blocks while the pool
 * is at its capacity. `deadline` bounds how long this call blocks for a
 * slot. A NULL deadline and an inactive chttp_deadline_t both mean no limit.
 * If the deadline passes first, this function returns ccol_timed_out and
 * does not wait forever.
 *
 * This is what keeps the documented contract of
 * chttpclient_set_request_timeout true when the pool is full. That contract
 * is "the maximum time from when chttpclient_do is called...".
 * chttpclient_set_pool_size sets the size of that pool. The caller of this
 * function is chttp_do_internal. It computes the overall deadline BEFORE
 * this call, so the time of this wait counts against that deadline. An
 * unconditional ccol_cond_var_wait would hide that time from the deadline.
 *
 * _ccol_cond_var_init_monotonic initializes cli->available against
 * CLOCK_MONOTONIC. deadline->deadline is itself a CLOCK_MONOTONIC timespec
 * from _deadline_make. This code can therefore hand it to
 * ccol_cond_var_timedwait directly.
 */
static ccol_retval_t _slot_acquire(struct chttpclient *cli,
                                   const chttp_deadline_t *deadline) {
  ccol_mutex_lock(cli->lock);
  if (cli->destroying) {
    ccol_mutex_unlock(cli->lock);
    return ccol_not_permitted;
  }
  if (!cli->pool_initialized) {
    cli->pool_cap = _resolve_pool_cap(cli->configured_pool_size);
    cli->pool_initialized = true;
  }
  while (cli->in_flight_count >= cli->pool_cap && !cli->destroying) {
    if (deadline && deadline->active) {
      int wrc = ccol_cond_var_timedwait(cli->available, cli->lock,
                                        deadline->deadline);
      if (wrc == ETIMEDOUT) {
        ccol_mutex_unlock(cli->lock);
        return ccol_timed_out;
      }
      /* Every other outcome falls through and checks the loop condition
       * above again. A real wake and a spurious one are both such outcomes.
       * ccol_cond_var_wait handles a spurious wakeup in the same way. */
    } else {
      ccol_cond_var_wait(cli->available, cli->lock);
    }
  }
  if (cli->destroying) {
    ccol_mutex_unlock(cli->lock);
    return ccol_not_permitted;
  }
  cli->in_flight_count++;
  ccol_mutex_unlock(cli->lock);
  return ccol_success;
}

static void _slot_release(struct chttpclient *cli) {
  ccol_mutex_lock(cli->lock);
  cli->in_flight_count--;
  ccol_cond_var_broadcast(cli->available);
  ccol_mutex_unlock(cli->lock);
}

/* ========================================================================== */
/*                         KEEP-ALIVE IDLE POOL                               */
/* ========================================================================== */

/* The SSO storage of chmap_entry is naturally aligned. This memcpy is
 * therefore a defence in depth and not a live alignment requirement. It
 * stays here for consistency with the chmap-backed pointer storage of cjson,
 * cyaml, clrucache and cthreadcomm, which use the same pattern. */
static inline cvec _read_cvec(const void *src) {
  cvec v;
  memcpy(&v, src, sizeof(v));
  return v;
}

/*
 * This tries to pop a usable idle connection for `origin_key` into *out. It
 * returns false if there is none. It can pop and discard several stale or
 * dead candidates before it finds a live one or empties the list. The age
 * check and the liveness probe both run OUTSIDE the lock of the client. The
 * library never does I/O while it holds that lock.
 */
static bool _idle_pool_take(struct chttpclient *cli, const char *origin_key,
                            chttp_conn_t *out) {
  for (;;) {
    bool got = false;
    uint64_t cur_tls_generation;
    ccol_mutex_lock(cli->lock);
    cur_tls_generation = cli->tls_generation;
    if (cli->idle_pools) {
      cmap_pair kp = {.ptr = (void *)origin_key,
                      .size = strlen(origin_key) + 1};
      const cmap_pair *vp = NULL;
      if (chmap_get_elem_ref(cli->idle_pools, &kp, &vp) == ccol_success && vp) {
        cvec list = _read_cvec(vp->ptr);
        if (list && cvector_elem_count(list) > 0 &&
            cvector_pop_back(list, out) == ccol_success) {
          got = true;
          if (cli->idle_total_count > 0) cli->idle_total_count--;
          /* This removes the list and the map entry of this origin, which
           * are now empty. It does not leave them behind forever. A client
           * with a long life can contact many distinct origins. A crawler or
           * an outbound-heavy service does this. Without this prune, such a
           * client keeps one permanent chmap entry, plus an empty cvec, for
           * each origin that it EVER pooled a connection for.
           * CHTTP_MAX_IDLE_TOTAL and CHTTP_MAX_IDLE_PER_ORIGIN do not bound
           * that, because they bound live connections only and never
           * distinct origin keys. _idle_pool_offer already handles the case
           * of an origin with no entry yet. It makes a new one. See its own
           * code a few lines below. The removal of an empty entry here is
           * therefore always safe. The next offer for this origin builds the
           * entry again, exactly as it does for a completely new origin. */
          if (cvector_elem_count(list) == 0) {
            __cvector_destroy(list);
            chmap_delete_elem(cli->idle_pools, &kp);
          }
        }
      }
    }
    ccol_mutex_unlock(cli->lock);
    if (!got) return false;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t age_ms =
        ((int64_t)now.tv_sec - (int64_t)out->last_used.tv_sec) * 1000 +
        ((int64_t)now.tv_nsec - (int64_t)out->last_used.tv_nsec) / 1000000;

    /* A later chttpclient_set_tls call can replace the configuration that a
     * TLS connection ran its handshake under. This code refuses such a
     * connection here, and the code below tears it down. A connection that
     * is too old, or that the peer closed, gets the same treatment. Both
     * idle pools use the origin alone as their key. This check is therefore
     * the only thing that stops the library from answering a request over a
     * connection that it made under the old policy. Without it, that can
     * continue for up to CHTTP_IDLE_MAX_AGE_MS after the change. Such a
     * change can pin a private CA in place of the system bundle. It can turn
     * verification back on. It can rotate the client
     * certificate. */
    bool tls_config_current =
        (!out->tls || out->tls_generation == cur_tls_generation);

    bool alive = false;
    if (age_ms <= CHTTP_IDLE_MAX_AGE_MS && tls_config_current) {
      char probe;
      ssize_t pn;
      if (out->tls) {
        /* A raw MSG_PEEK on out->fd peeks wire bytes that are still
         * encrypted. It goes around OpenSSL completely. A peer can send
         * something at the TLS record layer after the library reads the last
         * response. A TLS 1.3 NewSessionTicket is the most common case, and
         * OpenSSL servers send one right after the handshake or the
         * response. The connection is healthy, but a raw peek reports "data
         * available". This probe then declares the connection dead and
         * discards it. HTTPS keep-alive reuse stops working, and nothing
         * reports it. A call through ctls_conn_read lets OpenSSL absorb and
         * process such a protocol-only record instead. The liveness check of
         * the async engine already does the same. That check is the
         * CHTTP_ASYNC_DISPATCH_IDLE branch of _async_on_readable_impl. This
         * is a real read and not a peek, and that is harmless here. If it
         * returns more than 0, real application data was waiting. This
         * connection then goes away and the library never reuses it, so the
         * consumption of that byte has no visible effect. If it returns
         * EWOULDBLOCK, it consumes nothing, and the connection stays fully
         * intact for reuse. */
        pn = ctls_conn_read(out->tls, &probe, 1);
      } else {
        /* This retries on EINTR. Every other syscall wrapper in this file
         * that must be safe against a signal does the same. _conn_read,
         * _conn_write and _conn_wait are those wrappers. A signal that
         * interrupts this MSG_DONTWAIT peek reports errno == EINTR. That
         * value matches neither EAGAIN nor EWOULDBLOCK below. Without the
         * retry, this code declares a healthy connection dead and discards
         * it in place of a reuse. */
        do {
          pn = recv(out->fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
        } while (pn < 0 && errno == EINTR);
      }
      alive = (pn < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    }
    if (alive) return true;

    _conn_teardown(cli->m_procs, out);
    /* The loop tries the next candidate for this origin, if there is one. */
  }
}

/*
 * This offers a connection that is still good back to the idle pool. A cap
 * for each origin and a total cap bound the pool. If the connection does not
 * fit, this function closes it instead. It does not fit when a cap is full,
 * or when the library is destroying the client. This is a lost chance for an
 * optimisation, never a correctness problem.
 */
static void _idle_pool_offer(struct chttpclient *cli, chttp_conn_t *c) {
  bool pooled = false;
  ccol_mutex_lock(cli->lock);
  if (!cli->destroying && cli->idle_pools &&
      cli->idle_total_count < CHTTP_MAX_IDLE_TOTAL) {
    cmap_pair kp = {.ptr = c->origin_key, .size = strlen(c->origin_key) + 1};
    const cmap_pair *vp = NULL;
    cvec list = NULL;
    if (chmap_get_elem_ref(cli->idle_pools, &kp, &vp) == ccol_success && vp) {
      list = _read_cvec(vp->ptr);
    }
    /* A genuinely new origin gets a fresh entry only while the cap on
     * distinct origins still has room. See the comment of
     * CHTTP_MAX_IDLE_ORIGINS. At the cap, list stays NULL. The code then
     * falls through to the same "it does not fit" path that every other
     * pooling failure below already uses. */
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
  ccol_mutex_unlock(cli->lock);
  if (!pooled) _conn_teardown(cli->m_procs, c);
}

/* ========================================================================== */
/*                         REQUEST SERIALIZATION                              */
/* ========================================================================== */

/*
 * This records the presence of each header that _serialize_request adds a
 * default for. _scan_header_presence below reads req->headers in one pass
 * and ignores case.
 *
 * A plain point lookup with chmap_get_elem_ref keys on a lower-cased query
 * string, which matches chttp_request_get_header. Such a lookup misses a
 * borrowed map whose keys use natural case, such as "Content-Type" or
 * "Authorization". A caller can build such a map directly and hand it to
 * chttp_run_query, and that map never goes through the lower-case step of
 * chttp_request_set_header. A miss there makes chttp_run_query give back a
 * wire request that carries both the header of the caller and a duplicate
 * that the library added. For Host it does something else. The
 * header-emission loop below always skips any "host" match that ignores
 * case. The value of the caller therefore disappears with no report. Every
 * field below comes from this one scan that ignores case. No field can
 * therefore gain that asymmetry again.
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
} chttp_header_presence_t;

static chttp_header_presence_t _scan_header_presence(chmap headers) {
  chttp_header_presence_t p = {0};
  if (!headers) return p;
  cmap_iterator *it = chashmap_begin_iter(headers, NULL);
  for (; it; it = it->_next_fn(it)) {
    const char *name = (const char *)it->key_pair->ptr;
    if (strcasecmp(name, "host") == 0)
      p.has_host = true;
    else if (strcasecmp(name, "accept") == 0)
      p.has_accept = true;
    else if (strcasecmp(name, "user-agent") == 0)
      p.has_user_agent = true;
    else if (strcasecmp(name, "content-length") == 0)
      p.has_content_length = true;
    else if (strcasecmp(name, "content-type") == 0)
      p.has_content_type = true;
    else if (strcasecmp(name, "authorization") == 0)
      p.has_authorization = true;
    else if (strcasecmp(name, "expect") == 0)
      p.has_expect = true;
    else if (strcasecmp(name, "transfer-encoding") == 0)
      p.has_transfer_encoding = true;
  }
  return p;
}

/*
 * This tracks which header NAMES the header-emission loop of
 * _serialize_request below already sent. It ignores case. req->headers uses
 * the exact bytes of a key as its key. A map that a caller builds and lends
 * to the library can therefore hold two keys that differ as bytes. The two
 * keys can still be the SAME header name under the case-insensitive rules
 * of HTTP. "Host" and "host" are such a pair. The map holds them as two
 * distinct entries.
 * The lower-case step of chttp_request_set_header stops this for the
 * documented API that sets a header. A borrowed map that a caller builds
 * directly gives no such guarantee. The headers parameter of chttp_run_query
 * is one such map. A caller that writes chttp_request_t.headers itself
 * builds another.
 *
 * Without this tracker, both entries reach the wire as two separate header
 * lines. For an ordinary custom header that is only unusual, and RFC 7230
 * SS3.2.2 lets a sender join repeated fields with the same name. But for
 * "Host" it builds a request that RFC 7230 SS5.4 tells a server to reject.
 * That section lists "more than one Host header field" as a 400 Bad Request
 * condition. This is a real protocol violation that a remote peer can see.
 * It is not a question of style. Real request-smuggling and cache-poisoning
 * techniques use this same duplicate-Host shape.
 *
 * `names` holds borrowed pointers into the key storage of req->headers.
 * Those pointers stay valid for the whole emission loop, because chmap
 * iteration never changes or moves an entry that exists. The library fills
 * `names` in iteration order. For the separate-chaining chmap of this
 * codebase, that order puts the newest insertion first. See the
 * "Reverse-insertion-order iteration" documentation of chashmap.c. The loop
 * therefore checks for an earlier sighting before it adds a name, and it
 * skips a name on a hit. This keeps exactly the newest occurrence of each
 * name that ignores case, and it drops every older duplicate. One update
 * through chmap_insert_elem with an exact key behaves in the same way: the
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
    if (strcasecmp(seen->names[i], name) == 0) return true;
  return false;
}

/* This records that the loop sent name. It returns false only when an
 * allocation fails. That allocation is the growable backing array itself. It
 * is never a copy of the bytes of name, because name is borrowed and it
 * outlives this whole call. */
static bool _seen_names_add(ccol_memmgmt_procs_t *mp, chttp_seen_names_t *seen,
                            const char *name) {
  if (seen->count == seen->cap) {
    /* This guards the growth computation below, which doubles a size. This
     * project uses the same guard for every "curr_size *= 2" computation.
     * cvector, csort and cmempool use it. So do _ob_append and
     * _sink_buffered in this file. Without it, a seen->cap that is already
     * astronomical can double past SIZE_MAX. The later multiplication by
     * sizeof(*nn) can also overflow on its own. Either one can settle on a
     * small nc, or on zero. The header-emission loop above then treats that
     * array as one that grew correctly, although it is too small. The loop
     * then writes seen->names[seen->count++] past the real bounds. No real
     * input reaches this guard, because it needs about 2^61 distinct header
     * names on one request. But this project treats an unguarded overflow in
     * a size computation as a real bug. The size of the input that triggers
     * it does not matter. */
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
 * A white-box test helper. It exposes the overflow guard of _seen_names_add
 * directly. It builds a chttp_seen_names_t whose count and cap already sit
 * at fake_cap. names stays NULL. The library never allocates or touches a
 * real backing array of that size. The guard must reject the call before
 * the realloc call of the growth branch. It must also reject before
 * the seen->names[seen->count++] write below that branch. The helper then
 * calls _seen_names_add once.
 *
 * This follows the same pattern that this project uses for this class of
 * overflow guard: assert that the guard rejects before any real work
 * happens. _chttp_ob_append_overflow_guard_for_tests and
 * _chttp_percent_encode_unix_path_overflow_guard_for_tests do the same. This
 * helper is not part of the public API. A gate keeps this symbol out of a
 * production build of libccollections.so. Every other white-box helper in
 * this file has the same gate.
 */
bool _chttp_seen_names_add_overflow_guard_for_tests(ccol_memmgmt_procs_t *mp,
                                                    size_t fake_cap) {
  chttp_seen_names_t seen = {.names = NULL, .count = fake_cap, .cap = fake_cap};
  bool added = _seen_names_add(mp, &seen, "x");
  /* A call that the guard does not reject is an ordinary call. It really
   * grows seen.names with realloc. This helper frees that array here and
   * does not leak it. The struct of this helper is a local that goes away. A
   * caller of the real _seen_names_add is never responsible for that array.
   * The header-emission loop of _serialize_request always frees seen.names
   * itself. */
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

static void _ob_append(chttp_outbuf_t *b, const char *data, size_t n) {
  if (b->oom) return;
  /* This guards the computation of the needed size below against an
   * overflow. The other growable buffers of this project use the same
   * SIZE_MAX-relative guard for the same class of computation. cvector,
   * csort and cmempool are those buffers. Without this guard, a body from
   * the caller can be large enough to make b->len + n + 1 wrap past
   * SIZE_MAX. The doubling loop below then settles on an `nc` that is far
   * too small. Or `nc` itself wraps to 0 inside the loop, and the loop spins
   * forever, because 0 is always less than a target that is not zero. The
   * memcpy that follows then becomes a real heap buffer overflow, in place
   * of a clean allocation failure that the library reports. No real input
   * reaches this guard, because it needs a request body of many exabytes in
   * memory before any call to this function. But this project treats an
   * unguarded overflow in a size computation as a real bug. The size of the
   * input that triggers it does not matter. */
  if (n > SIZE_MAX - b->len || b->len + n > SIZE_MAX - 1) {
    b->oom = true;
    return;
  }
  size_t need = b->len + n + 1;
  if (need > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 1024;
    while (nc < need) {
      if (nc > SIZE_MAX / 2) {
        b->oom = true;
        return;
      }
      nc *= 2;
    }
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
 * A white-box test helper. It exposes the overflow guard of _ob_append
 * directly. It sets up a chttp_outbuf_t whose `len` already sits at
 * fake_len. That length is not real. The library never allocates a buffer of
 * that size. A chttp_outbuf_t starts with buf == NULL and cap == 0 in every
 * case. The guard must therefore reject the append before it touches the
 * malloc or realloc of mp. The helper appends up to 64 bytes. A compile-time
 * cap on n keeps a small, real backing buffer enough. The helper returns
 * whether _ob_append set b->oom.
 *
 * The caller passes a counting allocator and checks its call count
 * separately. This follows the pattern that this project uses for this class
 * of overflow guard: assert that the library never called the allocator. The
 * default comparators of csort and cmempool are examples. This helper is not
 * part of the public API. A gate keeps this symbol out of a production build
 * of libccollections.so. Every other white-box helper in this file has the
 * same gate.
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
#endif /* RUNNING_UNIT_TESTS */

/*
 * The headers that a caller sets with chttp_request_set_header carry no
 * origin of their own. A redirect can take the request to a server that the
 * caller never named. These two flags say which of those headers the
 * library leaves off a hop for that reason. Every tier computes them again
 * for each hop, against the origin of the FIRST request and never against
 * the previous hop, which is the rule of curl. A hop that returns to the
 * first origin therefore sends those headers again, and a hop that stays on
 * a foreign origin keeps leaving them off.
 *
 * CHTTP_REDIRECT_DROP_CREDENTIALS: the scheme, the host or the port of a
 * hop differs from the first request. The caller's Authorization, Cookie,
 * Cookie2 and WWW-Authenticate headers are not sent. This is the rule of
 * curl for the same case (CVE-2018-1000007 and CVE-2022-27776).
 *
 * CHTTP_REDIRECT_DROP_HOST: the host of a hop differs from the first
 * request. The caller's Host header is not sent, and the library writes its
 * own Host line for the new target. A Host header that names the first
 * server is wrong for another one. curl applies the same rule.
 */
#define CHTTP_REDIRECT_DROP_CREDENTIALS 0x1u
#define CHTTP_REDIRECT_DROP_HOST 0x2u

/* This finds the host part of an origin_key: the text between "://" and the
 * port for a TCP key, and the whole socket path for a "unix://" key. */
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

/* This is true when two origin_keys name the same origin for the rules
 * that move credentials across a redirect. A TCP origin compares its host
 * without regard to case, as DNS names and curl do. Its scheme is always
 * the lower-case "http" or "https", and its port is decimal digits, so one
 * case-insensitive comparison of the whole key compares the scheme and the
 * port exactly. A "unix://" key compares exactly, because a socket path is
 * a file name and its case is significant. */
static bool _origin_keys_same(const char *a, const char *b) {
  bool a_unix = strncmp(a, "unix://", 7) == 0;
  bool b_unix = strncmp(b, "unix://", 7) == 0;
  if (a_unix || b_unix) return a_unix == b_unix && strcmp(a, b) == 0;
  return strcasecmp(a, b) == 0;
}

/* This returns the CHTTP_REDIRECT_DROP_* flags that a hop to
 * `origin_key` needs, against `initial_origin_key`, the origin of the first
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
    same_host = strncasecmp(a, b, alen) == 0;
  }
  if (!same_host) drops |= CHTTP_REDIRECT_DROP_HOST;
  return drops;
}

/* This is true for a header name that CHTTP_REDIRECT_DROP_CREDENTIALS
 * removes. */
static bool _is_redirect_credential_header(const char *name) {
  return strcasecmp(name, "authorization") == 0 ||
         strcasecmp(name, "cookie") == 0 || strcasecmp(name, "cookie2") == 0 ||
         strcasecmp(name, "www-authenticate") == 0;
}

/*
 * This serializes the method line, the headers and the body into one wire
 * buffer. The caller is responsible for the change of the method and the
 * body on a redirect hop. The caller does that through the method and body
 * fields of `req`. `req` is a shallow view for one hop. It is not the
 * original request object of the caller.
 *
 * out_presence is optional, and NULL is fine. If it is not NULL, it receives
 * the same chttp_header_presence_t that this function already computes
 * inside, from req->headers. This function uses that value to decide which
 * headers to add and which duplicates to drop. out_presence exists so that a
 * caller that also needs one of these flags pays for no second scan. Such a
 * scan is O(header count) over the same header map, through a separate
 * _scan_header_presence call. The hop loop of Tier 1 needs has_expect. It
 * uses that flag to decide whether this hop goes through the wait for
 * "Expect: 100-continue".
 */
static ccol_retval_t _serialize_request(ccol_memmgmt_procs_t *mp,
                                        const chttp_request_t *req,
                                        const chttp_url_t *url,
                                        const char *auto_authorization,
                                        unsigned redirect_drops, char **out_buf,
                                        size_t *out_len,
                                        chttp_header_presence_t *out_presence) {
  chttp_outbuf_t ob = {.mp = mp};

  _ob_append_cstr(&ob, chttp_method_str(req->method));
  _ob_append(&ob, " ", 1);
  _ob_append_cstr(&ob, url->path_and_query);
  _ob_append_cstr(&ob, " HTTP/1.1\r\n");

  chttp_header_presence_t hp = _scan_header_presence((chmap)req->headers);
  if (hp.has_transfer_encoding) {
    /* chttp_request_set_header makes the same rejection. See the doc comment
     * of that function. This check is the real backstop. req->headers is an
     * internal chmap handle. A caller can still build such a handle and
     * assign it directly, and that goes around chttp_request_set_header
     * completely. The borrowed map of chttp_run_query is one case. A caller
     * that writes chttp_request_t.headers itself is another. This check
     * catches the header here. Without it, the Content-Length code further
     * down pairs the header with a Content-Length header that conflicts with
     * it, over a body that carries no chunk encoding. */
    _ccol_mem_free(mp, ob.buf);
    return ccol_invalid_args;
  }
  /* A Host header of the caller that CHTTP_REDIRECT_DROP_HOST removes counts
   * as absent, so the library writes its own Host line for this hop. */
  bool has_host = hp.has_host && !(redirect_drops & CHTTP_REDIRECT_DROP_HOST);
  bool has_accept = hp.has_accept;
  bool has_ua = hp.has_user_agent;
  bool has_cl = hp.has_content_length;
  bool has_ct = hp.has_content_type;
  /* CHTTP_REDIRECT_DROP_CREDENTIALS removes an "authorization" header that
   * the original caller set: a redirect crossed to a different origin than
   * the one that this header was set against. This code then treats that
   * header as one that was never there at all. That decides
   * whether to add auto_authorization below. The header loop further down
   * separately skips the write of that header from req->headers.
   *
   * Without this, a caller can set an explicit Authorization for the
   * original origin. The URL of the redirect target can then also carry
   * "user:pass@" userinfo of its own. That hop would carry NO Authorization
   * header at all. The correct header is the one that comes from the
   * userinfo of the new origin. */
  bool has_auth = hp.has_authorization &&
                  !(redirect_drops & CHTTP_REDIRECT_DROP_CREDENTIALS);

  if (!has_host) {
    if (url->is_unix) {
      /* A unix-domain-socket target has no real hostname and no real port
       * to send. "localhost" matches the default of the unix-socket option
       * of curl. It is simple, and any server on the other end can expect
       * it. RFC 7230 SS5.4 still needs a Host header on every HTTP/1.1
       * request, although its value has no meaning here. */
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
    /* This loop skips a "host" entry only under CHTTP_REDIRECT_DROP_HOST,
     * which also clears has_host so that the block above writes the Host
     * line of the library. The block above that builds a Host header runs
     * only when !has_host. Otherwise a "host" entry reaches this loop only
     * when the caller gave one. In that case has_host is true and the block
     * above does not run. A skip with no condition here would drop a Host
     * header that the caller gave. The
     * caller can give one through chttp_request_set_header, or in a borrowed
     * map. The wire would then carry no built line, which is correct, and no
     * line from the caller either, which is wrong. That breaks the rule of
     * RFC 7230 SS5.4 that every HTTP/1.1 request MUST carry a Host header.
     * The unix-socket comment in this same file cites that rule. A fall
     * through here, like any other header, is what honours the purpose of
     * the has_host gate. */
    chttp_seen_names_t seen = {0};
    cmap_iterator *it = chashmap_begin_iter((chmap)req->headers, NULL);
    for (; it; it = it->_next_fn(it)) {
      const char *name = (const char *)it->key_pair->ptr;
      const char *val = (const char *)it->val_pair->ptr;
      /* chttp_request_set_header makes the same checks. See the doc comment
       * of that function. This check is the real backstop. req->headers is
       * an internal chmap handle. Any caller can build such a handle and
       * assign it directly, and that goes around chttp_request_set_header
       * completely. The borrowed map of chttp_run_query is one case. A
       * caller that writes chttp_request_t.headers itself is another. This
       * loop is the one place that writes every header onto the wire, from
       * either path. */
      if (strpbrk(name, "\r\n") || strpbrk(val, "\r\n") || !*name) {
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
      /* A method that carries no body on THIS hop never puts req->body onto
       * the wire. See the body-append check further down. This is true
       * whatever req->headers still holds. There are two cases. In the
       * first, the caller set one of these three headers on a request that
       * they always meant to carry no body. The second case matters more. A
       * 301, 302 or 303 redirect turns a POST, PUT or PATCH into a GET.
       * chttp_do_internal and _async_submit_hop rewrite cur_method and
       * cur_body for the new hop. But they send req->headers and
       * chain->req_headers again, completely unchanged. A Content-Length,
       * Content-Type or Expect that the caller set to describe the ORIGINAL
       * body therefore survives onto a hop that never sends one. A stale
       * Content-Length is not only wrong to look at. A receiving server that
       * trusts the declared length at the start blocks on a read of a body
       * that never arrives. It stays blocked until its own read timeout
       * fires. chttpserver.c in this codebase is such a server, through
       * chttp1_declared_content_length. */
      if (!body_carrying_method && (strcasecmp(name, "content-length") == 0 ||
                                    strcasecmp(name, "content-type") == 0 ||
                                    strcasecmp(name, "expect") == 0))
        continue;
      /* See CHTTP_REDIRECT_DROP_CREDENTIALS and CHTTP_REDIRECT_DROP_HOST.
       * A redirect chain can cross to a server that the caller never
       * named. The credentials and the Host header that the caller set for
       * the first server then stay off the wire. */
      if ((redirect_drops & CHTTP_REDIRECT_DROP_CREDENTIALS) &&
          _is_redirect_credential_header(name))
        continue;
      if ((redirect_drops & CHTTP_REDIRECT_DROP_HOST) &&
          strcasecmp(name, "host") == 0)
        continue;
      /* See the comment of chttp_seen_names_t. A borrowed map can hold two
       * keys for the same header name that differ only in case. Only the
       * first one that this loop finds reaches the wire. That one is the
       * newest insertion, by the iteration order of chmap. */
      if (_seen_names_contains(&seen, name)) continue;
      /* A Content-Length that the caller gives must match the body bytes
       * that the code below appends. See the body-append check further down.
       * Without this check, the framing that this hop declares to the server
       * disagrees with what goes onto the wire. This client also pools that
       * connection for reuse. The Transfer-Encoding rejection above exists
       * to stop the same hazard, where the declared framing disagrees with
       * the wire. See the doc comment of chttp_request_set_header. Here the
       * cause is a wrong length in place of a wrong transfer-coding. This
       * code checks only a body_carrying_method. For any other method, the
       * code a few lines above already drops this exact header from the
       * wire. There is then no framing for a wrong value to break. */
      if (body_carrying_method && strcasecmp(name, "content-length") == 0) {
        size_t expected = req->body.data ? req->body.len : (size_t)0;
        /* If this code accepts val, it writes val onto the wire byte for
         * byte. The grammar of val must therefore pass a strict check. That
         * check is as strict as the one that the request-side parser of
         * this codebase applies to an incoming Content-Length. That parser
         * is parse_uint64_decimal in chttp1_parser.c, and chttpserver.c
         * uses it. The grammar is a bare "1*DIGIT" with no sign and no
         * whitespace inside. strtoull alone is
         * not enough here. Unlike parse_uint64_decimal, it accepts a leading
         * '+' or '-' and leading whitespace before the digits. A value such
         * as "+42" or " 42" can equal the real body length as a number. It
         * would then pass, because strtoull("+42", &endp, 10) returns 42
         * with *endp == '\0'. This code would write it onto the wire exactly
         * as it is. The chttpserver of this library rejects that request as
         * "Invalid Content-Length", although chttp_request_set_header and
         * _serialize_request accepted it as valid. */
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
    /* This gate uses body_carrying_method alone. It does not also test
     * req->body.data or req->body.len. The Content-Length code a few lines
     * below does the same: it runs for any body_carrying_method with
     * !has_cl, whether or not body.data is set. A method that carries no
     * body never puts req->body onto the wire at all. GET, DELETE, HEAD and
     * OPTIONS are such methods. See the body-append check further down. A
     * content-type header for one of those is therefore inconsistent. A
     * caller can attach a body to a DELETE request. The
     * delete_body_not_transmitted test in this file does that. Such a caller
     * would get a "content-type: ..." header with no body bytes and no
     * Content-Length to match it. body_carrying_method alone rules that out.
     *
     * An empty body on a method that carries a body is a different and
     * legitimate case. There body.data is NULL because body.len is 0.
     * CHTTP_JSON_BODY("", 0) makes such a body. The code below still writes
     * "content-length: 0". A matching "content-type: ..." that describes
     * that empty representation is therefore correct HTTP. It must not carry
     * the extra gate on body.data or body.len that the other case has.
     *
     * This check guards against the same CRLF injection as the header loop
     * above. content_type has no setter of its own to check it at. It
     * arrives in a chttp_request_body_t, and chttp_request_new_mp copies it
     * exactly as it is. This is therefore its only checkpoint before the
     * wire. */
    if (strpbrk(req->body.content_type, "\r\n")) {
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
  /* This guards the size computations below against an overflow. _ob_append
   * has the same guard for the growable buffer that serializes a request.
   * bb->len is a running total across many _on_body callbacks. One socket
   * read bounds each callback to about 8KB. No single value from a caller
   * therefore has to be huge on its own for bb->len + len to come near
   * SIZE_MAX. Only the CUMULATIVE total across many calls does. This is
   * different from a single allocation sized by strlen() elsewhere in this
   * file. No real input reaches this guard either, because the real
   * allocator behind _ccol_mem_realloc below fails long before bb->buf can
   * grow near that size. The `if (!nb)` check further down already handles
   * that failure. But this project treats an unguarded overflow in a size
   * computation as a real bug. The size of the input that triggers it does
   * not matter. Without this guard, a wrapped bb->len + len makes the
   * max_size comparison below conclude that an oversized response is still
   * under the cap. It also makes the growth loop settle on an nc that is far
   * too small. Or nc itself wraps to 0 inside the loop, and the loop spins
   * forever. The memcpy below then becomes a real heap buffer overflow, in
   * place of a clean allocation failure that the library reports. */
  if (len > SIZE_MAX - bb->len || bb->len + len > SIZE_MAX - 1) {
    bb->oom = true;
    return 0;
  }
  /* This applies the cap as the body arrives. It covers every mode of body
   * framing: Content-Length, chunked and EOF-delimited. The check in
   * _on_headers_complete runs at the start against the declared
   * Content-Length. It catches only a Content-Length that says honestly that
   * the body is too large, before the library reads one body byte. A short
   * return here makes _on_body stop the parse with CHTTP1_USER. The
   * out-of-memory case just below does the same. Without it, bb->buf keeps
   * growing with no bound and nothing reports it. */
  if (bb->max_size > 0 && bb->len + len > bb->max_size) {
    bb->too_large = true;
    return 0;
  }
  if (bb->len + len + 1 > bb->cap) {
    size_t nc = bb->cap ? bb->cap * 2 : 4096;
    while (nc < bb->len + len + 1) {
      if (nc > SIZE_MAX / 2) {
        bb->oom = true;
        return 0;
      }
      nc *= 2;
    }
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
 * A white-box test helper. It exposes the overflow guard of _sink_buffered
 * directly. It sets up a chttp_bodybuf_t whose accumulated `len` already
 * sits at fake_len. bb.buf stays NULL, and the library never allocates a
 * real buffer of that size. The guard must reject the call before it touches
 * the malloc or realloc of bb->mp, and before it touches bb->buf itself. The
 * helper then feeds it a small, real probe chunk. This follows the same
 * pattern as _chttp_ob_append_overflow_guard_for_tests, which covers this
 * class of guard on the other growable buffer. This helper is not part of
 * the public API. A gate keeps this symbol out of a production build of
 * libccollections.so. Every other white-box helper in this file has the same
 * gate.
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
#endif /* RUNNING_UNIT_TESTS */

#ifdef RUNNING_UNIT_TESTS
/* The number of record headers that the code below read. A test bounds it
 * by the number of records, which pins that no operation rescans the
 * block. */
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

/* FNV-1a over the lower-case name, then a multiply whose high bits pick the
 * slot, so that every byte of the name reaches the bits that the table
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

/* This returns the slot of the lower-case name, or NULL when the name has
 * no records. */
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

/* This makes room for `need` more bytes of records and for one more name in
 * the index, allocating or growing through mp. It returns false when an
 * allocation fails, and then leaves every record and every slot as it
 * was. */
static bool _field_lines_reserve(ccol_memmgmt_procs_t *mp,
                                 chttp_field_lines_t **linesp, size_t need) {
  chttp_field_lines_t *lines = *linesp;
  size_t len = lines ? lines->len : 0;
  size_t cap = lines ? lines->cap : 0;
  if (cap - len < need) {
    /* The header caps of the parser bound every length here far below any
     * overflow: the whole block holds at most two copies of the header
     * section, plus a small header for each record. */
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
  /* The index stays at most half full, so that a probe ends soon. */
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

/* This appends one record at the end of the block and returns its offset.
 * The caller reserved the room. */
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

/* This records one more occurrence, `value`, of the lower-case name. When
 * the name has no records yet, first_value is its first occurrence, which
 * the header map holds, and it is recorded first. It returns false when an
 * allocation fails, and then leaves *linesp as it was. */
/* A repeated name is the uncommon case, so this stays out of line and
 * leaves _on_header its short path. */
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
    /* The reserve above can move the block, and it can rebuild the index.
     * Find the slot again. */
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

/* This replaces the header map value of every repeated name with the
 * combined value of RFC 9110 section 5.3: each occurrence in the order of
 * the wire, joined with ", ". Set-Cookie keeps its first occurrence, because
 * RFC 9110 section 5.3 and RFC 6265 section 3 forbid its combination. The
 * combination runs once, after the whole header block, so that each value
 * is built once whatever the number of repeats. It returns false when an
 * allocation fails. */
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

/* chttp1_parser gives a whole header line to this callback. It already
 * splits the name from the value and trims the optional whitespace. This
 * callback therefore keeps no accumulator state at all. It fires only for
 * the header block of the response. The trailer fields of a chunked body go
 * to settings->on_trailer, and _init_chttp1_settings deliberately leaves
 * that callback NULL. Nothing that this callback puts into ctx->headers or
 * ctx->field_lines can therefore come from after the body, so a trailer
 * never changes what a response header reads as.
 *
 * The first occurrence of a name goes into the header map, in one probe.
 * A later occurrence leaves the map alone and goes into ctx->field_lines,
 * together with the first occurrence when the name repeats for the first
 * time. _field_lines_combine then builds the combined map value once the
 * header block is complete. Until then the map holds the first occurrence
 * of every name, which is what the redirect logic reads for Location.
 *
 * name and value point into the internal line buffer of the parser. They are
 * valid only for this call. BOTH therefore need a local, NUL-terminated copy
 * before use. name needs one because this callback must lower-case it. value
 * needs one because the cmap_pair convention of this codebase for a string
 * value is "size = strlen + 1", and chmap_insert_elem copies exactly that
 * many bytes. A read of value_len + 1 raw bytes out of the line buffer of
 * the parser would read one byte past the value itself. Nothing guarantees
 * that this byte is '\0'. Both copies share one buffer. A line that fits the
 * fixed buffer of the parser, CHTTP1_MAX_LINE_LEN bytes, always fits the
 * stack buffer below, because the name, the colon and the value are all
 * parts of that line. A longer line, which the parser holds on the heap,
 * gets a heap copy too. */
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
    lower_name[i] = (char)tolower((unsigned char)name[i]);
  lower_name[name_len] = '\0';

  char *value_copy = buf + name_len + 1;
  memcpy(value_copy, value, value_len);
  value_copy[value_len] = '\0';

  int ret = 0;
  cmap_pair kp = {.ptr = lower_name, .size = name_len + 1};
  cmap_pair vp = {.ptr = value_copy, .size = value_len + 1};
  const cmap_pair *slot = NULL;
  ccol_retval_t rv =
      ccol_chmap_insert_or_get_elem(ctx->headers, &kp, &vp, &slot);
  if (rv != ccol_success &&
      (rv != ccol_key_already_present ||
       !_field_lines_add(ctx->mp, &ctx->field_lines, lower_name, name_len,
                         (const char *)slot->ptr, slot->size - 1, value_copy,
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

  /* A "101 Switching Protocols" answers an Upgrade request (RFC 9110
   * SS15.2.2), and this client never sends Upgrade. After a 101 the bytes
   * that follow belong to another protocol, so no reading of them as HTTP
   * is right: neither the discard that every other 1xx gets nor a final
   * response. The request fails here, as curl fails it, before the
   * interim-response handling of any tier sees the message. ctx->error and
   * ctx->too_large stay false, so every caller of the parser maps this stop
   * to ccol_http_transfer_aborted. */
  if (p->status_code == 101) return -1;

  /* This code always detects whether the response IS a redirect. The number
   * of hops that the chain already used does not matter here. The caller
   * knows the current hop count. It decides whether to follow the redirect
   * or to report ccol_http_too_many_redirects. For Tier 1 that caller is the
   * hop loop of chttp_do_internal. For Tier 2 and Tier 3 it is
   * _async_handle_redirect. This callback has no hop count of its own to
   * gate on.
   *
   * A redirect status is followed only when its Location is present and not
   * empty. A 3xx with no Location, or with an empty one, is the final
   * response of the request, which is what Go's net/http client does. An
   * empty field value names no target, so there is nothing to follow.
   *
   * The map still holds the FIRST occurrence of every name here, because
   * _field_lines_combine runs below. A response with more than one Location
   * therefore redirects to the first one, as curl and Go's net/http do, and
   * never to a combined "a, b" that names no resource at all. */
  bool is_redirect_status = ctx->status_code == 301 ||
                            ctx->status_code == 302 ||
                            ctx->status_code == 303 ||
                            ctx->status_code == 307 || ctx->status_code == 308;
  if (is_redirect_status) {
    cmap_pair kp = {.ptr = (void *)"location", .size = sizeof("location")};
    const cmap_pair *vp = NULL;
    if (chmap_get_elem_ref(ctx->headers, &kp, &vp) == ccol_success && vp &&
        ((const char *)vp->ptr)[0] != '\0') {
      ctx->location = ccol_strdup(ctx->mp, (const char *)vp->ptr);
      if (!ctx->location) {
        ctx->error = true;
        /* Any value that is not 0 and not 1 stops the parse with
         * CHTTP1_USER. */
        return -1;
      }
      ctx->will_redirect = true;
    }
  }

  if (ctx->field_lines && !_field_lines_combine(ctx)) {
    ctx->error = true;
    /* Any value that is not 0 and not 1 stops the parse with CHTTP1_USER. */
    return -1;
  }

  /* This rejects an oversized Content-Length that a server declares
   * honestly. It does so before the library reads one body byte off the
   * wire. It has meaning only for a buffered request that is NOT itself a
   * redirect. _sink_discard always throws away the body of a redirect,
   * whatever its declared length says. A caller that streams the body
   * manages its own memory through the return value of chttpcli_write_fn,
   * and it never uses a chttp_bodybuf_t. The check also has meaning only for
   * a message that really carries a body onto the wire.
   *
   * Several classes of message can legitimately declare a Content-Length
   * that has nothing to do with what the library reads. The first is a 1xx
   * informational response. RFC 7230 SS3.3.2 says that a server MUST NOT
   * send Content-Length on one at all, but a bad or malicious server can do
   * it anyway. The interim-response code of the caller always discards or
   * skips every such response, and it never delivers one as a final result.
   * That code is the is_skippable_1xx check of _chttp_read_message_loop, or
   * the no_body rule for 1xx that the parser applies a few lines below this
   * callback. The second class is a 204 or 304 response. RFC 7230 SS3.3
   * treats these exactly like 1xx for body framing. A 304 commonly carries
   * the Content-Length of the original resource, as RFC 7232 SS4.1 allows.
   * A conditional GET against a CDN produces such a response. chttp1_parser
   * already forces no_body for both, whatever length they declare. See the
   * same set of status codes in chttp1_parser.c. The third class is a HEAD
   * response. Its Content-Length describes what a GET would have returned,
   * as RFC 7231 SS4.3.2 says, and no body bytes follow it. See the
   * is_head_request check at the very end of this function.
   *
   * Without these exclusions, this cap fails the WHOLE request with
   * ccol_msg_too_large although the response that the library delivers is
   * well inside the configured limit. That response always has zero bytes
   * for 1xx, 204 and 304. Three ordinary cases would fail. The first is a
   * 103 Early Hints interim response, or any other 1xx. It declares an
   * oversized length ahead of a small final response inside the cap. The
   * second is a plain HEAD request against a large resource. The third is an
   * ordinary conditional GET that receives 304 Not Modified with the
   * oversized Content-Length of the original resource.
   *
   * A chunked or EOF-delimited body declares no Content-Length. It gives
   * this code no signal to check at the start. The per-append check in
   * _sink_buffered below still bounds it. */
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
      /* Any value that is not 0 and not 1 stops the parse with
       * CHTTP1_USER. */
      return -1;
    }
  }

  ctx->sink_fn = ctx->will_redirect ? _sink_discard : ctx->requested_sink_fn;
  ctx->sink_ctx = ctx->will_redirect ? NULL : ctx->requested_sink_ctx;

  /* The Content-Length of a HEAD response describes a body that the server
   * never sent. The response may carry no such header at all. This is the
   * one case that the parser cannot work out from the wire on its own. */
  return ctx->is_head_request ? 1 : 0;
}

static int _on_body(chttp1_parser_t *p, const char *at, size_t len) {
  chttp_parse_ctx_t *ctx = (chttp_parse_ctx_t *)p->data;
  size_t n = ctx->sink_fn ? ctx->sink_fn(at, len, ctx->sink_ctx) : len;
  if (n != len) {
    /* A short return from _sink_buffered has two causes. Its realloc failed,
     * and ctx->error reports that so the caller sees
     * ccol_not_enough_memory. Or the body went past the configured cap on
     * the response size, and ctx->too_large reports that so the caller sees
     * ccol_msg_too_large. Neither one is a stop by the caller that streams
     * the body. ctx->aborted reports that third case, and the caller sees
     * ccol_http_transfer_aborted. All three arrive as the same CHTTP1_USER
     * return from chttp1_parser_execute. This is therefore the only place
     * that can still tell them apart. */
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
  ccol_once_flag_t once;
} client_http1_settings_bundler = {0};

static void _init_chttp1_settings(void) {
  chttp1_settings_init(&client_http1_settings_bundler.settings);
  client_http1_settings_bundler.settings.on_header = _on_header;
  /* settings.on_trailer stays NULL, because chttp1_settings_init zeroed it.
   * This client offers no API for a trailer. A callback that stays unset is
   * what makes a trailer field unable to reach ctx->headers by
   * construction. See the comment of _on_header. */
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

/* This resets ctx to parse a SECOND message on the same connection. That
 * message is logically distinct. It is the real final response that follows
 * a "100 Continue" interim response, which the same ctx just parsed. This
 * matches the convention of chttp_do_internal, which uses fresh state for
 * each hop with a new chttp_parse_ctx_t. Here the same idea applies inside
 * one hop, which is what the two-message exchange of this one connection
 * needs. The headers of the interim response are rare but legal, and they
 * must never leak into the header map of the final response.
 *
 * This function leaves is_head_request unchanged. That flag is a property of
 * the request and not of any one parsed message. It also leaves
 * requested_sink_fn and requested_sink_ctx unchanged, because they hold the
 * real sink configuration of the caller. It clears sink_fn and sink_ctx,
 * because _on_headers_complete resolves those again for the message that it
 * parses. */
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
  ctx->too_large = false; /* No current path reaches this reset with the
                           * flag set. Every path that sets it already fails
                           * the read with CHTTP1_USER before this reset
                           * runs. The reset stays anyway. A stale "too
                           * large" verdict from a discarded interim message
                           * can therefore never carry forward into the next
                           * message that this function prepares ctx for. */
  ctx->status_code = 0;
  ctx->sink_fn = NULL;
  ctx->sink_ctx = NULL;
  return ctx->headers ? ccol_success : ccol_not_enough_memory;
}

/*
 * This reads and parses exactly one HTTP/1.1 message from `conn`. It starts
 * with the bytes that `carry_in` already holds, if there are any. It feeds
 * those to the parser before it touches the socket. After carry_in is empty,
 * it falls back to ordinary socket reads that the deadline bounds. On
 * ccol_success, *keep_alive_out carries the keep-alive answer of
 * chttp1_parser itself. That answer does not yet account for trailing
 * garbage. See _chttp_early_resp_finish, which is the only caller that
 * should treat leftover bytes as garbage.
 *
 * This function does not treat bytes past the message boundary as trailing
 * garbage. It reports them through leftover_out and leftover_len_out
 * instead. It allocates that buffer with pctx->mp. Both values are NULL and
 * 0 when there is none. The "Expect: 100-continue" code of
 * chttp_do_internal needs this. It carries the real bytes of the final
 * response of a fast server forward into a second parse. Those bytes can
 * arrive in the same read as the "100 Continue" interim status line. Every
 * other caller
 * has no second message to carry them into. Such a caller must treat a
 * leftover that is not empty exactly as the trailing garbage that it is. See
 * _chttp_early_resp_finish.
 *
 * This function sets *any_bytes_read_out to true the moment that the first
 * byte of THIS message reaches the parser. It does not matter where that
 * byte comes from. It can come from a live read off the wire in this call.
 * It can also come from carry_in, which holds bytes that the caller read off
 * the wire in an earlier call. The "Expect: 100-continue" code of
 * chttp_do_internal works in that way. It passes the leftover bytes of a
 * fast server forward into the read of the real final response.
 *
 * This function deliberately treats both sources in the same way. It feeds a
 * carry_in that is not empty straight into chttp1_parser_execute() below.
 * That call can run on_header, on_body and on_headers_complete against pctx
 * and against the sink of the caller, exactly as a live read does. A failure
 * partway through this call can therefore leave real state behind that the
 * caller can see. This is true even when the library reads no byte from the
 * fd during this call.
 *
 * A caller uses *any_bytes_read_out to classify a failure. If it is false,
 * nothing was parsed and nothing reached the caller, so a quiet retry
 * against a fresh connection is safe. If it is true, part of a response is
 * already in flight, and a user callback may already hold some of it. The
 * caller must then report the failure. carry_in is deliberately NOT exempt
 * from this accounting. An exemption would let the retry-once safety net of
 * chttp_do_internal for a reused connection send a request again on a fresh
 * connection. It would reuse a chttp_parse_ctx_t and a body buffer that a
 * dead carry_in parse already filled in part. The headers and the body
 * prefix of a discarded response would then mix into the response that the
 * library delivers, with no report.
 *
 * first_byte is NULL, or a deadline that bounds only the wait for the first
 * byte of this message. The "Expect: 100-continue" wait of
 * _chttp_send_and_read passes its window here. Until a byte of the message
 * reaches the parser, that deadline bounds the read, and its expiry returns
 * ccol_timed_out with the parser untouched. From the first byte on, only
 * `overall` bounds the read, so a message that the server started inside the
 * window is always read to its end, with the parser that holds its start. A
 * TLS record that carries no plaintext, such as a session ticket, starts no
 * message.
 */
static ccol_retval_t _chttp_read_message_with(
    chttp1_parser_t *parser, chttp_conn_t *conn, chttp_parse_ctx_t *pctx,
    chttp_deadline_t *overall, chttp_deadline_t *first_byte,
    const char *carry_in, size_t carry_in_len, bool *keep_alive_out,
    bool *any_bytes_read_out, char **leftover_out, size_t *leftover_len_out) {
  *leftover_out = NULL;
  *leftover_len_out = 0;
  /* This is true once a byte of this message reached the parser. From then
   * on only `overall` bounds the read. See the doc comment above for
   * first_byte. */
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
    /* The message is not complete yet. Fall through and read more. */
  }

  char buf[8192];
  /* This matches the want_events of _chttp_send_all, in the opposite home
   * direction. An EWOULDBLOCK from ctls_conn_read() can equally mean "wait
   * for WRITABLE". OpenSSL may need to flush a renegotiation or a KeyUpdate
   * response before it can decrypt another record. A poll on POLLIN there
   * with no condition waits for a readability that never arrives. The
   * request then stalls for the whole remaining deadline. The default
   * request_timeout_us is 0, so the stall lasts forever. */
  short want_events = POLLIN;
  /* This is true after a read that returned EWOULDBLOCK, and false after a
   * read that made progress. See the wait below. */
  bool read_blocked = false;
  for (;;) {
    int wait_ms;
    chttp_deadline_t *dl = (started || !first_byte) ? overall : first_byte;
    /* See _read_deadline_passed for the rule that this check applies. */
    if (_read_deadline_passed(dl, conn->fd) ||
        !_deadline_remaining_ms(dl, &wait_ms))
      return ccol_timed_out;
    /* A TLS read with a buffer smaller than the record leaves the rest of
     * the record inside the TLS layer, where poll(2) cannot see it. A wait
     * for readability then waits for bytes that already arrived, until the
     * deadline, or forever under the default request_timeout_us of 0. The
     * read therefore goes first while ctls_conn_has_pending_input reports
     * input. After a read that blocks, the wait is correct again, because
     * that input is then only the start of a record that is not complete
     * yet. */
    bool buffered = conn->tls && want_events == POLLIN && !read_blocked &&
                    ctls_conn_has_pending_input(conn->tls);
    if (!buffered) {
      ccol_retval_t prv = _conn_wait(conn->fd, want_events, wait_ms,
                                     ccol_http_transfer_aborted);
      if (prv != ccol_success) return prv;
    }

    ssize_t n = _conn_read(conn, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EWOULDBLOCK) {
        read_blocked = true;
        if (conn->tls)
          want_events = _conn_tls_wants_write(conn->tls) ? POLLOUT : POLLIN;
        continue;
      }
      return ccol_http_transfer_aborted;
    }
    read_blocked = false;
    /* The read made progress, or it reached EOF. The next attempt starts
     * from the home direction of this call again. _chttp_send_all resets
     * its own direction in the same way. */
    want_events = POLLIN;
    if (n > 0) {
      *any_bytes_read_out = true;
      started = true;
    }
    if (n == 0) {
      /* A valid EOF-delimited body ends with CHTTP1_PAUSED here, and not
       * only with CHTTP1_OK. Both are expected. Such a body comes from an
       * HTTP/1.0-style response, or from an explicit "Connection: close"
       * with no Content-Length and no chunked framing. See the doc comment
       * of chttp1_parser_finish. Only CHTTP1_ERROR falls through to the
       * aborted case, and that value means a truncated message. */
      chttp1_errno_t fe = chttp1_parser_finish(parser);
      if ((fe != CHTTP1_OK && fe != CHTTP1_PAUSED) || !pctx->message_complete)
        return ccol_http_transfer_aborted;
      /* The peer closed, so there is nothing left to reuse. */
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
    /* The message is not complete yet. More data is needed. */
  }
}

static ccol_retval_t _chttp_read_message(
    chttp_conn_t *conn, chttp_parse_ctx_t *pctx, chttp_deadline_t *overall,
    chttp_deadline_t *first_byte, const char *carry_in, size_t carry_in_len,
    bool *keep_alive_out, bool *any_bytes_read_out, char **leftover_out,
    size_t *leftover_len_out) {
  ccol_call_once(client_http1_settings_bundler.once, _init_chttp1_settings);

  /* A response header line can be up to CHTTP1_MAX_SPILL_LINE_LEN bytes
   * long. The parser allocates only for a line that outgrows its fixed
   * buffer, and the release below frees that on every return. */
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
 * This works like _chttp_read_message, with one difference. It discards and
 * reads past any interim informational 1xx response whose status is not
 * stop_at_status. Pass 0 to treat no status as the expected stop code, which
 * skips every 1xx. It continues until a message that is worth a return to
 * the caller arrives. That message is stop_at_status itself, any final
 * response with a status of 200 or more, or a hard failure or a timeout.
 *
 * The skip of interim responses here is what makes a 1xx that a server sends
 * on its own safe. The alternative gives each caller exactly one message and
 * treats it as final. A server may send an interim status other than the one
 * that the caller watches for. "103 Early Hints" (RFC 8297) is the main
 * example, and a server may send it ahead of an ORDINARY response, not only
 * ahead of "100 Continue". A read that treats such a message as the final
 * response gives the caller an empty result with the wrong status. The real
 * response of the server then sits unread on the wire. That corrupts
 * whatever unrelated later request reuses this same connection from the
 * keep-alive pool.
 *
 * _parse_ctx_reset_for_continue clears the pctx state of every discarded
 * interim message before the next read. That state holds the headers, the
 * Location and the other fields. "100 Continue" itself already gets the same
 * treatment. The headers of a discarded message can therefore never leak
 * into the message that the caller receives.
 *
 * A fast server can already start to write its next message in the same
 * read. The leftover bytes past the boundary of a DISCARDED message go into
 * the very next read, as its carry_in. The "100 Continue" code of
 * chttp_do_internal uses the same mechanism. This function reports only the
 * leftover bytes past the message that it RETURNS. It reports them through
 * leftover_out and leftover_len_out, which matches the contract of
 * _chttp_read_message. The caller decides what to do with them.
 * _chttp_early_resp_finish always treats them as trailing garbage. The
 * "100 Continue" wait of _chttp_send_and_read instead carries them forward
 * into the real final read.
 *
 * A discarded interim message can receive bytes off the wire, and that sets
 * *any_bytes_read_out to true. This loop then deliberately never sets that
 * flag back to false for a later message. A complete interim response proves
 * that the server received this request and started to answer it. Such a
 * response is not merely partial or abandoned. A recovery at the caller
 * level that retries the whole hop against a fresh connection sends the
 * request again from the start. It is therefore no longer safe for that
 * recovery to act as if nothing happened yet. Every other use of
 * any_bytes_read_out in this file rests on the same reason.
 *
 * discarded_before is the number of interim responses of the same sequence
 * that the caller already discarded before this call, while the request was
 * still going out; the cap of CHTTP_MAX_INTERIM_RESPONSES counts them too.
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

  /* This loop compares n_discarded with the cap AFTER it reads a message
   * and finds that the message is an interim one that it can skip. It never
   * compares before a read attempt. See the comment of
   * CHTTP_MAX_INTERIM_RESPONSES for the documented contract: the number of
   * responses that the loop discards before it stops.
   *
   * If the loop compared the count at the start of each read attempt, the
   * comparison would apply to an interim message and to a final message.
   * After 64 discarded interim responses, the loop would then not read the
   * next message, and it would refuse a real final response in position 65.
   * That stops one message too early. It breaks the documented contract of
   * this function, which allows 64 consecutive discarded interim responses,
   * and which chttpclient_do(3) also publishes. It also does not agree with
   * the same loop in the async engine, _async_on_readable_impl, which
   * applies the cap to the interim messages only and never to the final
   * response.
   *
   * The comparison here, after the loop identifies the message, makes the
   * two tiers agree exactly. The library can always deliver a final
   * response, at any position. Only the count of discarded interim
   * responses has a limit. */
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
 * This builds cli->tls_ctx from cli->tls, or rebuilds it. The library calls
 * it at the construction of a client, with the default configuration. It
 * also calls it from chttpclient_set_tls.
 *
 * ctls_ctx_cert_add and ctls_ctx_trust report a failure with an ordinary
 * ccol_retval_t. They never end the process. A file that is missing or that
 * the library cannot read is always a recoverable error here. It is never a
 * process abort. chttpclient_set_tls must accept a path whose syntax is
 * valid but that does not exist yet, and it must not crash. A caller can
 * configure TLS long before it makes an HTTPS request, or it can never make
 * one. This function therefore checks the configured paths with access()
 * BEFORE it calls into ctls. If a path does not pass, this function builds
 * no context. The failure then waits until connection time, in
 * chttp_do_internal, where it appears as an ordinary ccol_retval_t.
 *
 * This check at the start is what gives this function its contract: it
 * always returns ccol_success, and a failure always waits. The graceful
 * failure of ctls_ctx_cert_add alone would not give that contract.
 */
static ccol_retval_t _rebuild_tls_ctx_locked(struct chttpclient *cli) {
  /* This raise happens before anything else, and it holds on every path out
   * of this function. The deferred-failure returns below are such paths.
   * What matters to a pooled connection is that a new configuration replaced
   * the one that its handshake ran under. Whether the replacement built
   * successfully does not matter. See the field comment of
   * cli->tls_generation. A rise to UINT64_MAX needs one reconfiguration for
   * each nanosecond, for more than five centuries. No handling of a wrap is
   * therefore needed. At worst, a wrap makes one pooled connection eligible
   * for one extra reuse. */
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
    /* No explicit bundle, and the caller did not opt out, so the system
     * trust store is what the chain is verified against. This is the branch
     * that a zero-initialised chttp_tls_config_t takes; see the field comment
     * of insecure_skip_verify. The chain check and the hostname check are one
     * switch, because a hostname matched against a certificate whose chain
     * nobody validated gives no security at all. */
    if (ctls_ctx_trust_system(ctx) != ccol_success) {
      /* No check at the start can catch this failure. It means that ctls
       * could not load the default CA store of the platform. This is
       * different from a CA-bundle file that is missing or unreadable, which
       * the access() call above catches before anything touches ctls.
       *
       * The complete discard of ctx here matters. The alternative falls
       * through and installs ctx. ctx already committed its verify_peer flag
       * (the SSL_VERIFY_PEER that ctls_ctx_trust_system sets)
       * to SSL_VERIFY_PEER before this rebuild failed. But the rebuild that
       * would have loaded the trust anchors never committed. If this code
       * kept ctx, the PREVIOUS ctx_default from the first build of
       * ctls_ctx_new_mp would stay installed, with no verification of a
       * certificate at all. That ctx_default predates verify_peer. Every
       * other failure in this function discards ctx for the same reason. */
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
 * This is one static ccol_event_loop reactor for the whole process. Every
 * chttpcli instance in the process shares it, and that includes the default
 * client that the library creates on demand. There are two separate,
 * independent reactors. The other one belongs to chttpserver. See the same
 * lifecycle wrapper in chttpserver.c.
 *
 * chttpclient shares no process-wide singleton with chttpserver. This
 * lifecycle wrapper therefore needs no ordering across modules at all. It
 * needs only a refcount across the callers of Tier 2 and Tier 3. It has the
 * same acquire, release and reaper-thread shape as the g_reactor of
 * chttpserver.c. chttpclient adds its own resources on top of it, and tears
 * them down in step with it. cli_engine_bundler.dns_pool and the deadline
 * sweep are those resources.
 */
static struct {
  ccol_event_loop reactor;
  size_t reactor_refs;
  ccol_mutex_t mutex;
  ccol_cond_var_t stopped_cv;
  ccol_once_flag_t once;
  bool stopping;
  ccol_thread_id_t reaper_thread;
  bool reaper_joinable;
  ccol_memmgmt_procs_t mprocs_storage;
  ccol_memmgmt_procs_t *mprocs;

  /* A value of 0 means that the library detects the count with
   * sysconf(_SC_NPROCESSORS_ONLN). That is the default of this module. A
   * positive value pins the reactor to exactly that many OS threads. See the
   * doc comment of chttpcli_set_engine_num_reactor_threads. The library
   * bakes this value into the reactor when it builds it. The same rule as
   * for mprocs above applies: set it before the first start, or after a full
   * stop. */
  size_t num_reactor_threads;
  /* The value that the library passed to ccol_event_loop_create_with_mprocs
   * the last time that it built the reactor. That value comes from the
   * detection or from an explicit setting. It is for test instrumentation
   * only. See _chttpclient_engine_num_reactor_threads_for_tests below. */
  size_t last_resolved_num_reactor_threads;

  /* The logger for diagnostics of the reactor threads of chttpclient. TLS
   * handshake failures and connect errors are such events. While the engine
   * runs this is user_logger when a caller installed one with
   * chttpcli_set_engine_logger. Otherwise _client_engine_acquire opens a
   * fallback logger at the CLOG_FATAL level, on fd 2, each time that the
   * engine starts, and the reaper closes that fallback when the engine
   * stops. This field is therefore never NULL while the engine runs.
   *
   * This is not only an internal detail. The teardown ccol_log_info call of
   * _client_engine_reaper_fn needs SOME live logger to call through. So does
   * the call of the deadline sweep. chttp1_parser has no logger of its own.
   * _clog_write has no guard against a NULL handle, so a call through a NULL
   * logger crashes. It does not skip the call.
   *
   * Only one rare path reaps the engine with this field left NULL. On that
   * path the allocation of the logger itself fails with an out-of-memory
   * error. See the comment of that path in _client_engine_acquire.
   *
   * cli_engine_bundler.mutex guards this field only against a torn read or
   * write that races a concurrent chttpcli_set_engine_logger() call. clog
   * itself is already thread-safe for concurrent log calls through one
   * handle. */
  clog logger;

  /* The logger that chttpcli_set_engine_logger derived, or CLOG_INVALID.
   * The engine owns it for the rest of the process, across every stop and
   * restart of the engine, until a later chttpcli_set_engine_logger call
   * replaces it. No engine stop closes it: only that replacement in
   * chttpcli_set_engine_logger and the destructor of this module do. A
   * reaper that closed it would leave every later engine start on the
   * fallback logger, and would silently drop the logger that the caller
   * installed once. cli_engine_bundler.mutex guards it. */
  clog user_logger;

  /* This moves the DNS resolve and connect step off the caller threads. That
   * step can block. This pool lives and dies with the reactor itself. The
   * library creates and destroys them together, under
   * cli_engine_bundler.mutex. It is not a separate singleton that the
   * library builds on demand, because nothing needs it after the reactor
   * goes down.
   */
  ctpool dns_pool;

  /* The number of engine references that async chains hold. It is a
   * subset of reactor_refs. The idle pool holds the rest. A chain whose
   * future is already fulfilled still holds its reference until its last
   * connection is torn down or pooled, which the reactor does at once.
   * _client_at_exit waits for this count to reach zero, and exit_waiting
   * tells _client_engine_acquire_chain and _client_engine_release_chain to
   * wake it. */
  size_t chains_live;
  bool exit_waiting;

  /* This guards the one registration of _client_at_exit. See the comment
   * of that function. */
  ccol_once_flag_t exit_hook_once;
#if CCOL_FORK_SAFETY_REQUIRED
  /* This guards the one registration of the fork handlers. See
   * _client_atfork_install. */
  ccol_once_flag_t atfork_once;
#endif
} cli_engine_bundler = {0};

/* The process that last built the engine, or that registered the exit hook
 * first. _client_at_exit acts only in this process. A child of fork()
 * inherits the state of the engine but none of its threads, and the reaper
 * handle that it inherits names a thread of the parent. The field is atomic
 * because the exit hook reads it before it takes any lock: in such a child
 * the mutex of the engine can be inherited in a locked state. */
static _Atomic pid_t g_client_engine_pid = 0;

/* The number of async requests in the process, whatever the client, whose
 * future is not fulfilled yet. _client_engine_acquire_chain adds one, under
 * the mutex of the engine, and the first fulfilment of the future takes it
 * away again, before the future becomes ready. The only code of the
 * application that runs on a thread of the engine is the write_fn of a
 * streaming request, and it runs strictly before the fulfilment of its own
 * future. A zero here therefore proves to _client_at_exit that the thread
 * which calls exit() runs no such callback, and that no request is still in
 * flight. See that function. */
static _Atomic size_t g_async_requests_pending = 0;

static ccol_retval_t _client_deadline_sweep_start(void);
static void _client_deadline_sweep_stop_and_join(void);
static void _client_exit_hook_install(void);
static void _default_client_destroy_if_idle(void);

#if CCOL_FORK_SAFETY_REQUIRED
static void _chttpcli_atfork_prepare(void);
static void _chttpcli_atfork_parent(void);
static void _chttpcli_atfork_child(void);

/* cthreadcomm.h, cthreadpool.h and clogger.h do not declare these, because
 * they are not public. Each forces the ccol_at_fork() registration of its
 * module to happen now, ahead of the registration of this module. See the
 * call site in _client_engine_globals_init. */
void _cthreadcomm_ensure_atfork_registered_before_caller(void);
void _ctpool_ensure_atfork_registered_before_caller(void);
void _clog_ensure_atfork_registered_before_caller(void);
#endif

#if CCOL_FORK_SAFETY_REQUIRED
/* This registers the fork handlers of this module. The first acquire of the
 * engine runs it once, through cli_engine_bundler.atfork_once, so that a
 * process that never uses Tier 2 or Tier 3 registers nothing and builds no
 * state of the modules below. */
static void _client_atfork_install(void) {
  /* The prepare handlers of pthread_atfork run in the reverse of the order
   * of registration. The registrations of ccol_event_loop, ctpool and clog
   * therefore go first, so that the prepare handler of this module always
   * runs first and takes its locks before theirs. That is the order of every
   * ordinary call here: cli_engine_bundler.mutex is held across the creation
   * of the reactor, of the DNS pool and of the fallback logger, and across
   * log calls, and a deadline stripe is held across a modify of a
   * registration of the reactor. */
  _cthreadcomm_ensure_atfork_registered_before_caller();
  _ctpool_ensure_atfork_registered_before_caller();
  _clog_ensure_atfork_registered_before_caller();
  ccol_at_fork(_chttpcli_atfork_prepare, _chttpcli_atfork_parent,
               _chttpcli_atfork_child);
}
#endif

static void _client_engine_globals_init(void) {
  if (ccol_mutex_init(cli_engine_bundler.mutex) != 0)
    ccol_fatal_err("chttpclient engine: failed to initialize mutex");
  if (ccol_cond_var_init(cli_engine_bundler.stopped_cv) != 0)
    ccol_fatal_err(
        "chttpclient engine: failed to initialize condition variable");

  /* The engine leaves the disposition of SIGPIPE to the application. No
   * write of this module can raise it: every plain write of all three tiers
   * is a send() with MSG_NOSIGNAL, and every TLS write, the close_notify of
   * a teardown included, goes through the socket BIO of ctls, which also
   * sends with MSG_NOSIGNAL. */
}

/*
 * This logs through cli_engine_bundler.logger. It puts the read of that
 * pointer and the ccol_log_info call into ONE critical section.
 *
 * A copy of the pointer taken under the lock, and then used with no lock
 * held, is a use-after-free. chttpcli_set_engine_logger() swaps in a new
 * logger under cli_engine_bundler.mutex. It then unlocks that mutex, and
 * only after that does it call clog_close() on the OLD logger.
 * clog_close() always frees the handle itself. It does this whatever the
 * separate refcount of the shared backing store says. See clogger.c. A
 * caller that holds a copy of the old pointer from before the swap can
 * therefore call ccol_log_info() on memory that is already free.
 *
 * This is a macro and not a function. The macro form keeps the
 * __FILE__, __LINE__ and __func__ capture of ccol_log_info at the real
 * call site, and not at this helper. */
#define _CLIENT_ENGINE_LOG_INFO(fmt, ...)                                 \
  do {                                                                    \
    ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init); \
    ccol_mutex_lock(cli_engine_bundler.mutex);                            \
    if (cli_engine_bundler.logger)                                        \
      ccol_log_info(cli_engine_bundler.logger, fmt, ##__VA_ARGS__);       \
    ccol_mutex_unlock(cli_engine_bundler.mutex);                          \
  } while (0)

#ifdef RUNNING_UNIT_TESTS
/* The number of reaper threads that _client_engine_spawn_reaper created, and
 * the number that the library joined. Once the engine is quiescent the two
 * are equal. A reaper handle that something overwrote before anybody joined
 * it shows as a join that never happens. */
static _Atomic unsigned g_reaper_created_for_tests = 0;
static _Atomic unsigned g_reaper_joined_for_tests = 0;
#endif

static void _client_engine_join_reaper_if_needed_locked(void) {
  if (cli_engine_bundler.reaper_joinable) {
    ccol_thread_join(cli_engine_bundler.reaper_thread);
    cli_engine_bundler.reaper_joinable = false;
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add(&g_reaper_joined_for_tests, 1u);
#endif
  }
}

/* This is true while the engine runs with no reference because
 * _client_engine_spawn_reaper found no thread for its teardown. The deadline
 * sweep reads it on every tick and hands the teardown to a new reaper as
 * soon as a thread is available. It is atomic because the sweep reads it
 * before it takes cli_engine_bundler.mutex, so that a tick of an engine that
 * is not stranded takes no lock of the engine. */
static _Atomic bool g_engine_reap_abandoned = false;

/*
 * A child of fork() inherits the state of the engine and none of its
 * threads. Three fields name a thread of the parent or wait for one: a
 * reaper handle that nobody joined yet, a `stopping` that only the reaper of
 * the parent clears, and a reactor that has no reference and waits for a
 * reap. The child must never join or wait on any of them. A reaper that
 * finished is not joined until the next acquire, and glibc hands the
 * descriptor of a thread that the child did not inherit to the next thread
 * that the child starts, so a join of the inherited handle joins an
 * unrelated thread of the child and can block for good.
 *
 * This runs under cli_engine_bundler.mutex, before any wait or join on that
 * state. When the engine belongs to another process, it forgets it: the
 * child builds its own engine on its next acquire. The parent's reactor and
 * DNS pool stay allocated in the child and are never used there; their
 * teardown would join threads that do not exist in this process.
 *
 * The logger of that engine run goes with it. A fallback logger belongs to
 * the run, and only the reaper of the parent would close it, so the child
 * closes its own copy here; a logger that the application installed stays
 * in user_logger for the engine that the child builds. Without this, the
 * next engine of the child keeps logging through the handle of the run of
 * the parent, and nothing ever closes that handle in the child.
 *
 * getpid() is a system call, so this asks it only in the states that can
 * name a thread of another process. A running engine that holds references
 * is not one of them: a child can reach it only through a handle that it
 * inherited, which fork(2) does not support. The fast path of an acquire
 * therefore reads only fields that it reads anyway, and makes no system
 * call. */
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
  atomic_store(&g_engine_reap_abandoned, false);
}

/*
 * This runs on a new thread. It never runs inline on the calling thread
 * that dropped the last reference. That thread is often a reactor callback
 * thread itself. _async_on_error, which tears down the last live ctx, is
 * one example. Such a thread must never block on a join of the deadline
 * sweep. It must never block on a drain of cli_engine_bundler.dns_pool
 * either.
 *
 * This function tears down the deadline sweep and
 * cli_engine_bundler.dns_pool. It then destroys the reactor. Last, it
 * clears cli_engine_bundler.stopping, so that an acquirer that waits can
 * go on.
 */
#ifdef RUNNING_UNIT_TESTS
/* While g_reaper_final_hold_ms_for_tests is not zero, the next reaper holds
 * cli_engine_bundler.mutex in its final critical section for that many
 * milliseconds, and sets g_reaper_final_held_for_tests while it does. The
 * hold is one-shot, and bounded, so a test that forks while it lasts never
 * hangs the reaper. */
static _Atomic int g_reaper_final_hold_ms_for_tests = 0;
static _Atomic bool g_reaper_final_held_for_tests = false;

static void _reaper_final_hold_for_tests(void) {
  int ms = atomic_exchange(&g_reaper_final_hold_ms_for_tests, 0);
  if (ms <= 0) return;
  atomic_store(&g_reaper_final_held_for_tests, true);
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  atomic_store(&g_reaper_final_held_for_tests, false);
}
#endif

static void *_client_engine_reaper_fn(void *arg) {
  (void)arg;
  ccol_event_loop loop_to_destroy;
  ctpool dns_pool_to_destroy;
  ccol_mutex_lock(cli_engine_bundler.mutex);
  loop_to_destroy = cli_engine_bundler.reactor;
  dns_pool_to_destroy = cli_engine_bundler.dns_pool;
  ccol_mutex_unlock(cli_engine_bundler.mutex);

  /* Stop the deadline sweep first. Do this before the teardown of the
   * reactor and of the DNS pool, because a sweep can still reference them.
   * A sweep tick closes a fd and removes a registration directly. See
   * _client_deadline_sweep_once. */
  _client_deadline_sweep_stop_and_join();
  if (loop_to_destroy) ccol_event_loop_destroy(loop_to_destroy);
  /* The ctpool_destroy call happens here, outside the lock. This matches
   * the ccol_event_loop_destroy call just above it. ctpool_destroy is an
   * implicit drain shutdown. It joins every worker thread of dns_pool, and
   * that can be slow when one worker is inside getaddrinfo() or connect().
   *
   * A call that holds cli_engine_bundler.mutex across the drain blocks
   * other calls for the whole drain. chttpcli_set_engine_logger,
   * chttpcli_set_engine_mem_mgmt_procs and
   * chttpcli_set_engine_num_reactor_threads are those calls. Each one is
   * cheap, and each one always takes this same mutex. None of them waits
   * on a condition variable that would give the mutex back.
   *
   * No ordering rule asks for the lock here. The library always clears
   * reactor and dns_pool together, inside the single critical section
   * below. The position of the destroy call does not change that. */
  if (dns_pool_to_destroy) ctpool_destroy(dns_pool_to_destroy);

  ccol_mutex_lock(cli_engine_bundler.mutex);
#ifdef RUNNING_UNIT_TESTS
  _reaper_final_hold_for_tests();
#endif
  cli_engine_bundler.dns_pool = CTPOOL_INVALID;
  cli_engine_bundler.reactor = CCOL_EVENT_LOOP_INVALID;
  cli_engine_bundler.stopping = false;
  /* logger can still be NULL here, and that is a legitimate state. The
   * fallback-logger allocation of _client_engine_acquire can fail. That
   * function then reaps the engine through this same reaper path. See the
   * comment of that function. No logger is installed in that case.
   * _clog_write has no NULL-handle guard of its own. A log call with no
   * guard here would therefore crash, and not skip the diagnostics. Every
   * other ccol_log_info call site in this file already carries the same
   * guard, as `if (el)` or as `if (logger)`. */
  if (cli_engine_bundler.logger)
    ccol_log_info(cli_engine_bundler.logger,
                  "The http client reactor engine has been destroyed");
  /* Only the fallback logger belongs to this engine run. The logger of the
   * caller stays in user_logger for the next start. */
  clog old_logger = cli_engine_bundler.logger != cli_engine_bundler.user_logger
                        ? cli_engine_bundler.logger
                        : CLOG_INVALID;
  cli_engine_bundler.logger = CLOG_INVALID;
  ccol_cond_var_broadcast(cli_engine_bundler.stopped_cv);
  ccol_mutex_unlock(cli_engine_bundler.mutex);
  if (old_logger) clog_close(old_logger);
  return NULL;
}

/* These two values control the retries of _client_engine_spawn_reaper. The
 * first is the number of extra attempts that it makes. The second is the
 * pause between two attempts. After them, it gives up on a thread.
 *
 * pthread_create can report EAGAIN under a ceiling on processes or threads.
 * RLIMIT_NPROC and a cgroup pids.max are two such ceilings. This is an
 * ordinary short-lived condition. It clears as soon as any other thread of
 * the process exits. A few short retries therefore turn most cases into an
 * ordinary reap. The total pause is small on purpose. This code can run on
 * a reactor dispatch thread, and the fallback below that abandons the reap
 * is correct on its own. */
#define CHTTP_REAPER_SPAWN_RETRIES 5
#define CHTTP_REAPER_SPAWN_RETRY_NS (1000L * 1000L) /* 1 ms */

#ifdef RUNNING_UNIT_TESTS
/* While this flag is true, _client_engine_spawn_reaper skips every
 * ccol_thread_create attempt. It reports each one as a failure, and it
 * creates no thread. A test can therefore drive the give-up path below in a
 * deterministic way. The real call almost never fails on an unloaded
 * machine. A real EAGAIN needs a tight `ulimit -u` or an equivalent cgroup
 * ceiling. The test resets this flag itself.
 * _chttpclient_engine_reaper_abandoned_count_for_tests reports how many
 * times the library took that path. */
static _Atomic bool g_force_reaper_spawn_fail_for_tests = false;
static _Atomic unsigned g_reaper_abandoned_count_for_tests = 0;

/* This is -1 until a shutdown site for an abandoned ctx runs at all. After
 * that it is 1 when the shutdown ran while the idle_lock of the ctx was
 * still held. It is 0 when the shutdown ran after the unlock.
 *
 * Only 1 is safe. After the unlock of idle_lock, a reactor dispatch that
 * parks in _async_dispatch_kind runs the real teardown. That teardown
 * close()s ctx->fd. A shutdown after that point names an fd number that
 * the kernel can already give to an unrelated socket of this process.
 *
 * The answer comes from a marker that the lock itself maintains on every
 * lock and unlock. That marker is
 * chttp_async_ctx_t.idle_lock_held_for_tests. The shutdown site never
 * asserts about its own position. */
static _Atomic int g_abandon_shutdown_under_lock_for_tests = -1;

/* While g_reaper_spawn_hold_for_tests is true, the next reaper that
 * _client_engine_spawn_reaper creates is held between its creation and the
 * publication of its handle: the spawning thread sets
 * g_reaper_spawn_held_for_tests and waits, for at most ten seconds, until a
 * test sets g_reaper_spawn_gate_for_tests. The hold is one-shot. A test
 * therefore puts a second spawn inside the window of the first one. */
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
 * This hands the engine teardown to a dedicated thread. The teardown must
 * never run on the thread that dropped the last reference. That thread is
 * often a reactor dispatch thread or a dns_pool worker.
 * _client_engine_reaper_fn joins the deadline sweep. It then destroys the
 * reactor and the dns_pool. A run on that thread makes the join of the
 * event loop a self-join. cthreadpool also detects a worker that calls
 * ctpool_destroy on its own pool, and it stops the process with
 * ccol_fatal_err(). An inline run because no thread is free therefore turns
 * a short-lived resource shortage into an abort.
 *
 * When no thread is free even after the retries above, the library leaves
 * the engine running. It clears `stopping`, so that the reference count can
 * grow again, and it sets g_engine_reap_abandoned. The deadline sweep of the
 * engine then retries the teardown on each of its ticks, through
 * _client_engine_retry_abandoned_reap, until a thread is available. A
 * release that drops the count back to zero in the meantime repeats this
 * whole sequence itself.
 *
 * The cost is an engine that stays up while the process has no thread to
 * spare. An inline teardown is a crash.
 */
static void _client_engine_spawn_reaper(void) {
  ccol_thread_id_t reaper;
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
    /* The handle is published in the same critical section as the creation
     * of the thread. The reaper takes this mutex before it does anything,
     * and it clears `stopping` under it. No second spawn can therefore
     * happen before this handle is published, because a second spawn needs
     * an acquire, which waits for `stopping` to clear. A handle published
     * after the unlock can land after the reaper finished and after a
     * second spawn published its own handle, and then it overwrites a
     * handle that nobody joined. */
    ccol_mutex_lock(cli_engine_bundler.mutex);
    rc = ccol_thread_create(reaper, _client_engine_reaper_fn, NULL);
    if (rc == 0) {
#ifdef RUNNING_UNIT_TESTS
      atomic_fetch_add(&g_reaper_created_for_tests, 1u);
      _reaper_spawn_hook_for_tests();
#endif
      /* A reaper that is still joinable here has already finished, because
       * a spawn needs `stopping` to be clear, and only the end of a reaper
       * clears it. The join never blocks, and it keeps the handle from
       * being lost. */
      _client_engine_join_reaper_if_needed_locked();
      cli_engine_bundler.reaper_thread = reaper;
      cli_engine_bundler.reaper_joinable = true;
      atomic_store(&g_engine_reap_abandoned, false);
    }
    ccol_mutex_unlock(cli_engine_bundler.mutex);
    if (rc == 0) break;
  }
  if (rc != 0) {
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add(&g_reaper_abandoned_count_for_tests, 1u);
#endif
    ccol_mutex_lock(cli_engine_bundler.mutex);
    cli_engine_bundler.stopping = false;
    bool first_give_up = !atomic_exchange(&g_engine_reap_abandoned, true);
    ccol_cond_var_broadcast(cli_engine_bundler.stopped_cv);
    ccol_mutex_unlock(cli_engine_bundler.mutex);
    if (first_give_up)
      _CLIENT_ENGINE_LOG_INFO(
          "The http client reactor engine could not be reaped (no thread "
          "available); the deadline sweep retries the teardown");
    return;
  }
}

/* This retries the teardown of an engine that _client_engine_spawn_reaper
 * left running with no reference. Only the deadline sweep calls it, once per
 * tick. The sweep is a thread of the engine that no dispatch runs on and that
 * holds no lock here, and the reaper that this spawns joins it; the join
 * waits only for the sweep to return to its loop and see its stop flag. A
 * stranded engine therefore goes down as soon as a thread is available, and
 * does not wait for an acquire and a release that may never come. */
static void _client_engine_retry_abandoned_reap(void) {
  if (!atomic_load_explicit(&g_engine_reap_abandoned, memory_order_relaxed))
    return;
  bool reap = false;
  ccol_mutex_lock(cli_engine_bundler.mutex);
  if (atomic_load(&g_engine_reap_abandoned) && cli_engine_bundler.reactor &&
      cli_engine_bundler.reactor_refs == 0 && !cli_engine_bundler.stopping) {
    cli_engine_bundler.stopping = true;
    reap = true;
  }
  ccol_mutex_unlock(cli_engine_bundler.mutex);
  if (reap) _client_engine_spawn_reaper();
}

/*
 * This takes a reference to the reactor of chttpclient. The first call
 * creates that reactor. It also creates cli_engine_bundler.dns_pool and
 * starts the deadline sweep. A later call only adds one to
 * cli_engine_bundler.reactor_refs. Each call MUST have exactly one
 * _client_engine_release() call to match it.
 */
static ccol_retval_t _client_engine_acquire_impl(bool for_chain) {
  ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  /* The exit hook must be registered after OpenSSL registers its own
   * cleanup, so that it runs before that cleanup. See _client_at_exit. */
  ccol_call_once(cli_engine_bundler.exit_hook_once, _client_exit_hook_install);
#if CCOL_FORK_SAFETY_REQUIRED
  ccol_call_once(cli_engine_bundler.atfork_once, _client_atfork_install);
#endif
  ccol_mutex_lock(cli_engine_bundler.mutex);
  _client_engine_forget_inherited_locked();
  while (cli_engine_bundler.stopping)
    ccol_cond_var_wait(cli_engine_bundler.stopped_cv, cli_engine_bundler.mutex);
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
    cli_engine_bundler.reactor = ccol_event_loop_create_with_mprocs(
        256, 4, nthreads, cli_engine_bundler.mprocs, &err);
    if (!cli_engine_bundler.reactor) {
      ccol_mutex_unlock(cli_engine_bundler.mutex);
      return ccol_not_enough_memory;
    }

    char *dns_err = NULL;
    cli_engine_bundler.dns_pool =
        ccol_create_cthread_pool(nthreads, 0, &dns_err);
    if (!cli_engine_bundler.dns_pool) {
      /* Copy the reactor handle out and clear the struct field BEFORE the
       * unlock of the mutex. A concurrent _client_engine_acquire then sees
       * a clean "nothing built yet" state and builds the engine itself.
       * Without this, that call sees a reactor field that still looks
       * live, while this thread blocks on a join of every reactor thread.
       *
       * The blocking destroy runs AFTER the unlock. Nothing else needs
       * this mutex to make progress at this exact point. This new reactor
       * has zero registrations, and the deadline sweep thread does not run
       * yet. A hold of the mutex across the destroy therefore buys no
       * correctness. It only blocks every other thread that wants to
       * acquire, release or reconfigure the engine for the whole join. */
      ccol_event_loop stale_reactor = cli_engine_bundler.reactor;
      cli_engine_bundler.reactor = CCOL_EVENT_LOOP_INVALID;
      ccol_mutex_unlock(cli_engine_bundler.mutex);
      __ccol_event_loop_destroy(stale_reactor);
      return ccol_not_enough_memory;
    }

    if (_client_deadline_sweep_start() != ccol_success) {
      /* This uses the same reasoning as the branch just above for a failed
       * dns_pool creation. Clear both fields under the mutex. Then unlock
       * it before the two blocking destroys. The deadline sweep thread did
       * not even start here. Nothing else can depend on either handle at
       * this point. The branch below for a failed logger allocation is
       * different, because it has a sweep thread that already runs. */
      ctpool stale_dns_pool = cli_engine_bundler.dns_pool;
      ccol_event_loop stale_reactor = cli_engine_bundler.reactor;
      cli_engine_bundler.dns_pool = CTPOOL_INVALID;
      cli_engine_bundler.reactor = CCOL_EVENT_LOOP_INVALID;
      ccol_mutex_unlock(cli_engine_bundler.mutex);
      __ctpool_destroy(stale_dns_pool);
      __ccol_event_loop_destroy(stale_reactor);
      return ccol_unexpected_failure;
    }

    if (!cli_engine_bundler.logger && cli_engine_bundler.user_logger)
      cli_engine_bundler.logger = cli_engine_bundler.user_logger;
    if (!cli_engine_bundler.logger) {
      cli_engine_bundler.logger =
          clog_open_fd_mp(2, CLOG_FATAL, NULL, cli_engine_bundler.mprocs);
      if (!cli_engine_bundler.logger) {
        /* The sweep thread ALREADY runs at this point. The three rollback
         * branches above, for a failed reactor, dns_pool or sweep, are
         * different. This branch can therefore not destroy everything
         * inline in the same way.
         *
         * The sweep thread itself uses _CLIENT_ENGINE_LOG_INFO, which
         * takes cli_engine_bundler.mutex. A join of that thread while this
         * code still holds that same mutex is a deadlock. The current tick
         * of the sweep thread can be blocked on exactly this lock.
         *
         * An unlock first and a join afterwards is not safe either. In
         * that window a concurrent _client_engine_acquire call sees
         * cli_engine_bundler.reactor as non-NULL. It then skips this whole
         * "create everything" branch. It hands out a live reference to a
         * reactor that this thread is about to destroy.
         *
         * This branch does what _client_engine_release() does when the
         * last reference drops and the teardown must not block the calling
         * thread. It marks the bundle as stopping, so that the wait loop
         * at the top of a concurrent acquirer blocks instead of going on.
         * It then hands the teardown to a new reaper thread. The result is
         * the same as a reference that this acquire takes and frees at
         * once, although it never grants that reference.
         *
         * Without this, a failed logger allocation here leaks the reactor,
         * the dns_pool and the sweep thread for the rest of the life of
         * the process. reactor_refs never leaves 0, so nothing ever calls
         * _client_engine_release() to reap them. */
        cli_engine_bundler.stopping = true;
        ccol_mutex_unlock(cli_engine_bundler.mutex);
        _client_engine_spawn_reaper();
        return ccol_not_enough_memory;
      }
      clog_set_field(cli_engine_bundler.logger, "component",
                     "http-client-engine");
    }

    ccol_log_info(cli_engine_bundler.logger,
                  "New http client reactor engine has been created");
  }
  cli_engine_bundler.reactor_refs++;
  if (for_chain) {
    cli_engine_bundler.chains_live++;
    atomic_fetch_add_explicit(&g_async_requests_pending, 1,
                              memory_order_relaxed);
    /* A waiting exit hook gives up once a request is in flight again. */
    if (cli_engine_bundler.exit_waiting)
      ccol_cond_var_broadcast(cli_engine_bundler.stopped_cv);
  }
  ccol_mutex_unlock(cli_engine_bundler.mutex);
  return ccol_success;
}

static ccol_retval_t _client_engine_acquire(void) {
  return _client_engine_acquire_impl(false);
}

/* This takes the one engine reference of an async chain. The reference
 * counts in chains_live, and the request that it serves counts in
 * g_async_requests_pending until its future is fulfilled. Every success
 * pairs with exactly one _client_engine_release_chain(). */
static ccol_retval_t _client_engine_acquire_chain(void) {
  return _client_engine_acquire_impl(true);
}

/* This takes back the pending count of an async request whose future is
 * about to be fulfilled. It runs once for each request, before the
 * fulfilment itself, so that a caller that sees the future ready also sees
 * the count without that request. */
static inline void _client_async_request_settled(void) {
  atomic_fetch_sub_explicit(&g_async_requests_pending, 1, memory_order_release);
}

/*
 * This frees a reference that _client_engine_acquire() took. After
 * cli_engine_bundler.reactor_refs goes back to zero, it hands the teardown
 * to a new reaper thread. It does not run the teardown inline.
 *
 * This is necessary and not a matter of style. A reactor callback thread
 * often calls this function itself. _async_on_error, which tears down the
 * last live ctx, is one such caller. Such a thread must never block on a
 * join of the deadline sweep. It must never block on a drain of
 * cli_engine_bundler.dns_pool either. An inline teardown of either one
 * from a reactor callback thread is a real hang.
 */
static void _client_engine_release_impl(bool for_chain) {
  bool should_reap = false;
  ccol_mutex_lock(cli_engine_bundler.mutex);
  if (for_chain && cli_engine_bundler.chains_live > 0 &&
      --cli_engine_bundler.chains_live == 0 && cli_engine_bundler.exit_waiting)
    ccol_cond_var_broadcast(cli_engine_bundler.stopped_cv);
  if (cli_engine_bundler.reactor_refs > 0) cli_engine_bundler.reactor_refs--;
  if (cli_engine_bundler.reactor_refs == 0 && cli_engine_bundler.reactor) {
    cli_engine_bundler.stopping = true;
    should_reap = true;
  }
  ccol_mutex_unlock(cli_engine_bundler.mutex);
  if (should_reap) _client_engine_spawn_reaper();
}

static void _client_engine_release(void) { _client_engine_release_impl(false); }

/* This frees the reference that _client_engine_acquire_chain took. */
static void _client_engine_release_chain(void) {
  _client_engine_release_impl(true);
}

/*
 * This blocks until an in-flight reaper thread finishes the teardown of the
 * resources of this module. See _client_engine_release. It does nothing
 * when the engine is not stopping. That covers an engine that does not run
 * at all. It also covers an engine that runs and stays up because other
 * Tier 2 or Tier 3 references are still live.
 *
 * This is NOT "block until the engine stops on its own". That would hang
 * forever against a healthy engine that still has references. A caller
 * uses this only to wait for a teardown that it triggered itself, by a
 * release of its own last reference.
 *
 * RUNNING_UNIT_TESTS gates this function. This module exposes no public
 * equivalent of chttpsvr_engine_wait() on the server side. The engine
 * starts and stops on its own as Tier 2 and Tier 3 use comes and goes, and
 * _client_at_exit does its own wait at the exit of the process. This
 * primitive therefore has no caller in a production build. It exists only
 * for _chttpclient_engine_wait_for_quiescence_for_tests below.
 */
#ifdef RUNNING_UNIT_TESTS
static void _client_engine_wait_for_quiescence(void) {
  ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  ccol_mutex_lock(cli_engine_bundler.mutex);
  _client_engine_forget_inherited_locked();
  while (cli_engine_bundler.stopping)
    ccol_cond_var_wait(cli_engine_bundler.stopped_cv, cli_engine_bundler.mutex);
  _client_engine_join_reaper_if_needed_locked();
  ccol_mutex_unlock(cli_engine_bundler.mutex);
}
#endif /* RUNNING_UNIT_TESTS */

/*
 * This runs once, from the atexit(3) list, when the process exits, and also
 * from the unload of this library, where glibc runs the exit handlers that a
 * shared object registered. It stops every thread that the library started
 * for Tier 2 and Tier 3 before OpenSSL releases its own global state. The
 * engine otherwise stops on a reaper thread in the background once its last
 * reference goes, and a process that returns from main at that moment runs
 * the cleanup of OpenSSL while a reactor thread, a DNS worker or the reaper
 * itself still frees the per-thread state of OpenSSL. That is a double free.
 * A reaper that nothing joins is also a thread that a leak checker reports.
 *
 * The order depends on the registration. _client_exit_hook_install calls
 * OPENSSL_init_ssl() first, which registers the cleanup of OpenSSL, and
 * registers this function after it. The atexit list runs in the reverse of
 * the order of registration, so this runs first. A destructor of the shared
 * object would run after every atexit handler, which is too late.
 *
 * What it does, in order:
 *  - Nothing in a process that did not build the engine, such as a child
 *    of fork() that inherited a running engine without its threads.
 *  - Nothing while an async request is in flight. Its write_fn can be the
 *    very code that called exit(), on a thread that the teardown joins. A
 *    request in flight at exit is a request that the application abandons.
 *  - It waits for every chain whose future is already fulfilled to let go
 *    of the engine. The reactor does that at once.
 *  - It destroys the default client when no call of Tier 1 runs on it. Its
 *    idle connections of Tier 2 hold references to the engine, and its TLS
 *    state must go before OpenSSL does.
 *  - It waits for a teardown in progress, joins the reaper, and tears down
 *    an engine that no reference holds any more on this thread.
 * An engine that a client which the application never destroyed still holds
 * through its idle connections keeps running, and so do its threads. */
static void _client_at_exit(void) {
  if (atomic_load(&g_client_engine_pid) != getpid()) return;
  if (atomic_load_explicit(&g_async_requests_pending, memory_order_acquire))
    return;
  ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);

  ccol_mutex_lock(cli_engine_bundler.mutex);
  cli_engine_bundler.exit_waiting = true;
  while (cli_engine_bundler.chains_live > 0 &&
         atomic_load(&g_async_requests_pending) == 0)
    ccol_cond_var_wait(cli_engine_bundler.stopped_cv, cli_engine_bundler.mutex);
  cli_engine_bundler.exit_waiting = false;
  bool in_flight = cli_engine_bundler.chains_live > 0;
  ccol_mutex_unlock(cli_engine_bundler.mutex);
  if (in_flight) return;

  _default_client_destroy_if_idle();

  ccol_mutex_lock(cli_engine_bundler.mutex);
  while (cli_engine_bundler.stopping)
    ccol_cond_var_wait(cli_engine_bundler.stopped_cv, cli_engine_bundler.mutex);
  _client_engine_join_reaper_if_needed_locked();
  bool reap_here =
      cli_engine_bundler.reactor && cli_engine_bundler.reactor_refs == 0;
  if (reap_here) {
    cli_engine_bundler.stopping = true;
    atomic_store(&g_engine_reap_abandoned, false);
  }
  ccol_mutex_unlock(cli_engine_bundler.mutex);
  /* An engine with no reference and no reaper is one whose reaper could not
   * be created. This thread is not a thread of the engine, so it runs the
   * teardown itself. */
  if (reap_here) (void)_client_engine_reaper_fn(NULL);
}

static void _client_exit_hook_install(void) {
  atomic_store(&g_client_engine_pid, getpid());
  /* OPENSSL_init_ssl registers the cleanup of OpenSSL with atexit(3) on its
   * first call. A later call only reports success. */
  (void)OPENSSL_init_ssl(0, NULL);
  (void)atexit(_client_at_exit);
}

ccol_retval_t chttpcli_set_engine_logger(clog cl) {
  if (!cl) return ccol_invalid_args;
  clog derived = clog_derive(cl);
  if (!derived) return ccol_not_enough_memory;
  clog_set_field(derived, "component", "http-client-engine");
  ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  ccol_mutex_lock(cli_engine_bundler.mutex);
  /* The logger that the engine uses right now is either the previous
   * user_logger or a fallback of this engine run. Both close here. A fallback
   * is replaced only while the engine runs, because only then is it open. */
  clog old_user = cli_engine_bundler.user_logger;
  clog old_active = cli_engine_bundler.logger;
  cli_engine_bundler.user_logger = derived;
  if (old_active) cli_engine_bundler.logger = derived;
  ccol_mutex_unlock(cli_engine_bundler.mutex);
  if (old_active && old_active != old_user) clog_close(old_active);
  if (old_user) clog_close(old_user);
  return ccol_success;
}

/* This closes the logger that chttpcli_set_engine_logger installed, at the
 * unload of this module or at the exit of the process. It does so only when
 * the engine is fully stopped. A running engine still logs through it, and
 * then it stays open, like any other handle that is still in use at exit.
 * The order against the destructor of clogger does not matter: whichever of
 * the two runs last releases the slot table of clogger. */
__attribute__((destructor)) static void _cleanup_engine_user_logger(void) {
  ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  ccol_mutex_lock(cli_engine_bundler.mutex);
  clog user = CLOG_INVALID;
  if (!cli_engine_bundler.reactor && !cli_engine_bundler.stopping &&
      cli_engine_bundler.logger != cli_engine_bundler.user_logger) {
    user = cli_engine_bundler.user_logger;
    cli_engine_bundler.user_logger = CLOG_INVALID;
  }
  ccol_mutex_unlock(cli_engine_bundler.mutex);
  if (user) clog_close(user);
}

ccol_retval_t chttpcli_set_engine_mem_mgmt_procs(ccol_memmgmt_procs_t *mp) {
  if (mp && (!mp->malloc || !mp->free || !mp->calloc || !mp->realloc))
    return ccol_invalid_args;
  ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  ccol_mutex_lock(cli_engine_bundler.mutex);
  _client_engine_forget_inherited_locked();
  if (cli_engine_bundler.reactor) {
    ccol_mutex_unlock(cli_engine_bundler.mutex);
    return ccol_not_permitted;
  }
  if (mp) {
    cli_engine_bundler.mprocs_storage = *mp;
    cli_engine_bundler.mprocs = &cli_engine_bundler.mprocs_storage;
  } else {
    cli_engine_bundler.mprocs = NULL;
  }
  ccol_mutex_unlock(cli_engine_bundler.mutex);
  return ccol_success;
}

ccol_retval_t chttpcli_set_engine_num_reactor_threads(size_t num_threads) {
  ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  ccol_mutex_lock(cli_engine_bundler.mutex);
  _client_engine_forget_inherited_locked();
  if (cli_engine_bundler.reactor) {
    ccol_mutex_unlock(cli_engine_bundler.mutex);
    return ccol_not_permitted;
  }
  cli_engine_bundler.num_reactor_threads = num_threads;
  ccol_mutex_unlock(cli_engine_bundler.mutex);
  return ccol_success;
}

/* ========================================================================== */
/*                    ASYNC CONNECTION STATE MACHINE (STEP A + B)             */
/* ========================================================================== */

/*
 * This is the engine of Tier 2 and Tier 3. It runs one non-blocking HTTP or
 * HTTPS request and response cycle for each connection. It adds redirect
 * support on top of a pipeline for one hop. The ccol_event_loop reactor
 * of chttpclient drives it. See the "SHARED STATIC REACTOR" section above.
 *
 * TLS: this engine uses the same reactor-agnostic client-mode ctls API as
 * Tier 1. That API is ctls_conn_create_client, ctls_conn_handshake_step,
 * ctls_conn_read and ctls_conn_write. Here the on_readable and on_writable
 * callbacks drive it, and not a blocking poll() loop.
 *
 * This code needs no tls_lock. ccol_event_loop has its own dispatch_lock
 * for each event_entry. See the documentation of cthreadcomm. That lock
 * guarantees that the library never calls the callback of one registration
 * concurrently with itself. It gives a stronger guarantee too: it never
 * dispatches both directions of one fd, on_readable and on_writable,
 * concurrently with each other. That guarantee is what lets this module
 * drive OpenSSL on one shared connection from either callback with no lock
 * of its own.
 *
 * A re-arm of write readiness needs no special case. ccol_event_loop is
 * level-triggered. It keeps a registration live until an explicit remove.
 * The library calls ccol_event_loop_modify instead, and only to flip which
 * directions are of interest. There is therefore no window for a forgotten
 * re-arm of a one-shot write interest. This holds for a raw TLS write that
 * bypasses the queue.
 *
 * The identity of a connection is its raw fd plus the ccol_event_reg handle
 * that ccol_event_loop_add returns for its current registration. That
 * handle is CCOL_EVENT_REG_INVALID while no registration is active. A
 * connection that is mid-connect in the DNS and connect pool is one such
 * case. An idle pooled connection is another. ccol_event_loop also has its
 * own generation counter, ccol_event_loop_reg_generation. That counter is
 * available for defensive bookkeeping, and correctness here does not need
 * it. The dispatch path of ccol_event_loop already checks the liveness of a
 * registration again under its own lock at dispatch time. A stale entry of
 * an already fetched epoll_wait batch, for a registration that the library
 * removed, is therefore always a safe no-op.
 *
 * ccol_event_loop_remove protects only the internal structures of
 * ccol_event_loop itself, with deferred and epoch-based frees. It does NOT
 * protect the ctx payload of this module from a dispatch that was already in
 * flight at the time of the remove. Every place that ends a connection
 * therefore goes through one of two small explicit teardown helpers. Those
 * helpers are _async_ctx_finish and _async_idle_ctx_finish, and the code
 * defines them after the idle pool section below. No place depends on an
 * implicit "on_close runs at some point" guarantee. ccol_event_loop has no
 * unconditional terminal callback, so this module must end a connection
 * explicitly.
 *
 * The lifetime of a redirect chain: a redirect chain spans more than one
 * connection. It uses one chttp_async_ctx_t for each hop. This matches the
 * "a fresh chttp_parse_ctx_t per hop" comment of Tier 1. The chain must
 * still fulfil the future of the caller exactly once. It must also free
 * exactly one engine reference for the whole chain.
 *
 * chttp_async_chain_t is the small heap struct that carries that shared
 * state. It outlives the ctx of any single hop. It holds the future and the
 * fulfilled-once guard. It also holds an owned deep copy of the headers and
 * the body of the original request. It holds the pinned TLS context and a
 * refcount as well. The TLS context
 * is constant across hops, exactly like the tls_ctx local of Tier 1. The
 * refcount tracks how many ctx of hops are live now.
 *
 * Every ctx teardown path frees one chain reference. After the count
 * reaches zero, _async_chain_release frees the chain. It also has a backstop
 * that the fulfilled-once guard protects. That backstop fulfils the future
 * with a generic error if nothing else already did so.
 */

/* The result of the ctpool_future is a chttpcli_async_result_t. That type
 * is public, and chttpclient.h declares it. _async_fulfill_chain below
 * builds the result. */

typedef enum {
  CHTTP_ASYNC_CONNECTING,
  CHTTP_ASYNC_TLS_HANDSHAKING,
  CHTTP_ASYNC_WRITING,
  CHTTP_ASYNC_AWAITING_CONTINUE, /* This state is for "Expect: 100-continue"
                                  * only. The library wrote the whole header
                                  * block, and this ctx is registered for
                                  * READ. It waits up to
                                  * ctx->continue_deadline for one of three
                                  * events. An interim "100 Continue" makes
                                  * it write the body. A direct final
                                  * response means that it never sends the
                                  * body. The deadline sweep can also flip
                                  * this registration to WRITE as a timeout
                                  * signal, which makes it send the body
                                  * anyway. The deadline bounds only the
                                  * wait for the first byte of a message; a
                                  * message that started before it is read
                                  * to its end. See the field comments of
                                  * ctx->want_100_continue,
                                  * ctx->continue_decided and
                                  * ctx->continue_msg_started for the full
                                  * design. */
  CHTTP_ASYNC_READING,
  CHTTP_ASYNC_IDLE, /* This ctx sits in cli->idle_pools_async. No chain owns
                     * it, so ctx->chain is NULL. See the "ASYNC IDLE POOL"
                     * section below. */
} chttp_async_state_t;

/*
 * This is the shared state of a redirect chain. There is one such struct
 * for each request, and not one for each hop. See the comment above for
 * the reasons behind the ownership and the lifetime.
 */
typedef struct {
  ccol_memmgmt_procs_t *mp;
  struct chttpclient *cli; /* The client that owns this chain.
                            * _async_submit_hop needs it to reach the async
                            * idle pool, cli->idle_pools_async. It takes a
                            * reusable connection from that pool, and it
                            * offers one back to it. */
  ctpool_future *future;   /* The caller holds a separate reference of its
                            * own. See the return value of
                            * _chttp_do_async_internal. This module holds the
                            * producer-side reference. It only ever calls
                            * ctpool_future_fulfill on it. */

  chttpcli_write_fn write_fn; /* This is NULL for a buffered request from
                               * chttpclient_do_async. It is non-NULL for a
                               * streaming request from
                               * chttpclient_do_async_streaming.
                               * _async_submit_hop and _async_retry_hop wire
                               * it into ctx->pctx.requested_sink_fn of every
                               * hop. It is constant across the whole chain.
                               * This matches the identical streaming and
                               * write_fn locals of Tier 1 in
                               * chttp_do_internal. The library calls it on
                               * the reactor thread that drives the on_data
                               * callback of this hop. See the public
                               * documentation of this field on
                               * chttpclient_do_async_streaming. That
                               * documentation gives the contract that the
                               * function must not block. */
  void *write_ctx;            /* The library passes this to write_fn exactly
                               * as it is. */

  chmap req_headers; /* An owned deep copy of the headers of the original
                      * request. It is a chmap(char* -> char*). The library
                      * sends it again without a change on every hop. This
                      * matches hop_req.headers of Tier 1. It is NULL when
                      * the original request had no headers. */
  void *body_data;   /* An owned deep copy of the body bytes of the original
                      * request. It is NULL when the original request had no
                      * body. The library sends it again exactly as it is on
                      * a 307 or 308 hop. A redirect that does not preserve
                      * the method drops it. Such a redirect is a 301, 302 or
                      * 303 with a method other than HEAD. */
  size_t body_len;
  char *body_content_type; /* An owned copy. It is NULL when there is
                            * none. */
  bool body_dropped;       /* This latches to true, permanently, the first
                            * time that a redirect which does not preserve
                            * the method drops the body.
                            *
                            * chttp_do_internal of Tier 1 keeps the same
                            * state in a different way. After a drop, it
                            * overwrites its own cur_body loop local with an
                            * empty body. A LATER 307 or 308 on the same
                            * chain therefore keeps the current body, which
                            * can already be empty, and not the original one.
                            *
                            * _async_handle_redirect has no per-ctx
                            * equivalent of the cur_body local of Tier 1. A
                            * chttp_async_ctx_t exists only for the hop that
                            * is in flight now. This chain-level flag is
                            * therefore what carries "once dropped, stays
                            * dropped" across hops here.
                            *
                            * Without this flag, a 307 or 308 hop after an
                            * earlier downgrade brings body_data, body_len
                            * and body_content_type back. It would take them
                            * from the ORIGINAL hop-0 values of this struct,
                            * and not the empty body that the chain already
                            * moved to.
                            *
                            * Only _async_handle_redirect changes this field.
                            * That function always runs synchronously. It
                            * runs on the thread that just finished the parse
                            * of the response of a hop. That thread calls
                            * _async_submit_hop for the next hop from the
                            * same call stack. This field therefore needs no
                            * lock, for the same reason as carried_auth and
                            * redirect_drops below. See the
                            * comments of those fields in this struct. */

  char *carried_auth;        /* The Authorization value that the library
                              * injects from the userinfo of the URL. It
                              * carries forward across hops. This matches the
                              * identical carried_auth local of Tier 1 in
                              * chttp_do_internal. See the comment of that
                              * function for the full contract. That contract
                              * carries the value on the same origin. It
                              * drops the value permanently across origins.
                              * This field is NULL when the chain has seen no
                              * userinfo at all.
                              *
                              * Only _async_submit_hop changes this field. It
                              * changes it at the same point where the
                              * decision about a method and body downgrade
                              * already changes this chain with no lock. See
                              * the comment of that function for why no lock
                              * is needed. */
  char *carried_auth_origin; /* The origin_key that the library derived the
                              * field above for. */

  char *initial_origin_key; /* The origin_key of hop 0, which is the request
                             * as the caller submitted it.
                             * _chttp_do_async_internal captures it once,
                             * before the chain queues its first hop. The
                             * library uses it only to detect a redirect
                             * across origins for the check below. This field
                             * never changes for the whole lifetime of the
                             * chain, and carried_auth_origin does change. */
  unsigned redirect_drops;  /* The CHTTP_REDIRECT_DROP_* flags of the hop
                             * that is being prepared. _async_submit_hop
                             * computes them again for every hop, against
                             * initial_origin_key. This matches the
                             * redirect_drops local of chttp_do_internal in
                             * Tier 1. See CHTTP_REDIRECT_DROP_CREDENTIALS
                             * for the headers that each flag removes.
                             *
                             * Only _async_submit_hop changes this field. It
                             * changes it at the same point where
                             * carried_auth and carried_auth_origin already
                             * change this chain with no lock. See the
                             * comment of that field for why no lock is
                             * needed. */

  ctls_ctx_t *tls_ctx; /* The library pins this once for the whole chain,
                        * with a ctls_ctx_retain on cli->tls_ctx. This
                        * matches the tls_ctx local of Tier 1. A redirect can
                        * hop between http and https. This context must
                        * therefore outlive every hop, whatever scheme the
                        * chain started with. The library frees it once with
                        * ctls_ctx_release when it frees the chain. */
  bool tls_ctx_usable;
  bool verify_host;
  /* This is the value of cli->tls_generation at the time that the library
   * pinned tls_ctx above. The two therefore describe one configuration for
   * the whole lifetime of this chain. The library stamps this value onto
   * every ctx that this chain creates. It compares the value against the
   * current value of the client before it reuses a pooled connection. See
   * the field comment of chttpclient.tls_generation. */
  uint64_t tls_generation;

  uint64_t connect_timeout_us;       /* The library reads this again into
                                      * ctx->connect_deadline at the start of
                                      * every hop that really connects. The
                                      * path that reuses a connection never
                                      * reads it, because that path skips
                                      * CONNECTING and TLS_HANDSHAKING. See
                                      * the "ASYNC DEADLINE SWEEP" section
                                      * below. */
  chttp_deadline_t overall_deadline; /* The library computes this once, here,
                                      * for the whole lifetime of the chain.
                                      * This matches overall_dl of Tier 1,
                                      * which it computes once before its hop
                                      * loop and never resets for a hop. */
  size_t max_response_body_size;     /* A snapshot of
                                      * cli->max_response_body_size.
                                      * _chttp_do_async_internal takes it
                                      * once. This matches
                                      * connect_timeout_us and
                                      * overall_deadline above.
                                      * _async_submit_hop and
                                      * _async_retry_hop copy it into
                                      * ctx->bb.max_size of every hop. A
                                      * value of 0 means no limit. */
  /* A snapshot of the prevent_tls_downgrade_on_redirect field of the original
   * chttp_request_t. _chttp_do_async_internal takes it once, in the same way
   * as expect_continue below, and every hop of this chain reads it. */
  bool prevent_tls_downgrade_on_redirect;
  bool expect_continue; /* A snapshot of the expect_continue field of the
                         * original chttp_request_t.
                         * _chttp_do_async_internal takes it once. This
                         * matches connect_timeout_us and
                         * max_response_body_size above.
                         *
                         * _async_submit_hop still computes for each HOP
                         * whether that hop goes through the
                         * "Expect: 100-continue" wait. This matches the
                         * identical per-hop use_100_continue local of
                         * Tier 1. Tier 1 splits a request-level flag from a
                         * per-hop decision in the same way. A redirect can
                         * downgrade the method or drop the body. The wait is
                         * then pointless for a later hop, although this flag
                         * itself never changes for the lifetime of the
                         * chain. */

  ccol_mutex_t lock; /* This guards fulfilled and refcount. Reactor callbacks
                      * of hops change both, and those callbacks can run
                      * concurrently. See the comment above this struct. The
                      * on_close of an old hop can run at the same time as
                      * the on_data or the on_ready of a new hop. That
                      * happens after the redirect handoff queues the next
                      * connection. */
  bool fulfilled;
  /* The result that the one fulfilment of the future hands over. The chain
   * gets it before it exists, so that the fulfilment itself allocates
   * nothing and can never degrade to a NULL result, which reads as a
   * cancelled future. It is NULL once the future has it. */
  chttpcli_async_result_t *result;
  int refcount; /* The number of live per-hop ctx that reference this chain.
                 * It reaches zero exactly once. That happens when the
                 * teardown of the last hop runs and no further hop is queued
                 * to take over. */
} chttp_async_chain_t;

/* See the "ASYNC HAPPY EYEBALLS" section. */
typedef struct chttp_connect_race_s chttp_connect_race_t;

/* The values of ctx->flip_watch. See _async_upload_pause. */
#define CHTTP_FLIP_NONE 0
/* The send blocked and waits for the write direction; a response that
 * arrives meanwhile turns the registration to the read direction. */
#define CHTTP_FLIP_ON_INPUT 1
/* The send is paused in duplex mode and waits for the read direction; room
 * to write turns the registration to the write direction. */
#define CHTTP_FLIP_ON_ROOM 2

/* This struct has a tag and is not anonymous. deadline_prev and
 * deadline_next below must point at the struct itself. An anonymous struct
 * has no name to spell such a pointer with. */
typedef struct chttp_async_ctx_s {
  ccol_memmgmt_procs_t *mp;
  struct chttpclient *cli; /* The client that owns this ctx. While the ctx is
                            * idle, chain is NULL, and the ctx needs this
                            * field to reach cli->idle_pools_async. While the
                            * ctx is active, it needs the field to offer this
                            * connection back to the pool after a completion
                            * that leaves the connection reusable. */
  chttp_async_chain_t
      *_Atomic chain;        /* The shared state of the whole redirect chain.
                              * The ctx does not own it. See
                              * _async_ctx_teardown. It is NULL while the
                              * idle pool holds the ctx.
                              *
                              * It is _Atomic, like state and fd below, so
                              * that the deadline sweep can read it with no
                              * idle_lock. See the comment of that lock and
                              * the "ASYNC DEADLINE SWEEP" section. They
                              * explain why single atomic loads are enough
                              * there. The stronger COMPOUND guarantee of
                              * idle_lock, which covers state and chain
                              * together, is still necessary. This field
                              * leaves that guarantee completely undisturbed
                              * for every idle_lock call site. */
  int hop;                   /* The 0-based hop index of THIS connection. */
  chttp_method_t cur_method; /* The method that the library used to build the
                              * wire bytes of THIS hop. It is the basis for
                              * the method of the next hop on a redirect.
                              * This matches the cur_method loop variable of
                              * Tier 1. */
  _Atomic chttp_async_state_t state;
  _Atomic int fd; /* This is -1 until the connect task gets a real fd. */
  _Atomic(ccol_event_reg)
      reg; /* The current ccol_event_loop registration. It is
            * CCOL_EVENT_REG_INVALID while no registration is active. A
            * mid-connect ctx in the DNS and connect pool is one such case.
            * An idle pooled ctx with no direction of interest yet is
            * another. _async_connect_task sets this field exactly once,
            * right after ccol_event_loop_add returns.
            *
            * The field is _Atomic because that assignment is NOT invisible
            * until a dispatch could care about it. The internal
            * registration of ccol_event_loop_add goes live as part of the
            * call itself. Another reactor thread can dispatch it from that
            * moment. That call can therefore finish and hand a callback to a
            * DIFFERENT thread before the `ctx->reg = ccol_event_loop_add(
            * ...)` assignment of this thread finishes. This is especially
            * likely for a loopback connect, which is often writable the
            * moment that the library registers it.
            *
            * A plain non-atomic pointer here is a real data race. Every
            * dispatch callback reads this field lock-free, so every access
            * must be atomic. ThreadSanitizer reports the non-atomic form.
            * A read of the code does not.
            *
            * Every dispatch callback receives the handle of its own
            * registration from ccol_event_loop_add, and it stores that
            * handle here through _async_adopt_reg before anything reads
            * this field. A dispatch that runs before the assignment of
            * _async_connect_task finishes therefore still sees the correct
            * handle. The adopted value and the returned value are the same
            * handle, so the two stores never disagree. */
  _Atomic(ccol_event_reg)
      room_reg; /* A second registration of the same fd, in the write
                 * direction, that exists only while a duplex send is paused
                 * on a full send buffer: reg then watches for input and this
                 * one for room, so the send goes on the moment there is room
                 * and a response is still read at once. See
                 * _async_room_watch_start. It shares the dispatch lock of
                 * reg, because both belong to one fd. */

  bool is_unix;
  char *unix_socket_path; /* An owned copy. It is NULL unless is_unix. */
  char *host;             /* An owned copy. It is NULL when is_unix is true.
                           * The library passes port separately. */
  uint16_t port;
  chttp_addr_list_t *addrs; /* The owned candidate addresses of a TCP target.
                             * _async_connect_task resolves them for the
                             * connect. A list of more than one address
                             * moves to the race of ctx->race. See
                             * chttp_addr_list_t. It is NULL for a unix
                             * target, for a ctx that has not resolved yet,
                             * and once the connect succeeds. */
  _Atomic(chttp_connect_race_t *)
      race;     /* The Happy Eyeballs race of this connect, or NULL. See the
                 * "ASYNC HAPPY EYEBALLS" section. The connect task sets it
                 * before any attempt starts; the ctx holds one reference on it
                 * until the ctx is freed, after the ctx left the deadline
                 * registry, so the deadline sweep can always read it. */
  bool is_ipv6; /* host is a raw IPv6 literal with no brackets. This
                 * matches chttp_url_t.is_ipv6.
                 * _async_handle_redirect needs it to bracket the
                 * host again when it rebuilds a redirect target.
                 * Without it, that rebuild quietly produces a
                 * malformed "scheme://<unbracketed-ipv6>:port/path"
                 * target. The next hop then fails to parse it. */
  char *path_and_query; /* An owned copy of the path and query of the request
                         * of THIS HOP. "/a/b?x=1" is one example. This
                         * matches chttp_url_t.path_and_query.
                         *
                         * _async_submit_hop captures it again on EVERY hop,
                         * a reused connection included. The path can differ
                         * from one request to the next, although host, port
                         * and origin_key stay the same. The origin of the
                         * idle pool therefore stays the same too.
                         * _async_retry_hop carries this field across a
                         * retry, exactly like host, unix_socket_path and
                         * origin_key.
                         *
                         * _async_handle_redirect needs it to resolve a
                         * Location header with a relative path against the
                         * own URL of this hop. The per-hop chttp_url_t url
                         * local of Tier 1 already does the same.
                         *
                         * Without it, the chttp_url_t base that
                         * _async_handle_redirect builds by hand leaves this
                         * field NULL. A genuinely relative Location header
                         * then crashes the process with a NULL-pointer
                         * strchr() inside _merge_ref_path. Such a header is
                         * not an absolute path, not a full URL, and not
                         * protocol-relative. */
  char *origin_key;     /* An owned copy. It is "scheme://host:port" or
                         * "unix://<path>". It matches
                         * chttp_conn_t.origin_key of Tier 1. The library
                         * uses it to put this ctx into
                         * cli->idle_pools_async, and to take it out. */
  bool reused;          /* This is true when the connection of this hop came
                         * from the idle pool, and not from a fresh connect.
                         * It describes THIS attempt only. A retry always
                         * resets it to false. See _async_retry_hop. */
  bool any_bytes_read;  /* This becomes true after the library reads one or
                         * more response bytes for the CURRENT attempt. It
                         * gates the retry of a dead reused connection. This
                         * matches the any_bytes_read output parameter of
                         * Tier 1, which has the same name. */
  size_t interim_responses_seen; /* The number of CONSECUTIVE interim 1xx
                                  * responses that the library discarded
                                  * since the most recent message boundary
                                  * that it did not discard.
                                  *
                                  * This value must survive across dispatch
                                  * callback calls. The discard loop in
                                  * _async_on_readable_impl and in
                                  * _async_awaiting_continue_on_data can
                                  * return early in the middle of one skip.
                                  * It then waits for more bytes through a
                                  * later on_readable dispatch. The identical
                                  * cap of Tier 1 is different: it is a plain
                                  * stack loop counter in
                                  * _chttp_read_message_loop, because that
                                  * function blocks synchronously for the
                                  * whole loop.
                                  *
                                  * See the comment of
                                  * CHTTP_MAX_INTERIM_RESPONSES for why this
                                  * cap exists at all. Without it, a server
                                  * that never stops sending "103 Early
                                  * Hints" can keep this ctx alive forever.
                                  * It also keeps the chain and the future
                                  * that wait on that ctx alive.
                                  *
                                  * The library resets this to 0 on every
                                  * fresh attempt. A new connect and a retry
                                  * through _async_retry_hop are such
                                  * attempts. A reused connection from the
                                  * idle pool also starts at 0. The reset
                                  * block of _async_idle_pool_offer zeroes
                                  * every field of a hop attempt before it
                                  * pools the connection.
                                  *
                                  * The library ALSO resets this to 0 the
                                  * moment that
                                  * _async_awaiting_continue_on_data claims a
                                  * genuine "100 Continue". It does this
                                  * right before it hands off to the read
                                  * phase of the final response. See the
                                  * comment at that call site. A confirmed
                                  * "100 Continue" is itself a real message
                                  * that the library does not discard. The
                                  * run of consecutive discards that this
                                  * field tracks therefore restarts there.
                                  *
                                  * This is the same as in _chttp_send_and_read
                                  * of Tier 1. That function gives the continue
                                  * wait and the read of the final response to
                                  * two SEPARATE _chttp_read_message_loop
                                  * calls. Each call has its own counter.
                                  *
                                  * Without this reset, a hop can discard N
                                  * interim responses while it waits for
                                  * "100 Continue". Then only 64 - N remain for
                                  * the interim responses of the final
                                  * response, and not a full 64. This silently
                                  * breaks the documented contract of this cap:
                                  * 64 CONSECUTIVE discarded interim responses.
                                  * chttpclient_do_async(3) publishes that
                                  * contract. A confirmed "100 Continue" ends
                                  * the consecutive run. Therefore, it must not
                                  * count against the responses after it. */
  struct timespec last_used;     /* The library sets this when it offers the
                                  * connection to the idle pool. The
                                  * staleness check of _async_idle_pool_take
                                  * reads it. */
  bool upload_paused; /* A response started to arrive while the request
                       * was still going out, and the send waits for the
                       * read path to decide whether it goes on. See
                       * _async_upload_pause. Only the dispatch of this
                       * ctx reads and writes it. */
  _Atomic unsigned char
      flip_watch;         /* CHTTP_FLIP_*. The send of the request waits on
                           * one direction while the other one can matter,
                           * and the deadline sweep watches that other one;
                           * see _async_upload_pause. */
  bool answered_early;    /* A response message started while the send was
                           * paused, and did not end as an interim one.
                           * Only the dispatch of this ctx reads and writes
                           * it. See _async_request_sent. */
  bool duplex;            /* A final response of 200 to 299 streams in while
                           * the request still goes out. Only the dispatch of
                           * this ctx reads and writes it. */
  size_t upload_check_at; /* The value of wire_sent from which a write that
                           * succeeds checks again for a response that
                           * arrived meanwhile. See
                           * _async_upload_should_pause. */
  bool idle_tainted;      /* The dispatch of an idle ctx found input or the
                           * end of the stream on it, and the ctx was taken
                           * for a new hop before that dispatch could evict
                           * it. Bytes that arrive after a complete response
                           * belong to no request, so the new hop must not
                           * read them as its answer; see
                           * _async_reused_conn_is_clean. The offer to the
                           * idle pool clears it. Only the dispatches of
                           * this ctx read and write it. */
  bool reuse_checked;     /* _async_reused_conn_is_clean already ran for
                           * this hop. _async_submit_hop clears it. */
  bool tls_read_blocked;  /* The last ctls_conn_read of the read path
                           * returned EWOULDBLOCK. What the TLS layer still
                           * holds is then the start of a record that is not
                           * complete yet: ctls_conn_has_pending_input
                           * reports it, and no read takes it until the
                           * rest arrives on the socket. A read that takes
                           * bytes or sees the end clears it.
                           * _async_input_pending reads it, so that such a
                           * start does not pause a send that the socket
                           * would let go on, as the read_blocked of
                           * _chttp_send_all_watch does in Tier 1. Only the
                           * dispatch of this ctx reads and writes it. */
  bool resp_msg_started;  /* The parser holds the start of a response
                           * message: a byte of it arrived since the last
                           * message that the read path discarded. Only the
                           * dispatch of this ctx reads and writes it. */
  char *wire; /* The owned serialized bytes of the request. This module
               * always keeps ownership of them. It frees them after it
               * writes all of them, and wire_sent below tracks that. This is
               * true for the plain path and for the TLS path alike, because
               * there is no queue to hand ownership to. The free in
               * _async_ctx_free is always safe, and it does nothing after
               * the pointer is NULL. */
  size_t wire_len;
  size_t wire_sent; /* The number of bytes of wire that the library already
                     * wrote. ctls_conn_write and a raw write(2) both have
                     * ordinary short-write semantics. */

  bool is_https;
  bool verify_host;
  /* The library stamps this from the chain whenever a handshake for this
   * ctx is about to start. It keeps the value untouched across a cycle
   * through the idle pool. A pooled Tier 2 or Tier 3 connection therefore
   * still knows which TLS configuration its handshake used. See the field
   * comment of chttpclient.tls_generation. See also the eligibility check
   * of _async_idle_pool_take. This field has no meaning unless is_https is
   * true, and the library never reads it in that case. */
  uint64_t tls_generation;
#ifdef RUNNING_UNIT_TESTS
  /* The idle_lock hold that _async_idle_pool_take hands out maintains this
   * field. The three unlocks that free that hold maintain it too. A
   * shutdown site for an abandoned ctx can therefore report whether it ran
   * before or after that unlock. It does not assert about its own position.
   * See _chttpclient_abandon_shutdown_under_lock_for_tests. */
  bool idle_lock_held_for_tests;
#endif
  _Atomic bool
      hop_completed; /* The library sets this after it fully parses the
                      * response of this hop. At that point it either
                      * fulfils the future or hands off to a redirect.
                      *
                      * The flag stops the dispatch callbacks from running
                      * that logic a second time on a spurious extra
                      * readable or writable dispatch. The logic of
                      * _async_handle_redirect is not idempotent. See the
                      * comment of that check for why such a dispatch can
                      * happen.
                      *
                      * _async_ctx_finish also reads this flag, together
                      * with reused, any_bytes_read and timed_out. See the
                      * comment at the top of the "ASYNC CONNECTION STATE
                      * MACHINE" section. At every point that ends a
                      * connection, that helper decides between three
                      * actions: fulfil the future of the chain, retry, or
                      * do neither. It does neither when the code path
                      * that called _async_ctx_finish already handled it.
                      *
                      * The field is _Atomic, because the own
                      * dispatch_lock of ccol_event_loop does not cover it.
                      * The reused-connection path of _async_submit_hop
                      * also writes it directly. That path is ordinary
                      * application code. It runs on whatever thread
                      * called chttpclient_do_async, and not in a dispatch
                      * callback. The dispatch_lock of ccol_event_loop
                      * therefore does not cover it at all. A concurrent
                      * dispatch for this same registered ctx can be in
                      * flight at that same moment.
                      *
                      * A plain bool here is a real data race, and
                      * ThreadSanitizer reports it. The
                      * async_idle_pool.dead_connection_detected_and_retried
                      * scenario reaches it. See the field comment of
                      * pending_app_teardown. That comment explains why a
                      * write of this field alone must never come with a
                      * direct _async_ctx_teardown call from an
                      * application thread. */
  _Atomic bool
      pending_app_teardown; /* Two application-thread failure paths set
                             * this. They are the paths in
                             * _async_submit_hop and
                             * _async_submit_hop_fail where a reused
                             * connection cannot be activated again. They
                             * always set it together with hop_completed,
                             * and always before the
                             * shutdown(ctx->fd, ...) call.
                             *
                             * Those two paths must NEVER call
                             * _async_ctx_teardown themselves. The
                             * ccol_event_loop registration of the ctx is
                             * still fully live at that point. The library
                             * just popped that ctx from the idle pool for
                             * reuse. A reactor dispatch callback
                             * can therefore be in flight, or about to
                             * run, on another thread at that same moment.
                             * The OS can also preempt that thread for an
                             * unbounded time. That gap sits between the
                             * liveness check of ccol_event_loop and the
                             * first touch of ctx.
                             *
                             * An application thread that frees ctx while
                             * that can still happen causes a real
                             * use-after-free. AddressSanitizer reports
                             * it. An atomic refcount for each ctx, pinned
                             * by every dispatch callback, does not close
                             * the hole either. The very first read of
                             * that refcount by the pin can itself race a
                             * concurrent free in the same way.
                             *
                             * One approach closes this by construction:
                             * no application thread ever calls the real
                             * destructive teardown for a registered ctx.
                             * These two failure sites therefore mark ctx
                             * as terminal. They then shut its fd down,
                             * which forces a genuine EPOLLIN or EPOLLERR
                             * dispatch on this fd, which is still
                             * registered for read. They return without
                             * another touch of ctx.
                             *
                             * A dispatch callback then finds
                             * hop_completed already true. See
                             * _async_ctx_handle_if_abandoned, which all
                             * five such checks call across
                             * _async_on_readable_impl,
                             * _async_on_writable_impl and
                             * _async_on_error_impl. That helper reads
                             * this flag to separate two cases. A false
                             * value means that an earlier dispatch call
                             * already handled everything, so there is
                             * nothing left to do. A true value means that
                             * an application thread deferred the real
                             * teardown to whichever dispatch notices it.
                             * The helper then runs _async_ctx_teardown
                             * for real. That is safe from a dispatch
                             * context. ccol_event_loop serialises it
                             * against every other dispatch for this exact
                             * registration. It does that with its own
                             * entry->dispatch_lock, and with its
                             * entry->refcount invariant of one job in
                             * flight for each entry.
                             *
                             * The library writes this field once for each
                             * ctx, and hop_completed is different. A ctx
                             * that ever has this flag true always ends in
                             * termination. The library never pools or
                             * reuses it again. There is therefore no
                             * later point that needs a reset back to
                             * false, and _async_idle_pool_offer does
                             * reset hop_completed. */
  ctls_conn_t *tls;         /* This is NULL until the connect succeeds and
                             * the handshake starts. It is NULL for plain
                             * HTTP. No separate lock guards it. See the
                             * comment at the top of this section for why
                             * the per-registration dispatch_lock of
                             * ccol_event_loop makes one unnecessary. */

  ccol_mutex_t idle_lock; /* This guards the pair of state and chain across the
                           * transition between idle and active.
                           *
                           * _async_idle_pool_take pops a connection out of
                           * cli->idle_pools_async. That connection stays fully
                           * attached to the reactor the whole time, because
                           * there is no way to pause the poll of a single fd.
                           * The removal from the bookkeeping of the POOL does
                           * not stop the reactor. The reactor can still
                           * dispatch on_readable, on_writable or on_error for
                           * that fd on another thread. It does so the moment
                           * the peer sends something, or the connection dies.
                           * Meanwhile _async_submit_hop can still be in the
                           * middle of its work. That work moves ctx->state away
                           * from CHTTP_ASYNC_IDLE, and ctx->chain away from
                           * NULL, for the new owner.
                           *
                           * Without this lock, the IDLE-state guard of a
                           * dispatch can read a stale ctx->state. That state
                           * belongs to a ctx that the library already popped
                           * but did not reconfigure yet. The dispatch then
                           * falls through the guard and dereferences ctx->chain
                           * while it is still NULL.
                           *
                           * The library holds this lock only for a very short
                           * time. It holds it around the few statements that
                           * flip state and chain in either direction. It never
                           * holds it across I/O.
                           *
                           * The deadline sweep deliberately does NOT use this
                           * lock. See the "ASYNC DEADLINE SWEEP" section. That
                           * section gives the lock-ordering hazard that such
                           * use creates. It also gives the design around
                           * shutdown(fd) that avoids the hazard.
                           *
                           * state, chain, fd and timed_out are _Atomic so that
                           * the sweep can read them lock-free. See the comments
                           * of those fields. They explain why single atomic
                           * reads are enough for the sweep, and why it does not
                           * need the stronger compound guarantee of this lock.
                           * Every OTHER consumer of state and chain still needs
                           * that full compound protection, and still gets it.
                           * Those consumers are the dispatch callbacks, which
                           * reach these fields through _async_dispatch_kind. */

  chttp_deadline_t connect_deadline; /* This has meaning only while state is
                                      * CHTTP_ASYNC_CONNECTING or
                                      * CHTTP_ASYNC_TLS_HANDSHAKING. The
                                      * thread that is about to submit this
                                      * hop attempt sets it again at the
                                      * start of every hop that really
                                      * connects. It never sets it for a
                                      * reused connection, because such a
                                      * connection skips both of those
                                      * states.
                                      *
                                      * This field is deliberately NOT
                                      * _Atomic. The library writes it
                                      * exactly once, strictly before the
                                      * _client_deadline_register call for
                                      * this attempt. The lock and unlock
                                      * inside that call form a release and
                                      * acquire pair. That pair already
                                      * guarantees a fully initialized
                                      * value for the sweep, and the sweep
                                      * only ever sees a ctx that it found
                                      * through the registry. No separate
                                      * synchronization is needed. See the
                                      * "ASYNC DEADLINE SWEEP" section. */
  ccol_mutex_t deadline_lock;        /* This guards overall_deadline below, and
                                      * continue_deadline. It also makes the
                                      * verdict of the deadline
                                      * sweep (its snapshot of state and chain and
                                      * its write of timed_out) one step against
                                      * the transitions between idle and active in
                                      * _async_idle_pool_offer and
                                      * _async_idle_pool_take, which make those
                                      * writes under it too. It
                                      * is a small dedicated leaf lock. The library
                                      * never holds it while it tries to take
                                      * idle_lock or the mutex of a deadline stripe.
                                      * It therefore adds no new lock-ordering cycle
                                      * with either of them.
                                      *
                                      * Both the writer and the reader take it
                                      * briefly. The writers are
                                      * _async_idle_pool_take, _async_submit_hop and
                                      * _async_retry_hop. Each of them can already
                                      * hold idle_lock at the point where it writes
                                      * overall_deadline. The reader is the deadline
                                      * sweep, which already holds the mutex of the
                                      * own stripe of this ctx for the walk of that
                                      * stripe.
                                      *
                                      * A real lock is necessary for THIS field. The
                                      * _Atomic type of ctx->chain works as a
                                      * publication barrier for connect_deadline
                                      * above, and that pattern is not enough here.
                                      * The library publishes connect_deadline
                                      * exactly once, through the mutex of
                                      * _client_deadline_register. It can REWRITE
                                      * overall_deadline on every reuse cycle of the
                                      * idle pool, with no fresh
                                      * _client_deadline_register call at all. That
                                      * is a genuine data race, and ThreadSanitizer
                                      * reports it. */
  chttp_deadline_t overall_deadline; /* A ctx-local COPY of
                                      * chain->overall_deadline. That value
                                      * never changes for the whole lifetime
                                      * of the chain, and
                                      * _async_chain_create computes it
                                      * once. The library updates this copy
                                      * under deadline_lock at every place
                                      * that assigns a live chain to
                                      * ctx->chain. Those places are the
                                      * fresh and reused paths of
                                      * _async_submit_hop,
                                      * _async_idle_pool_take and
                                      * _async_retry_hop.
                                      *
                                      * This copy exists so that the
                                      * deadline sweep never dereferences
                                      * ctx->chain at all. A read of
                                      * chain->overall_deadline through
                                      * node->chain is a use-after-free. The
                                      * sweep reads that pointer with an
                                      * atomic load and dereferences it
                                      * later. Nothing stops the last
                                      * reference of the chain from going
                                      * away in between. The chain is free
                                      * by then.
                                      *
                                      * The sweep therefore only compares
                                      * node->chain against NULL. That is a
                                      * safe pointer read with no
                                      * dereference, and it needs no lock.
                                      * The comparison tells the sweep
                                      * whether this field has meaning right
                                      * now. The sweep reads this field
                                      * itself under deadline_lock. */
  _Atomic bool timed_out;   /* The deadline sweep sets this lock-free, right
                             * before it shuts the fd of this connection
                             * down. See the "ASYNC DEADLINE SWEEP" section.
                             * The ordinary dispatch-driven teardown,
                             * _async_ctx_finish, then reports
                             * ccol_timed_out. It also skips the ordinary
                             * retry of a dead connection. A retry past a
                             * deadline that already expired only extends
                             * the overrun and gives no benefit. This field
                             * is _Atomic for the same reason as state,
                             * chain and fd above. */
  bool deadline_registered; /* This is true after the library links this ctx
                             * into the list of its stripe at least once.
                             * That list is
                             * client_deadline_bundle.stripes[
                             * _deadline_stripe_index_for_ctx(ctx)]. The
                             * registration is idempotent. After the library
                             * makes it, it lasts for the whole lifetime of
                             * the ctx, every idle-pool cycle included. See
                             * the comment of _client_deadline_register. */
  struct chttp_async_ctx_s *deadline_prev,
      *deadline_next; /* These two give intrusive membership in a
                       * doubly-linked list. That list is the stripe of
                       * this ctx inside the process-wide deadline registry,
                       * whose locks are sharded. The mutex of that one
                       * stripe guards these fields. That mutex is a
                       * different lock from idle_lock above. See the
                       * comment of client_deadline_bundle for the
                       * lock-ordering contract between the two. That
                       * contract holds for the mutex of every single
                       * stripe. */

  chttp1_parser_t parser;
  chttp_parse_ctx_t pctx;
  chttp_bodybuf_t bb;

  /* These fields drive "Expect: 100-continue" for Tier 2 and Tier 3. They
   * match the design of chttp_do_internal and _chttp_send_and_read in
   * Tier 1. See the doc comment of that function for the contract, which
   * has three outcomes. An interim 100 means that the library sends the
   * body. A direct answer from the server IS the final response, and the
   * library never sends the body. An expired wait means that the library
   * sends the body anyway. This module uses event-driven dispatch in place
   * of the blocking wait loop of Tier 1. */
  bool want_100_continue; /* _async_submit_hop and _async_retry_hop compute
                           * this once for each hop attempt. It matches the
                           * identical use_100_continue local of Tier 1
                           * exactly. The condition is
                           * req->expect_continue && !has_explicit_expect
                           * && body_carrying_method && body.data &&
                           * body.len > 0.
                           *
                           * Only two kinds of code touch this field. The
                           * first is the callbacks of ctx->reg, which the
                           * dispatch_lock serialises. The second is the
                           * hop-setup code, which runs strictly before the
                           * library registers this ctx. The field therefore
                           * needs no atomic type and no lock. */
  size_t header_len;      /* This has meaning only when want_100_continue is
                           * true. It is the boundary inside `wire` between
                           * the header block and the body. Its value is
                           * wire_len - body_len, which is exactly the
                           * header_len local of Tier 1. The first write
                           * phase stops here, and not at wire_len. See the
                           * want_100_continue branch of
                           * _async_plain_try_write and of
                           * _async_tls_try_write. */
  _Atomic bool
      continue_decided; /* This guards a race between a read that sees the
                         * 100 and a write that sees the timeout. It works
                         * in the same way as hop_completed, which guards
                         * the matching race about which dispatch finishes
                         * a hop elsewhere in this file.
                         *
                         * At most one of two branches may decide to write
                         * the body. The first is the "100 Continue seen"
                         * branch of _async_on_readable_impl. The second is
                         * the "continue_deadline expired" branch of
                         * _async_on_writable_impl. The two can never run
                         * concurrently, because the per-entry
                         * dispatch_lock of ccol_event_loop guarantees it,
                         * and because both directions of one fd share one
                         * entry.
                         *
                         * Whichever callback runs first sets this field
                         * with atomic_exchange. It goes on only when it
                         * sees the change from false. The other one, if it
                         * also runs, sees a true value and does nothing.
                         *
                         * The library resets this to false at the start of
                         * every fresh want_100_continue hop attempt,
                         * exactly like interim_responses_seen. */
  _Atomic bool
      continue_msg_started; /* This is true while the parser holds part of a
                             * message that arrived during the wait for a
                             * "100 Continue". continue_deadline bounds only
                             * the wait for the first byte of a message,
                             * exactly like the window of
                             * _chttp_send_and_read in Tier 1. A message that
                             * started inside the window is read to its end
                             * under overall_deadline alone: the expiry of
                             * the window neither sends the body nor touches
                             * the parser while this is true.
                             *
                             * Only the dispatches of this ctx write it, and
                             * the per-entry dispatch_lock of ccol_event_loop
                             * orders them. It is atomic because the deadline
                             * sweep also reads it, to skip a flip to the
                             * write direction that the dispatch would only
                             * undo. The library clears it wherever it clears
                             * continue_decided. */
  chttp_deadline_t
      continue_deadline; /* Two values bound this deadline. The first is
                          * CHTTP_100_CONTINUE_WAIT_US. The second is what
                          * remains of ctx->overall_deadline, and
                          * _deadline_earlier combines them. The library
                          * computes it once, when this hop attempt moves
                          * into CHTTP_ASYNC_AWAITING_CONTINUE.
                          *
                          * Only the deadline sweep reads it, from its own
                          * thread. A reused ctx rewrites it for every hop
                          * that waits for a continue. The writer stores it
                          * and the sweep copies it under deadline_lock,
                          * in the same critical section as its snapshot of
                          * state, so the copy and the state that it is
                          * judged by always belong to one hop. */
  bool retry_unsafe;     /* This matches the identical *retry_unsafe_out
                          * parameter of _chttp_send_and_read in Tier 1. The
                          * library sets it true the moment that this hop
                          * sends the body onto the wire. It sets it only
                          * AFTER the server sends an explicit
                          * "100 Continue" on THIS connection.
                          *
                          * From that point on, a read failure with zero
                          * bytes of a final response no longer means that
                          * nothing went out. A retry is therefore no longer
                          * free. See the doc comment of that function for
                          * the full reasoning about a double submission of
                          * a request that is not idempotent.
                          *
                          * _async_ctx_finish checks this field next to
                          * reused and any_bytes_read. The library resets it
                          * to false at the start of every fresh hop
                          * attempt. */
  char *continue_carry;  /* This is non-NULL only in one case. A
                          * "100 Continue" line arrives in the same read()
                          * as the start of the next message, which is
                          * usually the final response. A fast server
                          * writes both before it even reads the body.
                          * RFC 7231 SS5.1.1 permits that. Tier 1 feeds the
                          * same bytes to its early-response parser in
                          * _chttp_send_and_read.
                          *
                          * This field is an owned copy of those trailing
                          * bytes. _async_awaiting_continue_on_data stores
                          * it and pauses the send of the body at once;
                          * _async_upload_pause then feeds it to
                          * _async_process_reading_data before a byte of
                          * the body goes out, frees it and sets this field
                          * to NULL. The read path decides from those bytes
                          * whether the body is sent at all. The teardown of
                          * the ctx frees the copy of a ctx that the library
                          * abandons in between. */
  size_t continue_carry_len;
} chttp_async_ctx_t;

/* ========================================================================== */
/*                    ASYNC DEADLINE SWEEP (TIER 2)                           */
/* ========================================================================== */

/*
 * This enforces connect_timeout_us and request_timeout_us for Tier 2 and
 * Tier 3. It treats both as true absolute wall-clock deadlines. This
 * matches the semantics of _deadline_make and _deadline_remaining_ms in
 * Tier 1 exactly. It also catches a connection that never goes fully idle
 * and still blows its deadline. A slow trickle of bytes, always just before
 * an activity-reset timeout would fire, is one such connection.
 *
 * ccol_event_loop has no built-in timer or deadline mechanism. This is
 * therefore a small thread that sweeps periodically. It is fully separate
 * from the threads of the reactor and from the ctpool for DNS and
 * connect. The library starts and stops it next to them. See
 * _client_deadline_sweep_start and _client_deadline_sweep_stop_and_join,
 * which _client_engine_acquire and _client_engine_reaper_fn above call.
 *
 * The registry of live async ctx nodes is sharded. It has
 * CHTTP_DEADLINE_SWEEP_STRIPES independent stripes in
 * client_deadline_bundle.stripes[]. Each stripe has its own mutex and its
 * own head of an intrusive doubly-linked list. There is no single global
 * mutex over a single global list.
 *
 * A register or unregister call locks only the one stripe that its ctx
 * hashes into. _deadline_stripe_index_for_ctx computes that index.
 * Concurrent registrations for different ctx nodes, across many in-flight
 * requests, therefore do not serialise against each other. The walk of the
 * sweep below covers one stripe at a time. It also blocks only a register
 * or unregister that lands in that one stripe. It never blocks every one in
 * the process.
 *
 * The register and unregister pair of one request always lands on the very
 * same stripe, by construction. The hash is a pure and stateless function
 * of the ctx pointer. Both calls compute it in the same way. No extra
 * bookkeeping field on the ctx has to remember which stripe it landed in.
 *
 * This matches the lock-striped fd registry of ccol_event_loop in
 * cthreadcomm.c, which is _stripe_index_for_fd. The reason is the same: a
 * real hash key over a ctx needs the same stripe on every lookup.
 * ccol_event_loop assigns a stripe to a queue selectable round-robin
 * instead. That is safe only because the library never looks a queue
 * selectable up a second time.
 *
 * client_deadline_bundle.mutex and client_deadline_bundle.cond_var are
 * separate. They guard only the start, stop and sleep-wake lifecycle of the
 * sweep thread itself, which is client_deadline_bundle.stop and
 * client_deadline_bundle.thread. They play no part in the mutual exclusion
 * of the registry.
 *
 * The mutex of a stripe serialises two things for that one stripe. The
 * first is its intrusive doubly-linked list, which is ctx->deadline_prev
 * and ctx->deadline_next of every ctx that hashes into it. The second is
 * mutual exclusion between two parties. The first party is the sweep, which
 * reads the fields of a registered ctx. The second is any other thread that
 * frees that same ctx at the same time. That second point is the reason why
 * the library unregisters a ctx
 * only from inside _async_ctx_free. That function is the one reliable place
 * where it really frees the memory of a ctx.
 *
 * _client_deadline_unregister cannot finish while the sweep still walks the
 * own stripe of that ctx with that mutex held. _async_ctx_free can
 * therefore not go on and free the ctx. A walk of a DIFFERENT stripe in the
 * same sweep tick is untouched and runs concurrently.
 *
 * Registration through _client_deadline_register is idempotent. After the
 * library makes it, it lasts for the whole lifetime of a ctx, every
 * idle-pool cycle included. An idle pooled ctx sits in its stripe and does
 * nothing. The checks of the sweep skip it while it is idle. Those
 * checks are state == CONNECTING or TLS_HANDSHAKING, and chain != NULL.
 * This is simpler than an unregister on every idle-pool offer and a fresh
 * register on every reuse, and it is just as correct.
 *
 * Lock ordering: a caller CAN legitimately call
 * _client_deadline_register while it holds the idle_lock of some ctx. The
 * reused-connection branch of _async_submit_hop for a failed
 * ccol_event_loop_modify is one such caller. It holds the idle_lock of
 * the abandoned ctx across its call to _async_retry_hop. That call
 * registers the brand-new retry ctx before the unlock. The library
 * therefore does NOT always take the mutex of a stripe outside every
 * ctx->idle_lock.
 *
 * This stays free of deadlock only because the reverse order never happens
 * anywhere. Neither _client_deadline_register nor
 * _client_deadline_unregister ever touches the idle_lock of any ctx while
 * it holds the mutex of a stripe. Both only work on the intrusive list
 * of that one stripe. The sweep, _client_deadline_sweep_once, is the only
 * other consumer of a stripe mutex, and it never takes idle_lock at all. It
 * reads every field that it needs with plain _Atomic loads. See the comment
 * of that function.
 *
 * THAT one-directional invariant is what matters here: no code may hold the
 * mutex of a stripe while it takes any ctx->idle_lock. Do not "fix" the
 * sweep, the register function or the unregister function to also take
 * idle_lock before you derive this reasoning again. Such a change turns the
 * idle_lock-then-stripe-mutex order that is already in real use above into
 * a genuine AB-BA lock-order inversion.
 *
 * No code in this file ever holds two stripe mutexes at once either. Each
 * operation touches exactly one ctx, and therefore exactly one stripe. The
 * sweep locks one stripe, walks it and unlocks it fully before it moves to
 * the next. The sharding therefore adds no new lock-ordering surface
 * between stripes beyond the rule about a stripe mutex and idle_lock above.
 *
 * How the sweep neuters an expired connection: a raw fd has no protection
 * of its own. It can be closed, and an unrelated connection can then reuse
 * the number. That can happen between the moment that this sweep captures
 * the fd and the moment that it acts on it. An fd-based operation on a
 * captured int, after an unlock, can therefore act on a completely
 * unrelated connection. The library reaches that state when a fresh
 * connect() reuses the closed fd number.
 *
 * This sweep avoids that hazard by construction, and it needs no generation
 * check of its own. Every teardown path calls _client_deadline_unregister
 * strictly BEFORE it closes ctx->fd. Those paths are _async_ctx_finish and
 * _async_idle_ctx_finish, which the code defines after the idle pool
 * section below. _client_deadline_unregister needs the stripe mutex of
 * that ctx. A ctx that is still linked into its stripe therefore always
 * still has its fd open.
 *
 * This sweep therefore calls shutdown(fd, SHUT_RDWR) directly, while it
 * still holds the stripe mutex of that ctx. It needs no separate phase
 * that collects first and acts after an unlock. shutdown() is a plain
 * kernel-level operation with no synchronous application callback. There is
 * therefore no risk of reentrancy in a call to it here.
 *
 * shutdown() neuters both directions of the connection. The next
 * epoll_wait reports the fd as readable with an error and as writable with
 * an error. This sweep never touches the memory of the ctx, its
 * idle_lock, or its ccol_event_loop registration. The ordinary dispatch
 * path takes over from there, after the reactor sees the fd. Those
 * callbacks are _async_on_readable, _async_on_writable and
 * _async_on_error. They handle it exactly like any other natural failure of
 * a connection.
 */
#define CHTTP_DEADLINE_SWEEP_STRIPES 16

typedef struct {
  ccol_mutex_t mutex;
  chttp_async_ctx_t *head;
} chttp_deadline_stripe_t;

static struct {
  ccol_mutex_t mutex;
  ccol_cond_var_t cond_var;
  /* The mutex and the condition variable of client_deadline_bundle guard
   * only the start, stop and sleep-wake lifecycle of the sweep thread
   * itself. Those two fields are stop and thread. These two primitives
   * never guard the ctx registry. All of the locking of that registry is
   * per stripe. See stripes[] below.
   *
   * The library initializes both lazily at run time, under a
   * ccol_call_once guard. See _client_deadline_init_globals. That guard is
   * independent of the once-guard of the async engine pair above. A
   * static constant initializer is not enough here. The equivalent
   * primitive of a backend that is not pthreads can need real setup work,
   * and a compile-time constant cannot do that work.
   *
   * Every function below that touches any of these globals calls
   * ccol_call_once(client_deadline_bundle.once, ...) as its first
   * statement. */
  ccol_once_flag_t once;
  chttp_deadline_stripe_t stripes[CHTTP_DEADLINE_SWEEP_STRIPES];
  ccol_thread_id_t thread;
  bool stop;
} client_deadline_bundle = {0};

static size_t _deadline_stripe_index_for_ctx(const chttp_async_ctx_t *ctx) {
  uintptr_t p = (uintptr_t)ctx;
  /* Fold the full width of the pointer down to 32 bits before the
   * multiplicative hash. The fold is an exclusive or of the high half and
   * the low half. A plain truncation to the low 32 bits is worse. The low
   * bits of a heap pointer often carry an alignment constraint, such as
   * always a multiple of 16. A truncation therefore throws away the
   * entropy that the high bits carry. This uses the same Knuth
   * multiplicative constant as _stripe_index_for_fd in cthreadcomm.c.
   *
   * The `p >> 32` fold has meaning only on a platform where uintptr_t is
   * wider than 32 bits, and it only compiles there. On a 32-bit target
   * such as i386, uintptr_t IS the 32-bit value. There is no high half
   * left to fold in. `p >> 32` is then a shift by the full width of the
   * type. That is undefined behavior, and under the build flags of this
   * project it is a hard -Werror=shift-count-overflow error.
   *
   * A preprocessor #if guards it, and not a run-time if. This matches the
   * `#if SIZE_MAX == UINT64_MAX` convention of chashmap.c, for the same
   * reason. A run-time check does not stop the compiler from a full type
   * check of the branch that is dead on this platform, and from a
   * rejection of it. */
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
  if (ccol_mutex_init(client_deadline_bundle.mutex) != 0)
    ccol_fatal_err("chttpclient deadline sweep: failed to initialize mutex");
  for (size_t i = 0; i < CHTTP_DEADLINE_SWEEP_STRIPES; i++) {
    if (ccol_mutex_init(client_deadline_bundle.stripes[i].mutex) != 0)
      ccol_fatal_err(
          "chttpclient deadline sweep: failed to initialize stripe mutex");
  }
  /* This uses CLOCK_MONOTONIC to match the wake deadline of
   * _client_deadline_sweep_fn, which comes from
   * clock_gettime(CLOCK_MONOTONIC, ...). The default clock of
   * ccol_cond_var_init is CLOCK_REALTIME, and that makes the deadline
   * comparison meaningless. A timespec from the monotonic clock is always
   * a small value next to boot time. The condition variable then compares
   * it against the wall clock inside itself.
   *
   * ccol_cond_var_timedwait then reports ETIMEDOUT at once on every call.
   * It does not sleep for about 100 ms between sweeps. The sweep thread
   * busy-spins at 100 percent CPU for the whole life of the engine.
   *
   * The library initializes srv->requests_done_cv in chttpserver.c in the
   * same way, for the same reason. See _ccol_cond_var_init_monotonic, the
   * shared helper of this file for this exact pattern, for the full
   * reasoning. */
  if (_ccol_cond_var_init_monotonic(&client_deadline_bundle.cond_var) != 0)
    ccol_fatal_err(
        "chttpclient deadline sweep: failed to initialize condition "
        "variable");
}

#define CHTTP_DEADLINE_SWEEP_INTERVAL_MS 100
/* This is a soft cap on how many expired connections one sweep tick shuts
 * down. It heals itself and is not a hard limit. A connection past this cap
 * on one tick stays registered. The library does not mark it timed_out yet,
 * so a later tick finds it again and makes its shutdown() call. The worst
 * delay is another CHTTP_DEADLINE_SWEEP_INTERVAL_MS for each extra batch.
 * That delay does not matter in practice, for the number of connections
 * that must expire at the same time to pass this cap at all. */
#define CHTTP_DEADLINE_SWEEP_BATCH 256

/*
 * This adds ctx to the deadline registry when the registry does not hold it
 * yet. Call it only when ctx->idle_lock is NOT held. See the note about
 * lock ordering above.
 *
 * It locks only the one stripe that ctx hashes into. It never locks another
 * stripe. It never locks client_deadline_bundle.mutex itself, because that
 * mutex guards only the lifecycle of the sweep thread and not the registry.
 */
static void _client_deadline_register(chttp_async_ctx_t *ctx) {
  ccol_call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  chttp_deadline_stripe_t *stripe =
      &client_deadline_bundle.stripes[_deadline_stripe_index_for_ctx(ctx)];
  ccol_mutex_lock(stripe->mutex);
  if (!ctx->deadline_registered) {
    ctx->deadline_prev = NULL;
    ctx->deadline_next = stripe->head;
    if (stripe->head) stripe->head->deadline_prev = ctx;
    stripe->head = ctx;
    ctx->deadline_registered = true;
  }
  ccol_mutex_unlock(stripe->mutex);
}

/* This removes ctx from the deadline registry when the registry holds it.
 * The library calls it exactly once, as the first statement of
 * _async_ctx_free. See the comment of that function for why that is the one
 * correct place for this call.
 *
 * _deadline_stripe_index_for_ctx is a pure and stateless function of the
 * own address of ctx. This function therefore computes and locks the same
 * stripe that _client_deadline_register used for this same ctx. No separate
 * bookkeeping about which stripe held the ctx is needed. */
static void _client_deadline_unregister(chttp_async_ctx_t *ctx) {
  ccol_call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  chttp_deadline_stripe_t *stripe =
      &client_deadline_bundle.stripes[_deadline_stripe_index_for_ctx(ctx)];
  ccol_mutex_lock(stripe->mutex);
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
  ccol_mutex_unlock(stripe->mutex);
}

/*
 * This is one sweep pass. It walks each of the
 * CHTTP_DEADLINE_SWEEP_STRIPES stripes in turn. It locks only the mutex
 * of that one stripe for the whole walk of that stripe. It never holds more
 * than one stripe lock at a time. It never touches
 * client_deadline_bundle.mutex at all.
 *
 * For each ctx it checks two deadlines. It checks connect_deadline only
 * while the ctx is really in a connect phase. It checks the
 * overall_deadline of the chain whenever the ctx has a live chain.
 *
 * It then shuts the fd of every newly expired connection down. It does this
 * inline, while it still holds the lock of that stripe. See the comment at
 * the top of this section for why that is safe. There is no risk of
 * reentrancy, and no race over a reused fd.
 *
 * n_shutdown adds up across the whole tick, over every stripe.
 * CHTTP_DEADLINE_SWEEP_BATCH therefore bounds the total work of one sweep
 * tick across every stripe, and not the work for one stripe.
 *
 * This code reads state, chain, fd and timed_out as plain loads. Each field
 * is _Atomic, so each single load is free of a race. It deliberately does
 * NOT read them under the idle_lock of the ctx. See the note about lock
 * ordering above. Some call paths legitimately hold the idle_lock of a ctx
 * and also need to lock a deadline stripe. The failure branch of
 * _async_submit_hop for a write on a reused connection is one such path: it
 * calls _async_retry_hop, which calls _client_deadline_register. This sweep
 * must therefore never take idle_lock while it holds the mutex of a stripe.
 * That would be a classic AB-BA lock-order inversion for that stripe.
 *
 * This is safe without the stronger COMPOUND guarantee of idle_lock, which
 * covers state and chain together. The reason is what this function does
 * with a snapshot that can be torn. idle_lock really protects two points.
 * The first is the transition from idle to active in
 * _async_idle_pool_take. The second is the transition from active to idle
 * in _async_idle_pool_offer.
 *
 * At both points, every torn combination of state and chain that this code
 * can reach falls into one of two cases. In the first case it skips both
 * deadline checks, because chain is NULL or state is not a connect state.
 * In the second case it reads a chain that is alive for certain, although
 * it can be a moment away from release. The library drops the chain
 * reference strictly after it updates the pair of state and chain.
 *
 * The worst outcome of a torn read here is therefore one connection that
 * the sweep shuts down a few instructions too early or too late. The
 * library would otherwise have pooled that connection cleanly, or moved it
 * to the next hop. That is a lost optimisation. It is never a wrong abort
 * of an unrelated request that is still in flight. Every OTHER consumer of
 * state and chain needs the full guarantee of idle_lock, and takes it.
 * Those consumers are the dispatch
 * callbacks, which reach the fields through _async_dispatch_kind.
 *
 * The library marks ctx->timed_out before it shuts the fd of that ctx down.
 * This does two things. It tells the ordinary teardown path that this is a
 * timeout and not an ordinary connection failure. That path then reports
 * ccol_timed_out and skips the usual retry of a dead connection. A retry
 * past a deadline that already expired only extends the overrun. The mark
 * also makes repeated sweep ticks idempotent against a ctx that is still
 * mid-teardown from the shutdown() call of an earlier tick.
 */
#ifdef RUNNING_UNIT_TESTS
/* A test arms g_sweep_race_armed_for_tests to make the next sweep tick judge
 * the first ctx that it finds in CHTTP_ASYNC_READING as expired, as though
 * the clock of the sweep had passed the deadline of that hop while the
 * dispatch of the same hop still finished inside it. The sweep then parks
 * twice for that ctx: in phase 1 between its verdict and its write of
 * timed_out, and in phase 2 between the end of its critical section and
 * its shutdown() of the fd. g_sweep_race_parked_for_tests shows the phase
 * that the sweep waits in, and the test releases a phase by writing its
 * number into g_sweep_race_release_for_tests. Every park is bounded, so a
 * test that fails before its release never hangs the sweep for good. */
static _Atomic bool g_sweep_race_armed_for_tests = false;
static void *_Atomic g_sweep_race_forced_ctx_for_tests = NULL;
static _Atomic int g_sweep_race_parked_for_tests = 0;
static _Atomic int g_sweep_race_release_for_tests = 0;
/* The offer path counts its entry before, and its exit after, the critical
 * section that makes a ctx idle. */
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

/* A test arms g_continue_sweep_armed_for_tests to make the next sweep tick
 * that snapshots a ctx in CHTTP_ASYNC_AWAITING_CONTINUE park right after its
 * critical section under deadline_lock, before it judges the continue
 * window. The park ends when the test releases it, and after 10 s at most.
 * g_continue_deadline_stores_for_tests counts the stores of
 * continue_deadline. It is relaxed on both sides on purpose: a test that
 * waits on it learns that a store happened without gaining any ordering
 * with the thread that made it, so ThreadSanitizer still judges the
 * accesses of the sweep against that store on their own merits. */
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
#endif /* RUNNING_UNIT_TESTS */

static void _async_race_tick(chttp_connect_race_t *r, bool expired);

static void _client_deadline_sweep_once(void) {
  ccol_call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  size_t n_shutdown = 0;

  for (size_t s = 0; s < CHTTP_DEADLINE_SWEEP_STRIPES &&
                     n_shutdown < CHTTP_DEADLINE_SWEEP_BATCH;
       s++) {
    chttp_deadline_stripe_t *stripe = &client_deadline_bundle.stripes[s];
    ccol_mutex_lock(stripe->mutex);
    chttp_async_ctx_t *node = stripe->head;
    while (node && n_shutdown < CHTTP_DEADLINE_SWEEP_BATCH) {
      chttp_async_ctx_t *next = node->deadline_next;

      int fd = node->fd;
      bool expired = false;
      /* The snapshot of state and chain, the judgement, and the write of
       * timed_out form one critical section under node->deadline_lock.
       * _async_idle_pool_offer clears chain, sets state to IDLE and clears
       * timed_out under that same lock. A verdict on the hop that just
       * finished can therefore never land on the ctx after the offer made it
       * idle: either the verdict comes first and the offer clears it, or the
       * offer comes first and the snapshot here shows no chain. A mark that
       * landed after the offer would poison the next request that reuses the
       * connection, which then reports ccol_timed_out in place of the retry
       * of a dead connection. See the field comment of node->deadline_lock
       * for why this leaf lock is also what makes the read of
       * overall_deadline safe. */
      ccol_mutex_lock(node->deadline_lock);
      chttp_async_state_t state = node->state;
      chttp_deadline_t continue_dl = node->continue_deadline;
      /* This code only compares the pointer against NULL. It never
       * dereferences it. See the field comment of node->overall_deadline.
       * A dereference of a bare chain pointer that this code reads is a
       * use-after-free, and ThreadSanitizer reports it. */
      bool has_chain = (node->chain != NULL);
      if (!node->timed_out) {
        int ms;
        if ((state == CHTTP_ASYNC_CONNECTING ||
             state == CHTTP_ASYNC_TLS_HANDSHAKING) &&
            !_deadline_remaining_ms(&node->connect_deadline, &ms)) {
          expired = true;
        }
        if (!expired && has_chain &&
            !_deadline_remaining_ms(&node->overall_deadline, &ms)) {
          expired = true;
        }
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
      ccol_mutex_unlock(node->deadline_lock);
#ifdef RUNNING_UNIT_TESTS
      if (state == CHTTP_ASYNC_AWAITING_CONTINUE)
        _continue_sweep_park_for_tests();
#endif

      /* A ctx whose Happy Eyeballs race runs has no fd of its own yet. The
       * race holds the fds of its attempts, and a tick ends them or starts
       * the next one. See the "ASYNC HAPPY EYEBALLS" section. */
      chttp_connect_race_t *race =
          (state == CHTTP_ASYNC_CONNECTING) ? atomic_load(&node->race) : NULL;
      if (expired) {
#ifdef RUNNING_UNIT_TESTS
        if (atomic_load(&g_sweep_race_forced_ctx_for_tests) == node)
          _sweep_race_park_for_tests(2);
#endif
        if (fd >= 0) shutdown(fd, SHUT_RDWR);
        if (race) _async_race_tick(race, true);
        n_shutdown++;
      } else if (race) {
        _async_race_tick(race, false);
      } else if (fd >= 0 && atomic_load(&node->flip_watch) != CHTTP_FLIP_NONE) {
        /* The send of the request waits on one direction while the other
         * one can matter. See _async_upload_pause. The sweep turns the
         * registration to the other direction once that one is ready; the
         * dispatch that follows drives the exchange from there. A flip that
         * lands after another dispatch already moved the ctx on costs one
         * dispatch, exactly as the continue flip below does. */
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
        /* This is a soft deadline for one phase of one hop. It is separate
         * from connect_deadline and overall_deadline above. An expired
         * CHTTP_100_CONTINUE_WAIT_US does NOT mean that the request itself
         * timed out. overall_deadline stays the real backstop for that,
         * and the check above always runs it, because has_chain is true
         * here too. This branch leaves that deadline completely untouched.
         *
         * The expiry means that the wait for a "100 Continue" is over, and
         * that the library should send the body anyway. See the doc
         * comment of CHTTP_ASYNC_AWAITING_CONTINUE for the full contract
         * with three outcomes.
         *
         * The library delivers that decision by a flip of this
         * registration to the write direction. That flip dispatches to the
         * own CHTTP_ASYNC_AWAITING_CONTINUE branch of
         * _async_on_writable_impl. That branch acts on it under the own
         * dispatch_lock of that dispatch. This sweep thread therefore never
         * changes the fields of the ctx directly.
         *
         * This code only peeks at continue_decided. It does not claim it.
         * The real claim happens inside the dispatch that this flip
         * triggers. That matches every other exit from
         * _async_awaiting_continue_on_data.
         *
         * A true value here means that an in-flight on_readable dispatch
         * for this same ctx already claimed a genuine "100 Continue" or a
         * final response. A skip of the modify call in that case avoids a
         * wasted flip to the write direction. Such a flip is harmless, and
         * it races the already correct transition of that dispatch.
         *
         * This code reads both continue_decided and the state again right
         * before the modify. The window in which a concurrent transition
         * can still slip past is therefore a few instructions, and not the
         * whole deadline computation above.
         *
         * A flip to the write direction can still land on a ctx that
         * already left CHTTP_ASYNC_AWAITING_CONTINUE, or on a later hop of
         * a pooled ctx that waits in a window of its own, which continue_dl
         * does not describe. That costs exactly one wasted dispatch: the
         * dispatch judges the window of its own hop and sends no body while
         * that window is open. The fall-through of
         * _async_on_writable_impl restores the read direction for every
         * state with nothing left to flush. It does this under the same
         * ctx->idle_lock that the reactivation of a pooled ctx holds. It is
         * never a permanent hang, never a crash, and it never affects any
         * OTHER request. */
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
    ccol_mutex_unlock(stripe->mutex);
  }

  if (n_shutdown > 0) {
    _CLIENT_ENGINE_LOG_INFO("deadline sweep shut down %zu connection(s)",
                            n_shutdown);
  }
}

static void *_client_deadline_sweep_fn(void *arg) {
  (void)arg;
  ccol_call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  ccol_mutex_lock(client_deadline_bundle.mutex);
  while (!client_deadline_bundle.stop) {
    struct timespec wake;
    clock_gettime(CLOCK_MONOTONIC, &wake);
    wake.tv_nsec += CHTTP_DEADLINE_SWEEP_INTERVAL_MS * 1000000L;
    if (wake.tv_nsec >= 1000000000L) {
      wake.tv_nsec -= 1000000000L;
      wake.tv_sec += 1;
    }
    ccol_cond_var_timedwait(client_deadline_bundle.cond_var,
                            client_deadline_bundle.mutex, wake);
    if (client_deadline_bundle.stop) break;
    ccol_mutex_unlock(client_deadline_bundle.mutex);
    _client_deadline_sweep_once();
    _client_engine_retry_abandoned_reap();
    ccol_mutex_lock(client_deadline_bundle.mutex);
  }
  ccol_mutex_unlock(client_deadline_bundle.mutex);
  return NULL;
}

/* This starts the sweep thread. _client_engine_acquire calls it, next to
 * the code that starts the reactor thread of the engine. See that
 * function for how it rolls back on a failure. */
static ccol_retval_t _client_deadline_sweep_start(void) {
  client_deadline_bundle.stop = false;
  int rc = ccol_thread_create(client_deadline_bundle.thread,
                              _client_deadline_sweep_fn, NULL);
  return (rc == 0) ? ccol_success : ccol_unexpected_failure;
}

/* This signals the sweep thread and then joins it. _client_engine_reaper_fn
 * calls it, next to the join of the reactor thread of the engine. See
 * the comment of that function for why a teardown always runs on a
 * dedicated reaper thread. It never runs inline from a reactor callback. */
static void _client_deadline_sweep_stop_and_join(void) {
  ccol_call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  ccol_mutex_lock(client_deadline_bundle.mutex);
  client_deadline_bundle.stop = true;
  ccol_cond_var_broadcast(client_deadline_bundle.cond_var);
  ccol_mutex_unlock(client_deadline_bundle.mutex);
  ccol_thread_join(client_deadline_bundle.thread);
}

#if CCOL_FORK_SAFETY_REQUIRED
/* ========================================================================== */
/*                    FORK SAFETY (pthread_atfork)                            */
/* ========================================================================== */

/*
 * fork() duplicates only the calling thread. A lock that another thread of
 * this module held at that instant stays locked for good in the child. The
 * engine keeps threads that take cli_engine_bundler.mutex, the lifecycle
 * mutex of the deadline sweep and its stripe mutexes on their own: the
 * reaper, the sweep and the reactor threads. A process that destroyed its
 * last client and forks at once, which is a supported pattern, can
 * therefore fork while the reaper of its engine still runs.
 *
 * This handler waits for such a reaper to finish, so that the child never
 * inherits an engine in the middle of its teardown. It then takes every
 * lock of this module, in the order of every ordinary call: the engine
 * mutex, then the lifecycle mutex of the sweep, then the stripes in index
 * order. No code holds two stripes at once, and no code takes the engine
 * mutex or the lifecycle mutex while it holds a stripe, so the order has no
 * inversion. It runs before the prepare handlers of ccol_event_loop, ctpool
 * and clog; see _client_engine_globals_init.
 *
 * The wait cannot deadlock. A reaper runs only while no reference to the
 * engine is left, and the only code of the application that runs on a
 * thread that the reaper joins is the write_fn of a streaming request, which
 * holds a reference. The wait applies only in the process that built the
 * engine: a process that inherited an engine has no reaper to wait for.
 */
static void _chttpcli_atfork_prepare(void) {
#ifdef RUNNING_UNIT_TESTS
  _ccol_atfork_order_record(ccol_atfork_module_chttpclient);
#endif
  ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  ccol_call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  ccol_mutex_lock(cli_engine_bundler.mutex);
  if (atomic_load(&g_client_engine_pid) == getpid()) {
    while (cli_engine_bundler.stopping)
      ccol_cond_var_wait(cli_engine_bundler.stopped_cv,
                         cli_engine_bundler.mutex);
  }
  ccol_mutex_lock(client_deadline_bundle.mutex);
  for (size_t i = 0; i < CHTTP_DEADLINE_SWEEP_STRIPES; i++)
    ccol_mutex_lock(client_deadline_bundle.stripes[i].mutex);
}

static void _chttpcli_atfork_unlock_all(void) {
  for (size_t i = CHTTP_DEADLINE_SWEEP_STRIPES; i-- > 0;)
    ccol_mutex_unlock(client_deadline_bundle.stripes[i].mutex);
  ccol_mutex_unlock(client_deadline_bundle.mutex);
  ccol_mutex_unlock(cli_engine_bundler.mutex);
}

static void _chttpcli_atfork_parent(void) { _chttpcli_atfork_unlock_all(); }

/* The thread that called fork() holds every lock here, in the child as in
 * the parent, so a plain unlock is correct on both sides. Before it, the
 * child forgets an engine whose threads it did not inherit; see
 * _client_engine_forget_inherited_locked, which the acquire path also runs
 * for a build without this handler. */
static void _chttpcli_atfork_child(void) {
  _client_engine_forget_inherited_locked();
  _chttpcli_atfork_unlock_all();
}
#endif /* CCOL_FORK_SAFETY_REQUIRED */

/* This makes a deep copy of a chmap(char* -> char*) header map. It gives a
 * redirect chain its own copy of the headers of the original request. The
 * caller can free its chttp_request_t the moment that chttpclient_do_async
 * returns. That is long before a later hop must serialize those headers
 * again.
 *
 * The function returns NULL when it runs out of memory. A NULL input is not
 * an error. It only means that there are no headers, and the function
 * returns NULL for that too. The two cases therefore look the same on their
 * own. A caller that must tell them apart checks the source map first. That
 * is what every other call site in this file does, and each of them treats
 * "no headers" as normal. */
static chmap _clone_headers_map(ccol_memmgmt_procs_t *mp, chmap src) {
  char *err = NULL;
  chmap dst = chmap_create_full(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,
                                ccol_string, ccol_string, mp, NULL, NULL, &err);
  if (!dst) return NULL;
  cmap_iterator *it = chashmap_begin_iter(src, NULL);
  for (; it; it = it->_next_fn(it)) {
    if (chmap_insert_elem(dst, it->key_pair, it->val_pair) != ccol_success) {
      ccol_iter_destroy(it);
      __chmap_destroy(dst);
      return NULL;
    }
  }
  return dst;
}

/*
 * This allocates the shared state of a whole redirect chain. See the
 * comment above chttp_async_chain_t.
 *
 * On success it takes ownership of tls_ctx. It frees that context once,
 * with ctls_ctx_release, when it frees the chain. On failure the caller
 * still owns tls_ctx and must free it itself.
 *
 * It does NOT take req_headers, body_data or body_content_type by
 * reference. It makes a deep copy of whatever it needs from them. It never
 * keeps the originals.
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
  if (ccol_mutex_init(chain->lock) != 0) {
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

  /* This chain is live now, and the library is about to hand it to
   * _async_submit_hop. Count it in the async in-flight total of cli.
   * See the comment of that field on struct chttpclient.
   * __chttpclient_destroy can then wait for it. Exactly one subtraction in
   * _async_chain_release matches this, after the refcount of this chain
   * reaches zero. */
  ccol_mutex_lock(cli->async_count_lock);
  cli->async_in_flight_count++;
  ccol_mutex_unlock(cli->async_count_lock);

  return chain;

fail:
  if (chain->req_headers) __chmap_destroy(chain->req_headers);
  _ccol_mem_free(mp, chain->body_data);
  _ccol_mem_free(mp, chain->body_content_type);
  _ccol_mem_free(mp, chain->initial_origin_key);
  ccol_mutex_destroy(chain->lock);
  _ccol_mem_free(mp, chain);
  return NULL;
}

/*
 * A response and an async result outlive the client that produced them:
 * the caller may free either one after chttpclient_destroy. Both therefore
 * carry their own copy of the allocator procs, in the same allocation right
 * after the struct, and _m_procs points at that copy. The procs of the
 * client are gone once the client is destroyed. A NULL mp means the default
 * allocator, and then there is nothing to copy. The free functions read the
 * copy into a local before they free the block that holds it.
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

/* This is a forward declaration. The backstop of _async_chain_release needs
 * it. The code defines the function further below, after the logic that
 * builds a chttp_async_result_t is in scope. */
static void _async_fulfill_chain(chttp_async_chain_t *chain, ccol_retval_t rv,
                                 chttpcli_response *resp);

/* This adds one live-hop reference to chain. It MUST have exactly one
 * _async_chain_release call to match it. The caller already knows that the
 * chain is live. Either the library just created it, and the refcount goes
 * from 0 to 1, or a redirect handoff retains it again while the ctx of the
 * old hop is still alive. */
static void _async_chain_retain(chttp_async_chain_t *chain) {
  ccol_mutex_lock(chain->lock);
  chain->refcount++;
  ccol_mutex_unlock(chain->lock);
}

/*
 * This frees one live-hop reference. A count of zero means that no further
 * hop took over from the last one that the library tore down.
 *
 * That point is the one reliable place for two actions. The first is a
 * backstop that fulfils the future. It does nothing when some hop already
 * fulfilled it, because it goes through the same fulfilled guard that
 * _async_fulfill_chain checks. The second is the free of the chain itself.
 * That free includes the one engine reference that the chain holds for its
 * whole lifetime.
 */
static void _async_chain_release(chttp_async_chain_t *chain) {
  if (!chain) return;
  ccol_mutex_lock(chain->lock);
  int remaining = --chain->refcount;
  ccol_mutex_unlock(chain->lock);
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
  ccol_mutex_destroy(chain->lock);
  _ccol_mem_free(chain->mp, chain);
  _client_engine_release_chain();

  /* This matches the addition in _async_chain_create. See the field comment
   * of the async_in_flight_count of cli. That comment says why this
   * code uses a dedicated leaf lock and not cli->lock. This function runs
   * from inside ccol_event_loop dispatch callbacks as often as from a safe
   * synchronous context. */
  ccol_mutex_lock(cli->async_count_lock);
  if (cli->async_in_flight_count > 0) cli->async_in_flight_count--;
  if (cli->async_in_flight_count == 0)
    ccol_cond_var_broadcast(cli->async_count_drained);
  ccol_mutex_unlock(cli->async_count_lock);
}

static chttp_async_ctx_t *_async_ctx_create(ccol_memmgmt_procs_t *mp) {
  chttp_async_ctx_t *ctx =
      (chttp_async_ctx_t *)_ccol_mem_calloc(mp, 1, sizeof(*ctx));
  if (!ctx) return NULL;
  ctx->mp = mp;
  ctx->fd = -1;
  /* _ccol_mem_calloc above gives both pending_app_teardown and
   * hop_completed the correct default of false. They need no explicit
   * initialization. */
  if (ccol_mutex_init(ctx->idle_lock) != 0) {
    _ccol_mem_free(mp, ctx);
    return NULL;
  }
  if (ccol_mutex_init(ctx->deadline_lock) != 0) {
    ccol_mutex_destroy(ctx->idle_lock);
    _ccol_mem_free(mp, ctx);
    return NULL;
  }
  return ctx;
}

/*
 * This is the one reliable place that frees a ctx, which is the connection
 * state of one hop. _async_ctx_teardown calls it, and that function runs
 * through _async_ctx_finish and _async_idle_ctx_finish. Those two explicit
 * terminal-teardown helpers below give this module a teardown point that
 * runs exactly once. See the comment at the top of the "ASYNC CONNECTION
 * STATE MACHINE" section for why ccol_event_loop needs an explicit point
 * rather than an implicit one. A failing code path also calls this function
 * directly, for a failure before the library ever registers a connection
 * with the reactor.
 *
 * Every field is safe to free or destroy without a condition. A field whose
 * ownership went elsewhere is NULL or cleared at the transfer site. headers
 * and bb.buf go to a response that the library built successfully.
 * _ccol_mem_free, _parse_ctx_free_fields, a NULL tls, a NULL reg and a
 * negative fd all do nothing.
 *
 * This function does NOT touch ctx->chain, which is shared state of the
 * whole chain. See _async_ctx_teardown, which pairs this call with the
 * matching release of the chain.
 *
 * This function always calls ccol_event_loop_remove and close(), and both
 * are idempotent and safe. No caller has to make those calls first. This
 * matches the treatment of ctx->tls and ctx->wire, which are also always
 * safe to free here whatever the caller already did.
 *
 * _client_deadline_unregister runs FIRST, strictly before close(ctx->fd).
 * That order is load-bearing and not incidental. See the comment of the
 * "ASYNC DEADLINE SWEEP" section. The shutdown() call of the deadline sweep
 * works on an fd. It is safe from a race over a reused fd for one reason
 * only. Every teardown path unregisters from that registry before it can
 * close its fd. An unrelated connection could otherwise reuse that fd
 * number.
 */
/* This prepares ctx->parser for the next response message of the ctx. A
 * parser that the ctx used before can hold the heap buffer of a long header
 * line, which goes first. A ctx from _ccol_mem_calloc holds a zeroed parser,
 * which the release leaves alone. A response header line can be up to
 * CHTTP1_MAX_SPILL_LINE_LEN bytes long; the fixed buffer of the parser
 * covers the common case, and only a longer line allocates. */
static void _async_parser_start(chttp_async_ctx_t *ctx) {
  ccol_call_once(client_http1_settings_bundler.once, _init_chttp1_settings);
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
  /* This is a short lock and unlock. The code holds it across nothing
   * below. It guarantees that the window of _async_connect_task after
   * ccol_event_loop_add is fully over. See the comment of that function,
   * right after it takes this same idle_lock. Without this, that window is
   * still open when this function frees ctx out from under it.
   *
   * Every other call site of _async_ctx_free and _async_ctx_teardown
   * already unlocks idle_lock before it calls in here, if it held the lock
   * at all. This lock can therefore never deadlock against a caller. */
  ccol_mutex_lock(ctx->idle_lock);
  ccol_mutex_unlock(ctx->idle_lock);
  _client_deadline_unregister(ctx); /* This does nothing when the library
                                     * never registered the ctx. It MUST run
                                     * before the close() below. */
  _async_race_detach(ctx);          /* After the unregister: the sweep reads the
                                     * race of a registered ctx. */
  if (ctx->reg) ccol_event_loop_remove(cli_engine_bundler.reactor, ctx->reg);
  ccol_event_reg room = atomic_exchange(&ctx->room_reg, CCOL_EVENT_REG_INVALID);
  if (room) ccol_event_loop_remove(cli_engine_bundler.reactor, room);
  if (ctx->tls) ctls_conn_destroy(ctx->tls);
  if (ctx->fd >= 0) close(ctx->fd);
  chttp1_parser_release(&ctx->parser);
  ccol_mutex_destroy(ctx->idle_lock);
  ccol_mutex_destroy(ctx->deadline_lock);
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
 * This is the one reliable place that physically frees a ctx. It is safe to
 * call without a condition, because of the invariant of this module.
 * See the field comment of pending_app_teardown, and the rule that it
 * upholds: no application thread ever frees a registered ctx.
 *
 * That invariant means that the library reaches this function in one of two
 * situations only. In the first, the library never registered ctx with the
 * reactor at all. That is a failure before the connect, and nothing else
 * can reference the ctx. In the second, this call runs inside a reactor
 * dispatch callback. ccol_event_loop already guarantees that such a
 * callback never runs concurrently with another dispatch for this exact
 * registration. Its own entry->dispatch_lock and its entry->refcount
 * invariant of at most one job in flight for each entry give that
 * guarantee.
 *
 * Neither situation needs a reference count of its own on ctx, and neither
 * has one.
 */
static void _async_ctx_free(chttp_async_ctx_t *ctx) {
  if (!ctx) return;
  _async_ctx_destroy_now(ctx);
}

/* This frees the per-connection state of one hop. It also frees the chain
 * reference of that hop. It is the standard end of every terminal code path
 * for a ctx. Those paths are a dispatch-driven teardown, and an early
 * failure before the library ever registers the ctx with the reactor.
 *
 * It copies chain into a local first, because _async_ctx_free frees ctx
 * itself. */
static void _async_ctx_teardown(chttp_async_ctx_t *ctx) {
  chttp_async_chain_t *chain = ctx->chain;
  _async_ctx_free(ctx);
  _async_chain_release(chain);
}

/*
 * This delivers a terminal result to the future of the caller exactly once.
 * chain->fulfilled guards it, under chain->lock. See the comment of that
 * field for why a lock must protect it, and why a plain bool is not enough.
 * A redirect chain can run the callbacks of two hops concurrently.
 *
 * It is safe to call this from more than one exit path. An error path
 * followed by the backstop call of _async_chain_release is one such pair.
 * Only the first call has an effect.
 *
 * The result struct comes from chain->result, which _chttp_do_async_internal
 * allocated before the request was queued. This function therefore
 * allocates nothing, and every request reports its own code, an allocation
 * failure on the reactor included.
 */
static void _async_fulfill_chain(chttp_async_chain_t *chain, ccol_retval_t rv,
                                 chttpcli_response *resp) {
  ccol_mutex_lock(chain->lock);
  bool already = chain->fulfilled;
  chain->fulfilled = true;
  chttpcli_async_result_t *result = chain->result;
  chain->result = NULL;
  ccol_mutex_unlock(chain->lock);
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
  /* This marks the ctx as terminal. The guard at the top of
   * _async_on_readable and of _async_on_writable then skips all further
   * work for it. See the field comment of ctx->hop_completed. A stray extra
   * callback call after this point must do nothing. It must not run logic
   * that is not idempotent. _async_handle_redirect is such logic. A second
   * ctls_conn_handshake_step call on a handshake that already failed is
   * another. */
  ctx->hop_completed = true;
  _async_fulfill_chain(ctx->chain, rv, resp);
}

/* This builds the chttpcli_response from the finished parse. It matches the
 * own response-building code at the end of chttp_do_internal in Tier 1.
 *
 * It moves ownership of ctx->pctx.headers and ctx->bb.buf out of ctx, and
 * it sets both fields to NULL there. The library calls it BEFORE the
 * idle-pool-offer path of _async_finish_connection. That path resets ctx
 * for its idle life, and it would otherwise free those exact same fields.
 * See how _async_on_data handles CHTTP1_PAUSED for why this order matters
 * on its own terms too. That order is: build the response, finish the
 * connection, then fulfil the future.
 *
 * A streaming request sets ctx->chain->write_fn. The library already
 * delivered the body bytes to the callback of the caller as they came off
 * the wire. See how _async_submit_hop and _async_retry_hop wire the sink.
 * ctx->bb was never the sink at all. There is therefore nothing to move
 * into resp->body, which stays NULL with a length of 0. This matches how
 * chttpclient_do_streaming of Tier 1 leaves chttpcli_response.body.
 *
 * The library still parses the headers internally, because redirect
 * detection needs them whatever the sink is. This function frees them here
 * and does not expose them. That matches the documented contract of
 * chttpclient_do_streaming: the response headers are not accessible through
 * this path. */
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

/* This builds the response and fulfils the future with it in one step. The
 * completion path that an EOF drives uses it. That path never pools its
 * connection. A peer that closes the connection to signal the end of the
 * body does not offer keep-alive. There is therefore no ordering hazard
 * with _async_finish_connection here. */
static void _async_fulfill_success(chttp_async_ctx_t *ctx) {
  chttpcli_response *resp = _async_build_response(ctx);
  _async_fulfill(ctx, resp ? ccol_success : ccol_not_enough_memory, resp);
}

/* ========================================================================== */
/*                         ASYNC IDLE POOL (TIER 2)                           */
/* ========================================================================== */

/*
 * This is the keep-alive idle pool of Tier 2. It is a
 * chmap(char *origin_key -> cvec of chttp_async_ctx_t*). Each chttpcli has
 * its own pool in cli->idle_pools_async.
 *
 * Its shape and its policy match idle_pools, _idle_pool_take and
 * _idle_pool_offer of Tier 1. It uses the same
 * CHTTP_MAX_IDLE_PER_ORIGIN, CHTTP_MAX_IDLE_TOTAL and
 * CHTTP_IDLE_MAX_AGE_MS caps. Its mechanism must differ.
 *
 * A pooled connection here stays attached to the shared reactor. There is
 * no way to detach the registration of a live connection without a close.
 * To pool a ctx therefore means to move it to CHTTP_ASYNC_IDLE.
 * The IDLE-state branches of the dispatch callbacks keep driving it
 * from there.
 *
 * Any activity on such a connection means that it is no longer usable. The
 * natural EOF or hangup that a dead connection produces counts as activity
 * too. The library then tears it down through the exact same on_close path
 * as an ordinary failed connection, and only enters that path from another
 * state.
 *
 * There is no way to peek at a reactor-owned fd synchronously. The MSG_PEEK
 * liveness probe of Tier 1 does exactly that. A connection can therefore
 * die in the narrow window between the pop out of the pool and the real
 * reuse. The retry-once mechanism at the point of use catches that case
 * instead. That mechanism uses reused and any_bytes_read. See
 * _async_retry_hop. Together these two mechanisms give Tier 2 the same
 * effective guarantee as the probe-and-retry pair of Tier 1.
 *
 * A pooled ctx holds its OWN engine reference.
 * _async_idle_pool_offer takes that reference. It is then freed by
 * whichever path claims the ctx next: the reuse path of
 * _async_idle_pool_take, or the IDLE-state on_close path.
 *
 * That reference is separate from the reference of any chain. The chain
 * that produced this connection is already fulfilled and torn down, or it
 * is about to be. The pooled connection itself must keep the engine alive
 * for as long as it sits in the pool.
 */

#ifdef RUNNING_UNIT_TESTS
/*
 * These flags inject a fault, and only a test uses them. The library
 * consumes each one the first time that it reads it, and resets it to
 * false. An injected failure from one test can therefore never leak into an
 * unrelated later test in the same process. The setters are under
 * "White-box test helpers" further below.
 *
 * The first two flags exist because ordinary allocator-failure injection
 * cannot reach the two branches that they simulate. tests.c uses the
 * g_hop_fail_mp pattern for that kind of injection elsewhere.
 *
 * The cvector_push_back call of _async_idle_pool_offer can never fail here.
 * CHTTP_MAX_IDLE_PER_ORIGIN is 4, and that equals the minimum_capacity
 * of cvector, which is also 4. The library always creates the list of an
 * origin with enough backing capacity for every element that the own
 * has_room check of _idle_pool_offer lets through. That push_back therefore
 * never has to grow the array with a realloc, and it can never fail through
 * an allocator that runs out of memory.
 *
 * The ccol_event_loop_modify call on the reused-connection path of
 * _async_submit_hop is similar. It allocates nothing of its own. Every
 * other failure condition of it is structurally unreachable for a
 * connection that the library just popped from the idle pool. Such a
 * connection was always registered for read only. Those conditions are bad
 * arguments, and a selectable that is not an fd. They also are a
 * registration that the library already removed, and a target direction
 * that is already occupied.
 *
 * Both branches are still real, reachable code. A later change can raise
 * CHTTP_MAX_IDLE_PER_ORIGIN past 4. A later change to ccol_event_loop can
 * add a new failure mode. Each branch is also a use-after-free hazard if it
 * handles the ownership of a ctx wrongly. These hooks exist only to keep
 * both branches under permanent and deterministic test coverage.
 *
 * The third flag exists for the same reason. It applies to how
 * _async_on_readable_impl separates a hard transport error from a real EOF.
 * See the comment of that function.
 *
 * A genuine TCP RST, or a fatal error at the TLS level, can arrive in a
 * SEPARATE dispatch from the data before it. That is a real scenario in
 * production. Its exact timing against data that the library already
 * buffered and consumed is an OS-level race. No test can pin that race down
 * over a real socket. A graceful close is different, and the own
 * eof_delimited_body_without_content_length tests of this file already
 * exercise it reliably.
 *
 * This hook forces the NEXT read dispatch for a ctx to behave as if recv()
 * or ctls_conn_read() returned -1 with ECONNRESET. It touches no real
 * socket at all. It applies only to a ctx that already had at least one
 * real successful read. It therefore never applies to the very first read
 * of a fresh or reused connection.
 */
static _Atomic bool g_force_offer_push_fail_for_tests = false;
static _Atomic bool g_force_reactivate_fail_for_tests = false;
static _Atomic bool g_force_async_hard_read_error_for_tests = false;

/*
 * This is a fourth flag. It covers the one race that the write-direction
 * fall-through of _async_on_writable_impl exists for.
 *
 * The deadline sweep samples the state of a ctx. It then flips the
 * registration of that ctx to the write direction, to deliver a
 * "100 Continue" timeout. In between, a dispatch on another thread can move
 * the same ctx out of CHTTP_ASYNC_AWAITING_CONTINUE. The flip can therefore
 * land on a ctx that already armed its own read direction and has nothing
 * to write.
 *
 * Which thread wins is an OS scheduling race. No test can pin that race
 * down over a real socket. This flag reproduces the losing order directly.
 * The next plain-HTTP hop that finishes the write of its request arms the
 * write direction again, right after it arms read. That is exactly the
 * state that the race leaves behind.
 *
 * g_stray_write_arm_restores_for_tests counts how many times that
 * fall-through really restored the read direction. It is a relaxed atomic
 * and not a plain counter, because the increment sits on a reactor dispatch
 * path that several threads run concurrently. Read it only after the
 * request under test completes.
 *
 * g_force_stray_idle_write_arm_for_tests reproduces the same thing one
 * transition later. It applies to a connection that reached the idle pool,
 * and not to one that still reads its response. The state that the sweep
 * sampled can be that stale too. A pooled connection already answered its
 * caller. The damage of this variant is therefore only a shared reactor
 * that spins on a dispatch which does nothing. That is what makes it the
 * bounded half of the pair.
 */
static _Atomic bool g_force_stray_write_arm_for_tests = false;
static _Atomic bool g_force_stray_idle_write_arm_for_tests = false;
static _Atomic unsigned long long g_stray_write_arm_restores_for_tests = 0;

/*
 * This widens the window around the carry-over replay for
 * "Expect: 100-continue". That replay sits in _async_upload_pause. The
 * flag widens the window on both sides.
 *
 * That replay can fulfil its chain and free the last reference of the
 * chain. That free is what unblocks a chttpclient_destroy call that waits
 * on async_in_flight_count. The replay then frees the bytes through the own
 * allocator of the client, and that destroy is free to have taken that
 * allocator away.
 *
 * How much of the destroy fits in between is pure scheduling. The delay
 * makes all of it fit. The leading half lets the destroy reach its wait
 * before the replay starts. The trailing half lets the destroy run to its
 * end before the free. A value of zero, which is the default, turns both
 * halves off.
 */
static _Atomic long g_continue_carry_replay_delay_ms_for_tests = 0;

static void _chttp_test_continue_carry_replay_delay(void) {
  long ms = atomic_load(&g_continue_carry_replay_delay_ms_for_tests);
  if (ms <= 0) return;
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

/*
 * This holds the IDLE branch of _async_on_readable_impl between its
 * decision that the ctx is idle and its read, once, so that a test can let
 * _async_idle_pool_take take the ctx for a new hop inside that window. The
 * order that it produces is the one where the take wins the race against
 * the dispatch that evicts the ctx. 0 is off, 1 armed, 2 holding, and 3
 * released. The hold ends after 5 seconds whatever the test does.
 *
 * It holds only a dispatch that finds input waiting on the socket. A stale
 * or duplicate dispatch of an idle ctx, which the IDLE branch documents,
 * can run before the peer has sent anything; holding that one would let the
 * take run while the input is still in flight, where no probe can see it,
 * and the test would then measure the in-flight case instead of the race.
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

/* This subtracts one from idle_total_count_async. When that count reaches
 * zero, it wakes every waiter on idle_async_drained. __chttpclient_destroy
 * is that waiter, and it waits for the teardown of every pooled connection
 * to finish. Call this only with cli->lock held. */
static void _async_idle_count_dec_locked(struct chttpclient *cli) {
  if (cli->idle_total_count_async > 0) cli->idle_total_count_async--;
  if (cli->idle_total_count_async == 0)
    ccol_cond_var_broadcast(cli->idle_async_drained);
}

/* This removes `target` from the idle list of its origin, when that list
 * still holds it. It swaps the element with the last one, because cvector
 * supports only push_back and pop_back. It then subtracts one from the
 * count.
 *
 * It does nothing and returns false when it does not find target. That
 * happens when the staleness eviction of _async_idle_pool_take already
 * popped it out. That eviction runs just before the IDLE-state on_data or
 * on_close path ALSO finds its natural death. Both sides are safe to call
 * this with no condition. Call this only with cli->lock held. */
/* This function deliberately does NOT call _async_idle_count_dec_locked.
 * That subtraction is the responsibility of the caller, and so is the
 * broadcast of idle_async_drained that the count can trigger at zero. The
 * caller defers both until the library has REALLY freed the ctx.
 *
 * A subtraction here, at the time of the removal, opens a use-after-free.
 * __chttpclient_destroy then sees the count reach zero. It goes on and
 * frees cli, and cli->m_procs with it. Meanwhile _async_idle_ctx_finish,
 * the only caller of this function, is still mid-teardown on a reactor
 * worker thread. It reads that same freed cli->m_procs through ctx->mp
 * inside its own _async_ctx_free call.
 *
 * ThreadSanitizer reports that use-after-free. valgrind does not. It only
 * lands under a workload that cycles many connections through a real death
 * that the server starts. See the comment of _async_idle_ctx_finish for the
 * ordering that this needs. */
static bool _async_idle_remove_locked(struct chttpclient *cli,
                                      chttp_async_ctx_t *target) {
  if (!cli->idle_pools_async || !target->origin_key) return false;
  cmap_pair kp = {.ptr = target->origin_key,
                  .size = strlen(target->origin_key) + 1};
  const cmap_pair *vp = NULL;
  if (chmap_get_elem_ref(cli->idle_pools_async, &kp, &vp) != ccol_success ||
      !vp)
    return false;
  cvec list = _read_cvec(vp->ptr);
  if (!list) return false;
  size_t n = cvector_elem_count(list);
  for (size_t i = 0; i < n; i++) {
    chttp_async_ctx_t **slot = (chttp_async_ctx_t **)cvector_at(list, i);
    if (*slot != target) continue;
    chttp_async_ctx_t *last = NULL;
    cvector_pop_back(list, &last); /* This removes the last element. It can
                                    * shrink the backing array, so every
                                    * pointer into that array from before
                                    * this call is stale. */
    if (i < cvector_elem_count(list)) {
      chttp_async_ctx_t **slot2 = (chttp_async_ctx_t **)cvector_at(list, i);
      *slot2 = last;
    }
    /* Prune the list and map entry of this origin now that it is empty.
     * See the identical comment of _idle_pool_take, the sibling function
     * in Tier 1. This is necessary, because the number of distinct
     * origins otherwise grows without a bound for a long-running client
     * that contacts many origins. It is also always safe, because
     * _async_idle_pool_offer already creates a fresh entry when there is
     * none for an origin yet. */
    if (cvector_elem_count(list) == 0) {
      __cvector_destroy(list);
      chmap_delete_elem(cli->idle_pools_async, &kp);
    }
    return true;
  }
  return false;
}

/*
 * This tries to pop a usable idle connection for `origin_key`. It returns
 * true and fills *out on success. It returns false when no fresh candidate
 * exists, and the caller then falls back to a fresh connect.
 *
 * IMPORTANT contract about the return: on a true return, ctx->idle_lock
 * stays LOCKED. The caller is _async_submit_hop. It must keep that lock
 * held while it reconfigures ctx for the new hop and tries the write. It
 * unlocks only after ctx is fully consistent again, with every field set
 * and the write tried.
 *
 * This is not a matter of style. A pop of a candidate out of the cvec of
 * the pool makes that candidate impossible to find for a concurrent
 * _async_idle_pool_take. It does nothing to stop the reactor. The reactor
 * can dispatch a readable, writable or error callback for that connection
 * on another thread at any moment. The connection stayed fully attached to
 * the reactor for as long as it sat in the pool. That is the same race as
 * every other one in this section.
 *
 * The caller holds idle_lock across the WHOLE reconfiguration, and not only
 * across the pair of state and chain. A narrower hold lets a concurrent
 * on_close see ctx half reconfigured. The state is already off
 * CHTTP_ASYNC_IDLE, while this thread still writes hop, pctx, wire and the
 * other fields. That on_close then tears ctx down. It frees ctx and
 * destroys idle_lock itself, while the caller of this function still uses
 * it. That is a use-after-free that goes deeper than a torn write of the
 * pair of state and chain.
 *
 * _async_dispatch_kind takes the same lock. on_readable, on_writable and
 * on_error all use that helper. Any of them that races this function simply
 * blocks until the caller unlocks. By then ctx is fully self-consistent,
 * one way or the other.
 *
 * The WHOLE decision to classify and pop for one origin runs inside a
 * single critical section over `cli->lock`. It scans the cvec of that
 * origin from the back to the front. It starts at the highest index, which
 * holds the warmest candidate that the library offered most recently. It
 * ends at the oldest candidate at index 0.
 *
 * That LIFO preference is deliberate. A scan from the front to the back
 * prefers the OLDEST fresh candidate instead. An older idle connection is
 * statistically more likely to be closed by the peer already.
 *
 * A candidate older than CHTTP_IDLE_MAX_AGE_MS gets a shutdown right here.
 * A read of its fd is safe, because this whole walk holds cli->lock. A
 * removal from this vector is the only thing that can invalidate a
 * candidate, and this same lock also always gates that removal.
 *
 * The walk deliberately leaves that stale candidate exactly where it is in
 * the vector. It writes nothing to state, chain or hop_completed. It
 * removes nothing. This function does NOT tear a stale candidate down
 * itself.
 *
 * shutdown() forces a genuine EPOLLIN or EOF on the fd of that candidate,
 * which is still registered for read. The idle branch of
 * _async_on_readable_impl then reaches _async_idle_ctx_finish, and that
 * path reaps the candidate for real from a dispatch context. That path
 * already makes its own exclusive claim with _async_idle_remove_locked. It
 * also already frees the pooled engine reference.
 *
 * The walk pops the first genuinely fresh candidate that it finds and
 * returns it. The pop swaps the element with the last one, which matches
 * the removal pattern of _async_idle_remove_locked.
 *
 * A pop of a stale candidate, plus a teardown of it with _async_ctx_finish
 * directly from this application thread, is a use-after-free.
 * AddressSanitizer reports it. A reactor dispatch callback for that exact
 * registered ctx can legitimately be in flight on another thread at that
 * same moment, or about to run. The OS can also preempt that thread for an
 * unbounded time. That gap sits between the liveness check of
 * ccol_event_loop and the first touch of ctx. This application thread can
 * free the ctx out from under that callback during the gap.
 *
 * An atomic refcount for each ctx, pinned by every dispatch callback, does
 * not close this either. The very first read of the refcount by the pin can
 * itself race a concurrent free in the same way.
 *
 * One approach closes it by construction, and this function uses it: no
 * application thread ever tears a registered ctx down at all. The walk only
 * marks a stale candidate, with shutdown() and not with any field of the
 * ctx. The SAME dispatch-context reaping path then takes it, and an idle
 * connection that dies naturally already goes through that path today. That
 * path is inherently safe, because ccol_event_loop serialises it against
 * every other dispatch for that exact registration. Its own
 * entry->dispatch_lock and its entry->refcount invariant of one job in
 * flight for each entry give that guarantee.
 *
 * This design accepts one tradeoff. A stale candidate that has a shutdown
 * but no reap yet still counts against CHTTP_MAX_IDLE_PER_ORIGIN and
 * CHTTP_MAX_IDLE_TOTAL. It counts in the capacity checks of
 * _async_idle_pool_offer until the dispatch-driven reap runs, which usually
 * takes one turnaround of the reactor. That is a small and short-lived drop
 * in the effective capacity of the pool, and not a correctness problem. The
 * alternative is a teardown by this function itself, which reopens the bug
 * above.
 */
static bool _async_idle_pool_take(struct chttpclient *cli,
                                  const char *origin_key,
                                  chttp_async_chain_t *chain,
                                  chttp_async_ctx_t **out) {
  chttp_async_ctx_t *ctx = NULL;
  ccol_mutex_lock(cli->lock);
  if (cli->idle_pools_async) {
    cmap_pair kp = {.ptr = (void *)origin_key, .size = strlen(origin_key) + 1};
    const cmap_pair *vp = NULL;
    if (chmap_get_elem_ref(cli->idle_pools_async, &kp, &vp) == ccol_success &&
        vp) {
      cvec list = _read_cvec(vp->ptr);
      if (list) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        size_t n = cvector_elem_count(list);
        for (size_t idx = n; idx-- > 0;) {
          chttp_async_ctx_t **slot =
              (chttp_async_ctx_t **)cvector_at(list, idx);
          chttp_async_ctx_t *cand = *slot;
          int64_t age_ms =
              ((int64_t)now.tv_sec - (int64_t)cand->last_used.tv_sec) * 1000 +
              ((int64_t)now.tv_nsec - (int64_t)cand->last_used.tv_nsec) /
                  1000000;
          /* chttpclient_set_tls can replace the configuration that a TLS
           * candidate used for its handshake. The walk refuses such a
           * candidate exactly like an over-age one, and by the same
           * mechanism. It calls shutdown() only. It leaves the real
           * teardown to the dispatch-context reaping path above.
           *
           * This pool is keyed on the origin alone. This check is
           * therefore the only thing that stops the library from answering
           * a request over a connection that used the old policy. The
           * caller issues that request after a reconfiguration. Such a
           * reconfiguration can pin a private CA in place of the system
           * bundle, turn verification back on, or rotate a client
           * certificate.
           *
           * Tier 2 skips CONNECTING and TLS_HANDSHAKING for a reused
           * connection. Nothing further down would therefore ever apply
           * the new policy. */
          if (age_ms > CHTTP_IDLE_MAX_AGE_MS ||
              (cand->is_https && cand->tls_generation != cli->tls_generation)) {
            int fd = cand->fd;
            if (fd >= 0) shutdown(fd, SHUT_RDWR);
            continue;
          }
          chttp_async_ctx_t *last = NULL;
          cvector_pop_back(list, &last); /* This can shrink the backing
                                          * array. Every pointer into that
                                          * array from before this call is
                                          * stale. */
          if (idx < cvector_elem_count(list)) {
            chttp_async_ctx_t **slot2 =
                (chttp_async_ctx_t **)cvector_at(list, idx);
            *slot2 = last;
          }
          _async_idle_count_dec_locked(cli);
          ctx = cand;
          /* Prune the list and map entry of this origin now that it is
           * empty. See the identical comment of _idle_pool_take, the
           * sibling function in Tier 1, for why this is necessary and
           * always safe.
           *
           * Note: the walk deliberately leaves a stale candidate that the
           * `continue` above discarded in the list. A dispatch reaps that
           * candidate later, and not this code. See the comment above this
           * function. This check can therefore only fire on the exit for a
           * genuine reuse, and never in the middle of the scan. */
          if (cvector_elem_count(list) == 0) {
            __cvector_destroy(list);
            chmap_delete_elem(cli->idle_pools_async, &kp);
          }
          break;
        }
      }
    }
  }
  ccol_mutex_unlock(cli->lock);
  if (!ctx) return false;

  ccol_mutex_lock(ctx->idle_lock); /* This lock stays held. See the contract
                                    * above. */
#ifdef RUNNING_UNIT_TESTS
  ctx->idle_lock_held_for_tests = true;
#endif
  /* See the field comment of ctx->deadline_lock. That dedicated leaf lock
   * is what makes this field safe for the sweep to read concurrently. The
   * deadline sweep deliberately never takes idle_lock. */
  ccol_mutex_lock(ctx->deadline_lock);
  ctx->overall_deadline = chain->overall_deadline;
  /* A reused connection starts its hop with no verdict of the sweep. The
   * offer already cleared the flag under this lock, and nothing sets it
   * while the ctx is idle. This keeps the start of every hop the same
   * whatever the idle life of the ctx was. */
  ctx->timed_out = false;
  ctx->chain = chain;
  ctx->state = CHTTP_ASYNC_WRITING;
  ccol_mutex_unlock(ctx->deadline_lock);
  *out = ctx;
  return true;
}

/*
 * This offers a connection that is still good, and eligible for
 * keep-alive, back to the async idle pool of cli. The same per-origin cap
 * and total cap as in Tier 1 bound it.
 *
 * The contract with the caller has exactly two values, and not three.
 * `false` means that this function did not handle the connection. It left
 * ctx COMPLETELY UNTOUCHED. The ctx is still attached to its original
 * chain, in whatever state it had on entry. The caller falls back to an
 * ordinary close. This matches the identical comment of Tier 1: a lost
 * optimisation, never a correctness problem.
 *
 * `true` means that this function handled the connection. The caller must
 * not touch ctx again. That value covers two internally different
 * outcomes. The first is the ordinary success case, where ctx really joins
 * the idle pool. The second is the cvector_push_back failure below, which
 * happens only when memory runs out, and is therefore very rare. In that
 * case this function already tore ctx down itself.
 *
 * A `false` return for the second outcome is a double free. The own
 * fallback path of the caller for `false` calls _async_ctx_finish. That
 * call then tears down a ctx whose reference this function already freed.
 * This is the same class of lifetime hazard that the field comment of
 * pending_app_teardown describes. It is closed in the same way, by
 * construction and not by a refcount.
 *
 * On success this function detaches ctx from its original chain. It frees
 * the one chain reference that this hop held for it. The hop is over. The
 * chain no longer owns this connection, and the idle pool does. Without
 * that free, the reference leaks outright. A pooled connection never goes
 * through the ordinary _async_ctx_teardown path until the library reuses it
 * or evicts it later. That path is the only OTHER place that frees the
 * chain reference of a ctx.
 *
 * The capacity check runs FIRST, entirely under cli->lock. It asks whether
 * the pool has room for this origin, and it creates the list of that origin
 * on demand. It decides for certain whether this offer can succeed before
 * it touches ctx AT ALL.
 *
 * Only after that is certain does this function reset ctx to a clean idle
 * state. That reset clears the parse, body and wire buffers of the hop, and
 * marks the ctx CHTTP_ASYNC_IDLE. The function then inserts the ctx. Both
 * steps stay inside the SAME critical section over cli->lock, so no other
 * thread can pop from the list in between.
 *
 * That order matters for two independent reasons. First, ctx must never
 * become visible and poppable through _async_idle_pool_take while the reset
 * is only partly done. _async_idle_pool_take always overwrites whatever it
 * finds, and it waits for nothing. A concurrent take() can start to
 * reconfigure ctx for a brand-new hop. This function can still free and
 * reset those exact same fields for the OLD hop. That is a genuine data
 * race on the fields themselves. It is more than a logical
 * inconsistency.
 *
 * Second, a version of this function that resets ctx too early breaks a
 * contract. Such a version resets ctx before it knows whether the pool has
 * room. The contract above leaves ctx untouched on a failure.
 */
static bool _async_idle_pool_offer(chttp_async_ctx_t *ctx) {
  struct chttpclient *cli = ctx->cli;

  /* This ctx is about to belong to the idle pool, and to no chain. It
   * therefore needs its own engine reference. That reference keeps the
   * reactor alive while the ctx sits here. It is independent of the chain
   * whose lifetime just ended. This code takes it before everything else,
   * so that a failure here has to unwind no pool state at all. */
  if (_client_engine_acquire() != ccol_success) return false;

  cvec list = NULL;
  bool has_room = false;
  ccol_mutex_lock(cli->lock);
  if (!cli->destroying && cli->idle_pools_async &&
      cli->idle_total_count_async < CHTTP_MAX_IDLE_TOTAL) {
    cmap_pair kp = {.ptr = ctx->origin_key,
                    .size = strlen(ctx->origin_key) + 1};
    const cmap_pair *vp = NULL;
    if (chmap_get_elem_ref(cli->idle_pools_async, &kp, &vp) == ccol_success &&
        vp) {
      list = _read_cvec(vp->ptr);
    }
    /* A genuinely new origin gets a fresh entry only while the cap on
     * distinct origins still has room. See the comment of
     * CHTTP_MAX_IDLE_ORIGINS. _idle_pool_offer of Tier 1 is the sibling
     * that this code matches. At the cap, list stays NULL. The code then
     * falls through to the same "it does not fit, do not pool it" path
     * that the check of has_room below already produces. */
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
    ccol_mutex_unlock(cli->lock);
    _client_engine_release();
    return false; /* ctx stays untouched. The caller falls back to an
                   * ordinary close. */
  }

  /* The decision is made: this ctx WILL leave the chain and join the idle
   * pool. Copy the chain reference out for the free below. Then reset ctx
   * while this code STILL holds cli->lock. See the comment above this
   * function for why. */
  chttp_async_chain_t *old_chain = ctx->chain;

  /* _parse_ctx_free_fields frees location and headers, and sets both to
   * NULL. It leaves every other field stale. Those fields are the status
   * code, the sink wiring, and the per-message flags that a finished hop
   * leaves behind.
   *
   * That is safe at the other call sites of that helper. _async_ctx_free
   * calls it right before it frees the whole ctx. Tier 1 calls it on a
   * per-hop chttp_parse_ctx_t, which is a fresh stack struct for each hop.
   * Both discard the struct right after the call.
   *
   * This code does NOT discard it, because the library is about to reuse
   * ctx for a later hop. A stale flag or status code from the finished hop
   * would therefore be read as a description of the next hop.
   *
   * The explicit memset below runs after _parse_ctx_free_fields frees
   * everything that needs a free. It restores the same all-zero state that
   * a ctx from _ccol_mem_calloc already has. Every other user of
   * chttp_parse_ctx_t depends on that state. */
  _parse_ctx_free_fields(&ctx->pctx);
  memset(&ctx->pctx, 0, sizeof(ctx->pctx));
  /* A long header line of the finished hop can have left a heap line
   * buffer in the parser. An idle connection keeps none. */
  chttp1_parser_release(&ctx->parser);
  _ccol_mem_free(ctx->mp, ctx->bb.buf);
  ctx->bb.buf = NULL;
  ctx->bb.len = ctx->bb.cap = 0;
  ctx->bb.oom = false;
  ctx->bb.too_large = false;
  _ccol_mem_free(ctx->mp, ctx->wire);
  ctx->wire = NULL;
  ctx->wire_len = ctx->wire_sent = 0;
  /* This is the state for "Expect: 100-continue". It is stale the moment
   * that this hop is done. Every fresh hop attempt computes
   * want_100_continue and header_len again in _async_submit_hop and
   * _async_retry_hop. Each of those also arms continue_decided to false.
   *
   * A reset here as well matches the discipline of this function above:
   * restore the same all-zero state that a ctx from _ccol_mem_calloc
   * already has.
   *
   * continue_carry should always be NULL by the time a hop reaches here.
   * _async_upload_pause consumes it in the same dispatch that stores it.
   * See the comment of that field. This code still frees
   * it as a defence, the same as wire and bb just above. */
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
  /* This field is stale the moment that the ctx goes idle. It described
   * the request path of the hop that just finished, and it means nothing
   * while the pool holds the ctx. This code frees it here. It does not
   * leave it dangling until the next _async_submit_hop call overwrites it.
   * This matches the identical treatment of wire and bb just above. */
  _ccol_mem_free(ctx->mp, ctx->path_and_query);
  ctx->path_and_query = NULL;
  ctx->hop_completed = false;
  ctx->reused = false;
  ctx->any_bytes_read = false;
  ctx->interim_responses_seen = 0;
  /* ctx->chain and ctx->state flip together, under idle_lock. See the
   * comment of that field. Without the lock, a concurrent on_data,
   * on_ready or on_close dispatch on another thread can see the pair of
   * writes torn. It sees state already IDLE with chain not yet NULL, or
   * the opposite.
   *
   * This code resets ctx->timed_out in the same critical section, as late
   * as it can. The deadline sweep sets that flag in two cases only. The
   * first is a ctx in a connect state, which is not true here, because
   * this ctx just finished a hop cleanly. The second is has_chain, which
   * means node->chain != NULL.
   *
   * A reset here, right where the code clears chain and sets state to
   * IDLE, therefore closes a window. See the comment of
   * _client_deadline_sweep_once about has_chain. In that window the sweep
   * can mark this ctx as timed out from the already stale
   * overall_deadline of the hop that just finished. That poisons a FUTURE
   * unrelated hop that later reuses this ctx from the idle pool.
   *
   * Without this reset, the dead-connection retry of that future hop
   * starves. That retry is the `reused && !any_bytes_read` branch of
   * _async_ctx_finish. A stale ctx->timed_out from a hop that already
   * succeeded starves it. The library then reports ccol_timed_out to a
   * caller whose own deadline was never in danger. */
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_offer_entered_for_tests, 1);
#endif
  ccol_mutex_lock(ctx->idle_lock);
  /* deadline_lock makes this one step against the verdict of the deadline
   * sweep, which takes its snapshot and writes timed_out under that lock.
   * See _client_deadline_sweep_once. deadline_lock is a leaf lock, so its
   * nesting inside idle_lock adds no ordering cycle. */
  ccol_mutex_lock(ctx->deadline_lock);
  ctx->chain = NULL;
  ctx->state = CHTTP_ASYNC_IDLE;
  ctx->timed_out = false;
  ccol_mutex_unlock(ctx->deadline_lock);
  ccol_mutex_unlock(ctx->idle_lock);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_offer_done_for_tests, 1);
#endif

  clock_gettime(CLOCK_MONOTONIC, &ctx->last_used);
  bool pushed;
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_force_offer_push_fail_for_tests, false)) {
    /* This simulates a failed cvector_push_back. The code deliberately
     * does NOT make the real call. list therefore stays exactly as a
     * genuine failure to allocate leaves it, with nothing inserted. See
     * the comment of g_force_offer_push_fail_for_tests for why the
     * real call can never fail here. */
    pushed = false;
  } else
#endif
  {
    pushed = (cvector_push_back(list, &ctx) == ccol_success);
  }
  if (pushed) cli->idle_total_count_async++;
  ccol_mutex_unlock(cli->lock);

  if (!pushed) {
    /* This case is very rare. cvector_push_back itself runs out of
     * memory, although the check above confirmed that there is room. This
     * code already detached ctx from old_chain and reset it to an idle
     * shape. It can therefore no longer give ctx back to the caller as an
     * ordinary active ctx to close. The has_room == false path above can
     * do that. This path tears ctx down directly instead.
     *
     * This code frees ctx BEFORE it frees old_chain, and never after. The
     * free of the last reference of the chain is what lets a
     * chttpclient_destroy call go on. That call blocks until
     * async_in_flight_count reaches zero. It then frees the client. For a
     * client from ccol_create_chttpclient_mp, it also frees the own
     * ccol_memmgmt_procs_t of the caller. That is the very allocator that
     * _async_ctx_free uses to free ctx. */
    _async_ctx_free(ctx);
    _client_engine_release();
    _async_chain_release(old_chain);
    return true; /* This is true and NOT false. This function already tore
                  * ctx down itself. The has_room == false path above is
                  * different: it returns false with ctx completely
                  * untouched. The caller is _async_finish_connection, and
                  * it must not ALSO call _async_ctx_finish on this ctx.
                  *
                  * A false here is wrong for exactly that reason. It is
                  * the same class of lifetime hazard that the field
                  * comment of pending_app_teardown describes. The
                  * contract of _async_finish_connection says that false
                  * means "fall back to an ordinary teardown of ctx". That
                  * is exactly wrong for this failure. It calls
                  * _async_ctx_finish a second time on a ctx whose
                  * reference this function already freed, which is a
                  * genuine double free. */
  }

#ifdef RUNNING_UNIT_TESTS
  /* See g_force_stray_idle_write_arm_for_tests. This arms the write
   * direction on a connection that just joined the idle pool. A flip by
   * the deadline sweep on a stale sampled state does the same thing. The
   * library consumes the flag once for each arming. */
  if (atomic_exchange(&g_force_stray_idle_write_arm_for_tests, false)) {
    _async_reg_modify(ctx, ccol_select_write);
  }
#endif
  _async_chain_release(old_chain);
  return true;
}

/*
 * The library calls this after it fully consumes the response of a hop, and
 * before it decides between a redirect and a fulfil. It offers the
 * connection back to the idle pool of cli when `reusable` is true. It
 * closes the connection otherwise.
 *
 * This matches the identical pair of reusable and _idle_pool_offer in
 * chttp_do_internal of Tier 1. It applies uniformly, whether this hop turns
 * out to be a redirect or the final response. See _async_handle_redirect,
 * and how _async_on_data handles CHTTP1_PAUSED. Both call this before they
 * do anything else with the connection.
 */
/*
 * This is the teardown point of this module for an ACTIVE ctx, which
 * the idle pool does not hold. It runs exactly once, and that is
 * guaranteed. Every place that ends such a connection calls it exactly
 * once. Those places are a hard error, a timeout, and an explicit close
 * after a successful completion or a redirect that the library did not
 * pool. ccol_event_loop has no unconditional terminal callback of its own
 * to rely on instead. See the comment at the top of the "ASYNC CONNECTION
 * STATE MACHINE" section.
 *
 * This function deliberately does NOT fulfil the future with a generic
 * error before it checks whether a retry is possible. Such a fulfil marks
 * ctx->hop_completed. It then wrongly skips a legitimate retry of a reused
 * connection.
 *
 * This function fulfils nothing itself when neither timed_out nor the
 * reused-retry case applies. The chain release in _async_ctx_teardown below
 * carries its own backstop, and chain->fulfilled guards it. That backstop
 * fulfils the future with the generic ccol_http_transfer_aborted exactly
 * once. Every caller of this function that needs no more specific code
 * depends on that value.
 */
/* This is a forward declaration. The body of this function calls
 * _async_retry_hop. The code defines that function much further below,
 * after the "ASYNC IDLE POOL" section, next to the TLS and plain write
 * helpers that also need it. */
static void _async_retry_hop(chttp_async_ctx_t *ctx);

static void _async_ctx_finish(chttp_async_ctx_t *ctx) {
  if (!ctx->hop_completed) {
    if (ctx->timed_out) { /* This field is _Atomic, so a plain read is
                           * already free of a race. */
      _async_fulfill(ctx, ccol_timed_out, NULL);
    } else if (ctx->reused && !ctx->any_bytes_read && !ctx->retry_unsafe) {
      _async_retry_hop(ctx); /* This sets ctx->hop_completed to true
                              * itself. */
    }
  }
  _async_ctx_teardown(ctx);
}

/*
 * This is the teardown point of this module for a ctx that the idle
 * pool holds. It removes that ctx from the pool and frees it. Such a ctx
 * holds its own engine reference, which _async_idle_pool_offer took. It
 * holds the reference of no chain, because ctx->chain is NULL while the ctx
 * is idle.
 *
 * This function really tears ctx down only when THIS call is the one that
 * removes it from the cvec of the pool. cli->lock is the sole arbiter of
 * exclusive ownership here. See the bool return of
 * _async_idle_remove_locked. When the removal finds nothing, because a
 * concurrent caller already removed it, this function returns at once and
 * touches ctx no more.
 *
 * That check is load-bearing and not a defensive nicety. N reactor threads
 * share one epoll instance, and there is no deduplication like
 * EPOLLEXCLUSIVE. The EOF condition of a genuinely dead connection is
 * PERSISTENT. A one-shot event for readable data is different. Every later
 * recv() or peek on an fd that is already closed keeps reporting EOF. It
 * never reports EWOULDBLOCK.
 *
 * Two sequential stale dispatches for the same idle ctx can therefore both
 * legitimately see a real EOF. They see it through the peek in the idle
 * branch of _async_on_readable, and both decide to evict the ctx. This
 * function therefore calls _async_ctx_free and _client_engine_release ONLY
 * when the removal above really found something.
 *
 * A call to those two without that condition is a double free in exactly
 * that two-dispatch case. Ordinary testing does not surface it. It needs a
 * pooled connection that the server really closed for good, and not merely
 * a stale but harmless spurious dispatch.
 *
 * This code subtracts one from idle_total_count_async in a THIRD separate
 * locked section. That section can also broadcast idle_async_drained. It
 * runs after both _async_ctx_free and _client_engine_release fully run
 * below. It is not folded into the removal step above.
 *
 * __chttpclient_destroy reads a count of zero as its signal that every
 * pooled connection really finished its teardown. It then goes on and frees
 * cli, and cli->m_procs with it. ctx->mp still points at that same
 * cli->m_procs.
 *
 * A subtraction at the time of the removal instead lets that signal fire
 * too early. The _async_ctx_free call of this function then still uses
 * cli->m_procs on this thread. That is a use-after-free. ThreadSanitizer
 * reports it, and valgrind alone does not. See the comment of
 * _async_idle_remove_locked for the full account.
 */
static void _async_idle_ctx_finish(chttp_async_ctx_t *ctx) {
  struct chttpclient *cli = ctx->cli;
  ccol_mutex_lock(cli->lock);
  bool removed = _async_idle_remove_locked(cli, ctx);
  ccol_mutex_unlock(cli->lock);
  if (!removed) return;
  _async_ctx_free(ctx);
  _client_engine_release();
  ccol_mutex_lock(cli->lock);
  _async_idle_count_dec_locked(cli);
  ccol_mutex_unlock(cli->lock);
}

/*
 * This ends a CONNECTING ctx whose non-blocking connect completed with an
 * error. That is a failed getsockopt(SO_ERROR) on a writable dispatch, or
 * an error dispatch. The connect of such a ctx is a single one: a target
 * with more than one address connects through the race of the "ASYNC HAPPY
 * EYEBALLS" section, which hands the ctx a connected socket only.
 *
 * It reports the failure. ctx->timed_out is checked first. The
 * shutdown(fd, SHUT_RDWR) call that the deadline sweep makes against a
 * stuck connect also arrives here as a failed connect. Without that check,
 * the library reports an expired connect_timeout_us as
 * ccol_http_connection_failed. It should report ccol_timed_out, which
 * chttpclient.h documents.
 */
static void _async_connect_failed(chttp_async_ctx_t *ctx) {
  _async_fulfill(
      ctx, ctx->timed_out ? ccol_timed_out : ccol_http_connection_failed, NULL);
  _async_ctx_finish(ctx);
}

static void _async_finish_connection(chttp_async_ctx_t *ctx, bool reusable) {
  if (reusable && ctx->origin_key && _async_idle_pool_offer(ctx)) return;
  _async_ctx_finish(ctx);
}

/* This is a forward declaration. _async_on_readable below calls this
 * function for a reused connection that turns out to be dead before the
 * library reads any response byte. The full definition comes later in this
 * file, next to _async_submit_hop. It sits after _async_connect_task, which
 * it needs to queue the replacement attempt. */
static void _async_retry_hop(chttp_async_ctx_t *ctx);

/* This is a forward declaration. _async_tls_advance below calls this
 * function after the handshake completes. The definition comes later in
 * this file. */
static void _async_tls_try_write(chttp_async_ctx_t *ctx);

/* This is a forward declaration. _async_replay_continue_carry below calls
 * this function for the "Expect: 100-continue" trailing bytes. See the
 * field comment of ctx->continue_carry. The full definition comes later in
 * this file, and the ordinary on_readable dispatch path shares it. */
static void _async_process_reading_data(chttp_async_ctx_t *ctx,
                                        const char *data, size_t data_len);

/*
 * This drives the client TLS handshake one step at a time. It runs from
 * _async_on_readable or from _async_on_writable, whichever fires next. The
 * library flips the single registration of this ctx between the read and
 * the write direction with ccol_event_loop_modify. It follows the own
 * WANT_READ and WANT_WRITE demands of the handshake.
 *
 * No lock around ctx->tls is needed. The per-registration
 * dispatch_lock of ccol_event_loop already guarantees that the read and the
 * write direction of this ctx never run concurrently with each other. See
 * the comment at the top of this section for why that removes every need
 * for an tls_lock of this module.
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
    /* A value of 0 is X509_V_OK by the convention of OpenSSL. ctls.h
     * deliberately exposes no OpenSSL header to a caller. This code
     * therefore compares the raw value directly, and not against the
     * X509_V_OK symbol. This matches _tls_handshake of Tier 1.
     *
     * A handshake failure is never eligible for a retry, whatever
     * ctx->reused says. Such a failure is a problem at the TLS level. It
     * is no evidence that the pooled connection merely went stale.
     *
     * This code checks ctx->timed_out FIRST. That field is _Atomic, so a
     * plain read is already free of a race. This matches the read of
     * the same field in _async_ctx_finish.
     *
     * The shutdown(fd, SHUT_RDWR) call that the deadline sweep makes
     * against a stuck handshake appears here as CTLS_HANDSHAKE_ERROR.
     * Without this check, the library reports an expired
     * connect_timeout_us or request_timeout_us as a TLS handshake failure
     * or a certificate failure. It should report ccol_timed_out. That
     * would contradict the documented contract of chttpclient.h. */
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
 * A response that starts to arrive while Tier 2 still sends the request.
 * See chttp_early_resp_t for the case and for the rules, which are the same
 * in every tier. The single registration of a ctx watches one direction at
 * a time, so Tier 2 applies them at the points where the send cannot go
 * on:
 *   - A write that would block while the socket holds response bytes PAUSES
 *     the upload. The ctx moves to CHTTP_ASYNC_READING with
 *     ctx->upload_paused set, and the read path parses what arrived.
 *   - A write that fails because the peer is gone STOPS the upload. The ctx
 *     moves to CHTTP_ASYNC_READING without the flag, and the read path takes
 *     the answer that the peer left, or finds none.
 *   - While the upload is paused, a discarded interim response, a read that
 *     finds nothing more before any message started, and a final response
 *     of 200 to 299 whose header block arrived resume it
 *     (_async_upload_resume). A final response of 300 or above ends the
 *     pause for good: the rest of the request is never sent.
 * A ctx whose request did not go out in full is never offered to the idle
 * pool; see _async_request_sent.
 *
 * A write that blocks needs the write direction, and a response can then
 * arrive on the other one. The deadline sweep covers that direction for
 * each tick of CHTTP_DEADLINE_SWEEP_INTERVAL_MS (ctx->flip_watch): once
 * input arrives for a send that waits for room (CHTTP_FLIP_ON_INPUT), it
 * turns the registration to the read direction, and the send pauses. A
 * response of 200 to 299 that streams in while the request goes out
 * (ctx->duplex) is the one case where both directions carry the exchange,
 * as a server that echoes the body does. There a blocked write waits for
 * the read direction first, because such a server answers what it read,
 * and the sweep turns the registration back to the write direction once
 * there is room to write (CHTTP_FLIP_ON_ROOM), for a server that reads on
 * without writing.
 */

static void _async_plain_try_write(chttp_async_ctx_t *ctx);

/* This is true once every byte of the request went out before a response
 * started. A connection for which it is false is never reused. */
static inline bool _async_request_sent(const chttp_async_ctx_t *ctx) {
  return ctx->wire_sent >= ctx->wire_len && !ctx->answered_early;
}

/* This is true when the socket, or the TLS layer above it, holds input that
 * a read would take now. It runs only on a write that would block, so the
 * write path that makes progress pays nothing for it. */
static bool _async_input_pending(chttp_async_ctx_t *ctx) {
  /* ctls_conn_has_pending_input also reports the start of a record that is
   * not complete yet. After a read that blocked on such a start, only the
   * socket can bring the rest, so the socket alone answers; see the field
   * comment of ctx->tls_read_blocked. */
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

/* Changes the direction of ctx->reg. The room watch holds the write
 * direction of the fd while it exists, and the event loop keeps one
 * registration for each direction of an fd, so a turn of reg to the write
 * direction ends the room watch first; the write dispatch that follows
 * resumes a paused send in its place. */
static ccol_retval_t _async_reg_modify(chttp_async_ctx_t *ctx,
                                       ccol_select_dir dir) {
  if (dir == ccol_select_write) _async_room_watch_stop(ctx);
  return ccol_event_loop_modify(cli_engine_bundler.reactor, ctx->reg, dir);
}

static void _async_on_room(ccol_event_loop loop, ccol_event_reg reg,
                           ccol_selectable *sel, void *arg);

/* Starts the room watch of a duplex send that a full send buffer paused;
 * see ctx->room_reg. It gives false when the registration cannot be added.
 * The caller holds the dispatch lock of the fd, or owns the ctx alone. */
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

/* The room watch fired: the paused duplex send can go on. The dispatch
 * lock of the fd serialises this with every dispatch of ctx->reg, and the
 * teardown of ctx removes this registration before it frees ctx. An error
 * on the fd also arrives here, as the event loop routes it to the only
 * handler that this registration has; the write that follows reports it. */
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

/* See the field comment of ctx->continue_carry. The bytes that it holds are
 * the start of the final response, and they reach the parser before any
 * byte that a later read takes. Every caller calls this as its last action:
 * the replay can end the hop and free ctx.
 *
 * This code copies mp into a local first. _async_process_reading_data can
 * free ctx entirely on this same call. A completed final response with no
 * redirect is one such case. Nothing may touch ctx afterwards.
 *
 * This code retains the chain for the whole replay. It frees that reference
 * only after it frees carry. The replay can fulfil the chain and drop its
 * last reference. That drop is what lets a chttpclient_destroy call go on.
 * That call blocks until async_in_flight_count reaches zero. It then frees
 * the client. For a client from ccol_create_chttpclient_mp, it also frees
 * the own ccol_memmgmt_procs_t of the caller. Without the retain, the free
 * of carry on this thread then reads mp->free out of freed memory and calls
 * through it. */
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

/* This ends the send of the request for now (paused == true) or for good,
 * and hands the ctx to the read path. It is the last action of its caller:
 * it can end the hop and free ctx. */
static void _async_upload_pause(chttp_async_ctx_t *ctx, bool paused) {
  ctx->upload_paused = paused;
  atomic_store(&ctx->flip_watch, CHTTP_FLIP_NONE);
  ctx->state = CHTTP_ASYNC_READING;
  if (_async_reg_modify(ctx, ccol_select_read) != ccol_success) {
    _async_ctx_finish(ctx);
    return;
  }
  /* A duplex send waits for room and for input at once: reg now watches
   * input, and the room watch the write direction. Without the room watch
   * (it could not be added), the deadline sweep turns reg to the write
   * direction once there is room, which costs up to one sweep tick for each
   * send buffer of the request. */
  if (paused && ctx->duplex && !_async_room_watch_start(ctx))
    atomic_store(&ctx->flip_watch, CHTTP_FLIP_ON_ROOM);
  _async_replay_continue_carry(ctx);
}

/* This goes on with a paused send. It is the last action of its caller: the
 * write can end the hop and free ctx. */
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

/* A write that succeeds checks for a response that arrived meanwhile once
 * this many request bytes went out since the last check. A server that
 * answers early and then closes with the rest of the request unread resets
 * the connection, and on Linux the reset discards an answer that the client
 * has not read yet; a server with a lingering close (nginx, chttpsvr) reads
 * on for a while first. The check makes the send find the answer soon after
 * it arrives even while the writes keep succeeding, and it costs one poll(2)
 * for this many bytes. A request that one write takes whole is never
 * checked, so a small request pays nothing. */
#define CHTTP_UPLOAD_INPUT_CHECK_BYTES (32u * 1024u)

/* This is true when a write that succeeded leaves request bytes to send and
 * a response has started to arrive, which pauses the send; see above. It
 * polls once CHTTP_UPLOAD_INPUT_CHECK_BYTES or more went out since the last
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

/* This is true when a paused send may go on: no message has started since
 * the last one that the read path discarded, or the message that did start
 * is a final response of 200 to 299 whose header block arrived. */
static inline bool _async_upload_may_resume(const chttp_async_ctx_t *ctx) {
  return ctx->upload_paused &&
         (!ctx->resp_msg_started ||
          (ctx->pctx.status_code >= 200 && ctx->pctx.status_code < 300));
}

/*
 * This writes as much of ctx->wire[ctx->wire_sent..] as ctls_conn_write
 * accepts now. It tracks partial progress. A short write is ordinary and
 * not an error. The library calls it once right after the handshake
 * completes. It calls it again from _async_on_writable each time that the
 * write interest fires.
 */
static void _async_tls_try_write(chttp_async_ctx_t *ctx) {
  if (atomic_load_explicit(&ctx->flip_watch, memory_order_relaxed))
    atomic_store_explicit(&ctx->flip_watch, CHTTP_FLIP_NONE,
                          memory_order_relaxed);
  /* For "Expect: 100-continue", stop at the boundary between the header
   * and the body, and not at the end of wire. This happens exactly once
   * for each hop attempt. continue_decided becomes true the moment that a
   * real "100 Continue" arrives, or that the wait expires. See the doc
   * comment of CHTTP_ASYNC_AWAITING_CONTINUE for the full contract with
   * three outcomes. This code matches that contract from
   * _chttp_send_and_read of Tier 1. */
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
      /* A response that arrived while the request goes out; see
       * _async_upload_pause. The wait for a "100 Continue" reads its own
       * answer, so the send of the header block of that case is left
       * alone. */
      if (!awaiting_continue && (ctx->duplex || _async_input_pending(ctx))) {
        _async_upload_pause(ctx, true);
        return;
      }
      /* An EWOULDBLOCK from ctls_conn_write() does not always mean "wait
       * for writable". OpenSSL can need to READ before this exact call can
       * make progress. A TLS 1.2 renegotiation is one such case. A TLS 1.3
       * KeyUpdate that arrives while the send buffer is briefly full is
       * another. See the doc comment of ctls_conn_wants_write.
       *
       * The reactor is level-triggered. A return that leaves this
       * registration pinned to the write direction therefore hands the
       * SHARED process-wide reactor thread an event that is ready at once.
       * That thread can make no progress on it. That spins the thread at
       * 100 percent CPU. The read-direction dispatch that this code flips to
       * re-enters this function through the CHTTP_ASYNC_WRITING branch
       * of _async_on_readable_impl.
       *
       * This code sets the direction without a condition, and not only
       * when the direction changes. The read-direction dispatch that
       * consumes a "100 Continue" also enters this function. That dispatch
       * is _async_awaiting_continue_on_data. The registration is still on
       * read there, and a write that blocks needs a flip the other way.
       * ccol_event_loop_modify is idempotent for a direction that is
       * already in effect. */
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
    /* n is 0, or there is a hard error. The connection is dead, but the
     * peer may have answered before it went: a close_notify (n == 0) or a
     * reset (ECONNRESET or EPIPE) leaves that answer readable. The read
     * path takes it, or finds none, and then _async_ctx_finish checks
     * whether a retry is possible. See _async_upload_pause. Any other
     * error ends the hop here, and _async_ctx_finish makes the same
     * check. */
    if (n == 0 || errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN) {
      _async_upload_pause(ctx, false);
      return;
    }
    _async_ctx_finish(ctx);
    return;
  }
  if (awaiting_continue) {
    /* The library sent the whole header block. It now holds the body,
     * which is wire[header_len..wire_len), back. It waits for one of three
     * things from the server: a "100 Continue", a direct final response,
     * or a timeout.
     *
     * wire must stay alive here, whatever ctx->reused says. The ordinary
     * full-send case below is different. The body bytes that the library
     * must still send live in wire. A free of it now is a use-after-free
     * on a fresh ctx, the very next time that this hop tries to send the
     * body. */
    /* This code computes and stores continue_deadline BEFORE the store to
     * ctx->state below, and not after.
     *
     * The deadline sweep polls ctx->state on its own timer. It coordinates
     * with nothing beyond that one atomic load. It reads a visible
     * CHTTP_ASYNC_AWAITING_CONTINUE as its signal that continue_deadline
     * now has meaning. See the comment of that field.
     *
     * ctx->state is the real publish point. C11 gives sequentially
     * consistent atomics. A concurrent atomic_load of ctx->state that sees
     * this store therefore also sees every plain write that this thread
     * sequenced before that store.
     *
     * A write of continue_deadline afterwards lets the sweep see the new
     * state while continue_deadline still holds a stale or zeroed value.
     * calloc leaves such a value, and so does an earlier hop on a reused
     * ctx. The sweep reads it as already expired. It then fires the
     * wait-timeout path at once, and the library never waits the full
     * CHTTP_100_CONTINUE_WAIT_US. */
    ccol_mutex_lock(ctx->deadline_lock);
    ctx->continue_deadline = _deadline_earlier(
        _deadline_make(CHTTP_100_CONTINUE_WAIT_US), ctx->overall_deadline);
    ccol_mutex_unlock(ctx->deadline_lock);
#ifdef RUNNING_UNIT_TESTS
    _continue_deadline_stored_for_tests();
#endif
    ctx->state = CHTTP_ASYNC_AWAITING_CONTINUE;
    if (_async_reg_modify(ctx, ccol_select_read) != ccol_success) {
      _async_ctx_finish(ctx);
    }
    return;
  }
  /* The wire buffer of a REUSED ctx must survive a write that looks
   * successful. The local socket buffer accepts those bytes, and that does
   * NOT guarantee that the peer is still alive to answer. See
   * _async_retry_hop, which can run against THIS ctx later. The read side
   * can find that the connection was already dead. The retry then needs the
   * ORIGINAL request bytes to send again on a fresh connection.
   *
   * A free of wire here without a condition breaks that retry in two
   * separate ways. A free that leaves wire_len set crashes on the stale
   * pointer. A free that also zeroes wire_len and wire_sent makes
   * _async_retry_hop move a NULL wire with a wire_len of 0 into the retry
   * ctx. The retry then quietly sends ZERO bytes, because its own
   * `wire_sent < wire_len` check is false at once. Its connection sits
   * open until the peer times the read out and closes it.
   *
   * The second form is the harder one to see. It needs enough concurrent
   * connections for a write to a reused socket that the peer already
   * closed to succeed locally, before the read side notices. No data race
   * is involved, so ThreadSanitizer does not report it.
   *
   * The library never retries a fresh ctx that it did not reuse. See the
   * own reused-only check of _async_ctx_finish. This code therefore frees
   * the wire of such a ctx eagerly here. Only the reused case must keep it
   * alive.
   *
   * Three places then claim that buffer. The first is the transfer of
   * _async_retry_hop. The second is a successful _async_idle_pool_offer,
   * which always resets wire for the next hop. The third is the own
   * unconditional free of _async_ctx_free at teardown. */
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
 * This is the plain-HTTP counterpart of _async_tls_try_write. It writes as
 * much of ctx->wire[ctx->wire_sent..] as a raw non-blocking send() accepts
 * now.
 *
 * The library calls it once right after the connect of a fresh connection
 * completes. It calls it again from the WRITING branch of
 * _async_on_writable_impl, each time that the write interest fires.
 * _async_submit_hop deliberately does NOT make the first write attempt of a
 * reused connection synchronously. See the reused-connection comment of
 * that function for why.
 *
 * One function therefore holds all the handling of a partial write, an
 * EWOULDBLOCK and a hard failure. No call site repeats it.
 */
static void _async_plain_try_write(chttp_async_ctx_t *ctx) {
  if (atomic_load_explicit(&ctx->flip_watch, memory_order_relaxed))
    atomic_store_explicit(&ctx->flip_watch, CHTTP_FLIP_NONE,
                          memory_order_relaxed);
  /* See the identical comment of _async_tls_try_write. It explains why
   * this code stops at header_len, and not at wire_len, while it waits for
   * a "100 Continue". */
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
      /* See the identical check of _async_tls_try_write. */
      if (!awaiting_continue && (ctx->duplex || _async_input_pending(ctx))) {
        _async_upload_pause(ctx, true);
        return;
      }
      /* An EWOULDBLOCK from a plain send(2) always means "wait for
       * writable". But this registration is not always on the write
       * direction already. The read-direction dispatch that consumes a
       * "100 Continue" also enters this function. That dispatch is
       * _async_awaiting_continue_on_data.
       *
       * A registration that stays on read there waits for a readability
       * that the peer has no reason to produce. That stalls the hop until
       * its own deadline. Under the default request_timeout_us of 0, that
       * deadline never comes.
       *
       * ccol_event_loop_modify is idempotent for a direction that is
       * already in effect. This therefore costs the ordinary write
       * dispatch path nothing beyond the call itself. */
      if (!awaiting_continue)
        atomic_store(&ctx->flip_watch, CHTTP_FLIP_ON_INPUT);
      if (_async_reg_modify(ctx, ccol_select_write) != ccol_success) {
        _async_ctx_finish(ctx);
      }
      return;
    }
    /* See the identical branch of _async_tls_try_write. */
    if (n < 0 && (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN)) {
      _async_upload_pause(ctx, false);
      return;
    }
    _async_ctx_finish(ctx);
    return;
  }
  if (awaiting_continue) {
    /* See the identical branch of _async_tls_try_write for the full
     * reasoning. It covers the lifetime of the wire buffer. It also covers
     * why this code must compute and store continue_deadline BEFORE it
     * publishes ctx->state as CHTTP_ASYNC_AWAITING_CONTINUE, and not
     * after. */
    ccol_mutex_lock(ctx->deadline_lock);
    ctx->continue_deadline = _deadline_earlier(
        _deadline_make(CHTTP_100_CONTINUE_WAIT_US), ctx->overall_deadline);
    ccol_mutex_unlock(ctx->deadline_lock);
#ifdef RUNNING_UNIT_TESTS
    _continue_deadline_stored_for_tests();
#endif
    ctx->state = CHTTP_ASYNC_AWAITING_CONTINUE;
    if (_async_reg_modify(ctx, ccol_select_read) != ccol_success) {
      _async_ctx_finish(ctx);
    }
    return;
  }
  /* See the identical comment of _async_tls_try_write for the full
   * reasoning. A REUSED ctx must keep its wire buffer alive past a write
   * that looks successful. _async_retry_hop needs the original request
   * bytes to send again, when the read side then finds that the peer was
   * already dead. Only a fresh ctx, which the library never retries, frees
   * the buffer here eagerly. */
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
  /* See g_force_stray_write_arm_for_tests. This reproduces a
   * continue-timeout flip from the deadline sweep that lands after this
   * hop already armed its own read direction. It reproduces it at exactly
   * the instruction where the race produces it. The library consumes the
   * flag once for each arming. */
  if (atomic_exchange(&g_force_stray_write_arm_for_tests, false)) {
    _async_reg_modify(ctx, ccol_select_write);
  }
#endif
}

/* This is a forward declaration. The redirect-detection branches of
 * _async_on_readable below call this function. Its full definition comes
 * after _async_connect_task, which it needs to queue the next hop. */
static void _async_handle_redirect(chttp_async_ctx_t *ctx, bool reusable);

/*
 * This reads ctx->state and checks ctx->hop_completed again, both under
 * ctx->idle_lock. See the comment of that field for why a plain unlocked
 * read of state is not safe here.
 *
 * It returns CHTTP_ASYNC_DISPATCH_ABANDONED, and not "not idle", when
 * hop_completed became true in a specific window. The entry check of
 * the caller opens that window, at the very top of _async_on_readable,
 * _async_on_writable or _async_on_error. The lock that this function takes
 * closes it. One of the application-thread failure paths of
 * _async_submit_hop or _async_submit_hop_fail can race this exact dispatch.
 * See the field comment of pending_app_teardown. Such a path sets
 * hop_completed under the same idle_lock that this function takes.
 *
 * The caller must return at once in that case. It does that through
 * _async_ctx_handle_if_abandoned below, and it never touches ctx directly
 * itself. A fall-through to the processing of an active hop reads the
 * stale state, chain and pctx of an abandoned ctx as a live hop. It can
 * then deliver data to the wrong chain.
 *
 * Note that the first mechanism, the staleness eviction of
 * _async_idle_pool_take, never reaches here at all. That eviction touches
 * neither state nor hop_completed for a stale candidate. It only calls
 * shutdown() on the fd of that candidate. Such a candidate is therefore
 * always still legitimately IDLE from the point of view of this function.
 * Its real teardown runs through the separate _async_idle_ctx_finish path
 * below, and not through this ABANDONED case. */
typedef enum {
  CHTTP_ASYNC_DISPATCH_ACTIVE,
  CHTTP_ASYNC_DISPATCH_IDLE,
  CHTTP_ASYNC_DISPATCH_ABANDONED,
} chttp_async_dispatch_kind_t;

static chttp_async_dispatch_kind_t _async_dispatch_kind(
    chttp_async_ctx_t *ctx) {
  ccol_mutex_lock(ctx->idle_lock);
  bool completed = ctx->hop_completed;
  bool idle = (ctx->state == CHTTP_ASYNC_IDLE);
  ccol_mutex_unlock(ctx->idle_lock);
  if (completed) return CHTTP_ASYNC_DISPATCH_ABANDONED;
  return idle ? CHTTP_ASYNC_DISPATCH_IDLE : CHTTP_ASYNC_DISPATCH_ACTIVE;
}

/*
 * Every dispatch-callback code path that finds ctx->hop_completed already
 * true calls this function. Such a path finds it in one of two ways. The
 * first is a plain fast check at the top of a function.
 * _async_on_writable_impl has one, and so do _async_on_readable_impl and
 * _async_on_error_impl, BEFORE they even call _async_dispatch_kind. The
 * second is the ABANDONED result of _async_dispatch_kind itself, which
 * covers the narrower race window between that top check and the re-check
 * that idle_lock guards.
 *
 * A true hop_completed has two possible origins. In the first, an earlier
 * dispatch for this exact registration already ran the real teardown
 * itself, before it set hop_completed. There is nothing further to do. In
 * the second, an application-thread failure path of _async_submit_hop or
 * _async_submit_hop_fail set hop_completed AND pending_app_teardown. That
 * path then shut ctx->fd down and returned, and it never touched ctx again.
 * It deliberately deferred the real destructive teardown to whichever
 * dispatch notices. See the field comment of pending_app_teardown for
 * the full reasoning.
 *
 * This function tells the two apart with pending_app_teardown. It calls
 * _async_ctx_teardown only for the second case. It does NOT call
 * _async_ctx_finish, because hop_completed is already true. Any legitimate
 * retry already went to _async_retry_hop before anything set
 * pending_app_teardown. A second run of the retry check of
 * _async_ctx_finish here is therefore wrong.
 *
 * Every caller must return at once afterwards, whichever case applies. The
 * library can free ctx fully by the time that this function returns.
 *
 * The top checks in _async_on_readable_impl and _async_on_error_impl are
 * genuinely reachable for the second case. They are not defence in depth.
 * The shutdown(fd, SHUT_RDWR) of the second mechanism depends on the
 * kernel. The kernel delivers an EPOLLIN or EPOLLERR condition on this fd,
 * which is still registered for read. That condition dispatches to exactly
 * one of those two functions. Their top check runs before anything reaches
 * _async_dispatch_kind.
 *
 * A missing call at either of those two checks makes the second mechanism
 * quietly stop working. ctx leaks permanently, and the chain reference that
 * it holds leaks with it. chttpclient_destroy then hangs forever while it
 * waits for async_in_flight_count to reach zero.
 *
 * The top check of _async_on_writable_impl is not reachable for the
 * second case today. ccol_event_loop_modify never changes the direction of
 * a registration on any failure path that it can return through. A reused
 * ctx whose reactivation failed therefore stays registered for read, and
 * never for write. That check gets the identical treatment anyway, so that
 * correctness does not depend on this staying true forever. This matches
 * the hop_completed check of this same function. This file documents
 * that check as structurally impossible to fire for the read-only case, and
 * the code keeps it anyway.
 *
 * This can never fire twice, inside one dispatch call or across separate
 * ones. Inside one call: whichever of the two checks in a function sees
 * hop_completed true first calls this and returns at once. The second check
 * in that same function is therefore structurally unreachable after the
 * first fires.
 *
 * Across separate calls: _ccol_event_loop_poller_collect in cthreadcomm.c
 * refuses to submit a second job for an entry while entry->refcount is
 * above 0. _async_ctx_teardown reaches _async_ctx_destroy_now, which calls
 * ccol_event_loop_remove before it returns, and that sets reg->removed to
 * true. No later poller pass then builds another job item that references
 * this reg. entry->dispatch_lock also serialises the callbacks of one
 * entry. A dispatch can therefore reach _async_ctx_teardown at most once
 * for each ctx.
 */
static void _async_ctx_handle_if_abandoned(chttp_async_ctx_t *ctx) {
  if (ctx->pending_app_teardown) _async_ctx_teardown(ctx);
}

/* Publishes reg, the handle that ccol_event_loop passes to every dispatch of
 * this ctx, into ctx->reg when ctx->reg is still CCOL_EVENT_REG_INVALID. A
 * dispatch can run before the `ctx->reg = ccol_event_loop_add(...)`
 * assignment of _async_connect_task finishes, and every path below reads
 * ctx->reg to modify or remove the registration. ctx has exactly one
 * registration for its whole life, so the value adopted here and the value
 * that ccol_event_loop_add returns are the same handle. The compare and
 * exchange leaves a field that is already set untouched. */
static inline void _async_adopt_reg(chttp_async_ctx_t *ctx,
                                    ccol_event_reg reg) {
  ccol_event_reg expected = CCOL_EVENT_REG_INVALID;
  atomic_compare_exchange_strong(&ctx->reg, &expected, reg);
}

/* This is a forward declaration. _async_on_writable_impl below hands work
 * to this function for a TLS ctx whose pending READ asked for the write
 * direction. The write-direction dispatch that the flip produces then
 * drives the read that it was flipped for. The full definition comes later
 * in this file, next to the rest of the read path. */
static void _async_on_readable_impl(ccol_event_loop loop, ccol_event_reg reg,
                                    ccol_selectable *sel,
                                    chttp_async_ctx_t *ctx);

/*
 * This answers, once for each hop on a connection from the idle pool and
 * before the first byte of the request goes out, whether the connection
 * still carries nothing. A pooled connection is idle only while nothing
 * arrives on it. Input that arrives after a complete response answers no
 * request, and the end of the stream means that the peer is gone. Either
 * one evicts the ctx through the IDLE dispatch of _async_on_readable_impl,
 * but _async_idle_pool_take can win the race against that dispatch, and the
 * new hop would then read those bytes as the start of its own response.
 *
 * The check runs on a dispatch of this registration, which ccol_event_loop
 * serialises against every other one, so it may read the TLS layer:
 * ctls_conn_read lets OpenSSL absorb a record that carries no application
 * data, such as a TLS 1.3 NewSessionTicket, which a raw peek would
 * mistake for input. A plain socket is peeked with MSG_PEEK. It also reads
 * ctx->idle_tainted, which the IDLE dispatch sets when its own read took
 * the input. This is the same check that _idle_pool_take of Tier 1 makes
 * at take time.
 *
 * Nothing of the request has gone out when this runs, so a connection that
 * fails it is retried on a fresh one through the retry-once path of
 * _async_ctx_finish, exactly as a reused connection whose peer closed it.
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

/* This starts or goes on with the send of a ctx in CHTTP_ASYNC_WRITING. It
 * is the last action of its caller: it can end the hop and free ctx. */
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

static void _async_on_writable_impl(ccol_event_loop loop, ccol_event_reg reg,
                                    ccol_selectable *sel,
                                    chttp_async_ctx_t *ctx) {
  (void)loop;
  (void)sel;
  /* See the field comment of ctx->hop_completed. A stray dispatch can
   * still arrive after this ctx reaches a terminal outcome. It must not
   * run any of the state-transition logic below a second time.
   *
   * The registration of an idle pooled ctx carries the read direction. See
   * the "ASYNC IDLE POOL" section. A write-direction dispatch for such a
   * ctx therefore only ever arrives as the stray that a continue-timeout
   * flip of the deadline sweep leaves behind. The fall-through at the
   * bottom of this function handles that, and not an idle check here.
   *
   * See the comment of _async_ctx_handle_if_abandoned for why this
   * call is still needed here, although nothing reaches this path today. */
  if (ctx->hop_completed) {
    _async_ctx_handle_if_abandoned(ctx);
    return;
  }
  /* See the field comment of ctx->reg and _async_adopt_reg. A dispatch for
   * the very first registration of this ctx, in the write direction, can
   * arrive before the `ctx->reg = ccol_event_loop_add(...)` assignment of
   * _async_connect_task finishes. A loopback connect is often writable the
   * moment that the library registers it. */
  _async_adopt_reg(ctx, reg);

  if (ctx->state == CHTTP_ASYNC_CONNECTING) {
    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    if (getsockopt(ctx->fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0 ||
        soerr != 0) {
      /* This code checks ctx->timed_out first. That field is _Atomic, so a
       * plain read is already free of a race. The
       * shutdown(fd, SHUT_RDWR) call that the deadline sweep makes against
       * a stuck connect appears here as a failure of getsockopt or
       * SO_ERROR. Without this check, the library reports an expired
       * connect_timeout_us as ccol_http_connection_failed. It should
       * report ccol_timed_out, which chttpclient.h documents. */
      _async_connect_failed(ctx);
      return;
    }
    /* The connect is up, so no later candidate is needed. The list would
     * otherwise stay with a pooled connection for its whole idle life. */
    _ccol_mem_free(ctx->mp, ctx->addrs);
    ctx->addrs = NULL;
    if (!ctx->is_unix) _apply_tcp_nodelay(ctx->fd);

    if (ctx->is_https) {
      /* A configured cert, key or CA path can be unreadable. The library
       * defers that failure all the way to here, and it does not fail at
       * the chttpclient_set_tls call. This matches the comment of
       * _rebuild_tls_ctx_locked in Tier 1.
       *
       * _chttp_do_async_internal checks that deferred failure
       * synchronously, through tls_ctx_usable, before it opens any
       * connection. Code that reaches here therefore knows that TLS is
       * genuinely usable. */
#ifdef RUNNING_UNIT_TESTS
      /* Every connection whose handshake runs here is a fresh one, because
       * a reused ctx skips CONNECTING and TLS_HANDSHAKING. The library
       * therefore establishes it under chain->tls_ctx. It must already
       * carry the generation of that same chain.
       *
       * A ctx that reaches here with any other value goes back to the idle
       * pool with a label for a configuration that its handshake never
       * used. _async_idle_pool_take then hands it to a later request on
       * the strength of that label.
       *
       * This check reads two fields that this dispatch already owns. The
       * library writes chain->tls_generation once, at the creation of the
       * chain, and never again. The check therefore holds no state of its
       * own and cannot race. */
      if (ctx->tls_generation != ctx->chain->tls_generation) {
        ccol_fatal_err(
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
    /* This falls through to the WRITING branch below. That branch tries
     * the first write at once, because the fd is writable right now. The
     * code does not wait for a separate on_writable dispatch. */
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
    /* Only one thing reaches this branch: the continue_deadline expiry
     * check of the deadline sweep. That check flips this registration to
     * the write direction to deliver this dispatch. See the "ASYNC
     * DEADLINE SWEEP" section. While the hop waits for a continue, this
     * library never has anything else to flush on this direction. The
     * ordinary WRITING state above is different.
     *
     * Claim the decision first, exactly like every exit from
     * _async_awaiting_continue_on_data. A genuine "100 Continue" can
     * arrive, and a racing on_readable dispatch can claim it just before
     * this one runs. The per-entry dispatch_lock of ccol_event_loop
     * orders the two, and it does not decide which one wins. That dispatch
     * then already took over the send of the body, and this one has
     * nothing left to do. */
    if (atomic_load(&ctx->continue_msg_started)) {
      /* A message started inside the window, and the parser holds its
       * start. The window bounds only the wait for a first byte, so the
       * decision belongs to that message: overall_deadline bounds the rest
       * of it, and the sweep enforces that deadline. Give the registration
       * back to the read direction and leave the decision unclaimed. */
      if (_async_reg_modify(ctx, ccol_select_read) != ccol_success)
        _async_ctx_finish(ctx);
      return;
    }
    /* The window of this hop decides, and nothing else does. The sweep
     * judges a snapshot that it took earlier, and on a pooled ctx that
     * snapshot can belong to an earlier hop whose window had ended, while
     * this hop has just started its own. This dispatch runs on the thread
     * that owns the hop, so it reads the deadline of the hop itself. A
     * window that is still open gives the registration back to the read
     * direction, and a flip that came too early costs this one dispatch. */
    {
      ccol_mutex_lock(ctx->deadline_lock);
      chttp_deadline_t own_dl = ctx->continue_deadline;
      ccol_mutex_unlock(ctx->deadline_lock);
      int own_ms;
      if (_deadline_remaining_ms(&own_dl, &own_ms)) {
        if (_async_reg_modify(ctx, ccol_select_read) != ccol_success)
          _async_ctx_finish(ctx);
        return;
      }
    }
    if (atomic_exchange(&ctx->continue_decided, true)) return;
    /* The wait timed out with no answer either way. Send the body anyway.
     * This matches the CURLOPT_EXPECT_100_TIMEOUT_MS behaviour of
     * curl. It also matches the identical timeout branch of
     * _chttp_send_and_read in Tier 1.
     *
     * ctx->retry_unsafe deliberately stays false here. See the field
     * comment of that field. A confirmed "100 Continue" is different: a
     * bare timeout gives no evidence that the peer is even still alive.
     * The usual retry-once safety net for a reused connection therefore
     * still applies. It applies when this write, or the read after it,
     * finds that the connection was already dead. */
    /* See the field comment of ctx->interim_responses_seen. The
     * identical timeout branch of Tier 1 hands the read of the final
     * response to a fresh _chttp_read_message_loop call. That call has its
     * own independent counter, whatever ended the continue wait.
     *
     * This code must therefore reset the counter here too, and not only on
     * a confirmed "100 Continue". Without this reset, a hop that discarded
     * interim responses during the wait starts the read of its final
     * response with less than the documented budget of 64 responses. */
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
     * write direction, because there is room to write. See
     * _async_upload_pause. */
    _async_upload_resume(ctx);
    return;
  }
  if (ctx->state == CHTTP_ASYNC_READING && ctx->tls) {
    /* A TLS ctx that waits for its response holds write interest for one
     * reason only, apart from a paused send. The EWOULDBLOCK branch of
     * _async_on_readable_impl flipped it there, because ctls_conn_read had
     * to write before it could decrypt another record.
     *
     * Restore the read direction first. A read that now succeeds therefore
     * leaves this registration in its home direction. Then drive the read
     * that this dispatch was flipped for. A read that blocks on the write
     * direction again flips it back itself. */
    if (_async_reg_modify(ctx, ccol_select_read) != ccol_success) {
      _async_ctx_finish(ctx);
      return;
    }
    _async_on_readable_impl(loop, reg, sel, ctx);
    return;
  }

  /*
   * Every state that reaches here has nothing of this library left to
   * flush on the write direction. Those states are CHTTP_ASYNC_READING on
   * a plaintext connection, and CHTTP_ASYNC_IDLE, whose home direction is
   * read. The code above drives the TLS counterpart of the first one.
   *
   * A stray write-direction dispatch can reach both states. The
   * continue-timeout flip of the deadline sweep samples ctx->state and
   * then issues its ccol_event_loop_modify. In between, a dispatch on
   * another thread can move that same ctx out of
   * CHTTP_ASYNC_AWAITING_CONTINUE. The flip of the sweep can therefore
   * land after the modify of that dispatch.
   *
   * A restore of the read direction here bounds that to a single wasted
   * dispatch. Without it, the registration stays armed for write with
   * nothing to write. A connected socket is almost always writable, so the
   * shared process-wide client reactor dispatches this no-op again and
   * again. That burns one of its threads and degrades every other
   * connection on the engine.
   *
   * The hop itself can also never progress. Its read interest is gone, and
   * nothing else issues another modify. At the default
   * request_timeout_us of 0 there is no deadline backstop to end it. That
   * leaves chttpclient_async_result_get and chttpclient_destroy blocked
   * forever.
   *
   * Both the read of the state and the modify happen under ctx->idle_lock.
   * _async_idle_pool_take holds that lock across the whole reactivation of
   * a pooled ctx. See its own doc comment. A reactivation can arm the write
   * direction for a hop that genuinely has bytes to send. That hold is what
   * stops this restore from landing on top of it.
   */
  ccol_mutex_lock(ctx->idle_lock);
  chttp_async_state_t resting_state = ctx->state;
  bool restore = (resting_state == CHTTP_ASYNC_READING ||
                  resting_state == CHTTP_ASYNC_IDLE);
  ccol_retval_t mrv = restore
                          ? ccol_event_loop_modify(cli_engine_bundler.reactor,
                                                   ctx->reg, ccol_select_read)
                          : ccol_success;
  ccol_mutex_unlock(ctx->idle_lock);
#ifdef RUNNING_UNIT_TESTS
  if (restore)
    atomic_fetch_add_explicit(&g_stray_write_arm_restores_for_tests, 1,
                              memory_order_relaxed);
#endif
  /* An idle ctx whose registration no longer accepts a modify is already
   * unregistered. It can therefore not spin. The staleness handling of
   * the idle pool takes it. This stray dispatch does not tear it down. */
  if (mrv != ccol_success && resting_state == CHTTP_ASYNC_READING)
    _async_ctx_finish(ctx);
}

/*
 * ccol_event_loop has its own public callback shape.
 * ccol_event_writable_fn, ccol_event_readable_fn and ccol_event_error_fn
 * all take a `void *arg`, which is the value that the caller handed to
 * ccol_event_loop_add. These thin wrappers exist only to cast `arg` back to
 * a chttp_async_ctx_t*. They then hand the work to the real logic in
 * _async_on_writable_impl, _async_on_readable_impl and
 * _async_on_error_impl below.
 *
 * These wrappers deliberately do no pin and no refcount of any kind. An
 * atomic refcount for each ctx, pinned at this point, cannot close the
 * use-after-free below it. The very first read of that refcount by the pin
 * races a concurrent free in the same way. See the field comment of
 * pending_app_teardown for the full reasoning.
 *
 * Safety rests entirely on one rule: no application thread ever frees a
 * registered ctx. Only a dispatch callback itself can ever call the real
 * destructive teardown for a ctx that these wrappers dispatch for.
 * ccol_event_loop then guarantees that this happens at most once for each
 * ctx, with no other thread able to touch that ctx at the same time. Its
 * own entry->dispatch_lock and its entry->refcount invariant of one job in
 * flight for each entry give that guarantee. See cthreadcomm.c.
 */
static void _async_on_writable(ccol_event_loop loop, ccol_event_reg reg,
                               ccol_selectable *sel, void *arg) {
  _async_on_writable_impl(loop, reg, sel, (chttp_async_ctx_t *)arg);
}

/*
 * This parses data[0..data_len) against ctx->parser. It treats those bytes
 * as one or more messages in the CHTTP_ASYNC_READING state, exactly as an
 * ordinary on_readable dispatch does.
 *
 * Two kinds of caller share it. The first is the READING-state read of
 * _async_on_readable_impl, which is the ordinary case with data fresh off
 * the socket. The second is the continue_carry replay of
 * _async_upload_pause. Those bytes came off the socket in the same read as
 * a "100 Continue", during the AWAITING_CONTINUE wait, and the send of the
 * body waits for this function to decide on them.
 *
 * Both callers hand this function a message boundary that ctx->parser never
 * saw before. Its own loop and reset behaviour therefore needs no
 * difference between the two origins that a caller can see.
 *
 * The loop, in place of one chttp1_parser_execute call, is what lets this
 * function discard an interim 1xx informational response and keep parsing.
 * That next message can still sit inside THIS SAME chunk, when a fast
 * server already wrote it into the same buffer. Without the loop, the
 * function reads the first 1xx that it sees as the final response.
 *
 * By the time that this function runs, the library already made every
 * decision about "Expect: 100-continue". _async_awaiting_continue_on_data
 * owns that decision, and nothing reaches it from here. This function
 * therefore discards every 1xx that it sees without a condition. Only a
 * final response of 200 or above ends the loop, and a redirect is always
 * 200 or above too. This matches the general-path handling of
 * _chttp_read_message_loop in Tier 1.
 *
 * `data` and `data_len` walk forward across iterations. The function feeds
 * the trailing bytes of a discarded message to a freshly initialized
 * ctx->parser. The contract of chttp1_parser_execute forbids more bytes
 * to an instance that is already CHTTP1_PAUSED. Each later message
 * therefore needs a fresh instance. This matches how _async_submit_hop and
 * _async_retry_hop set up the ctx->parser of a brand-new hop. It also
 * matches the "one fresh parser for each message" convention of Tier 1.
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
      /* The message is complete. The parser pauses right after it on
       * purpose. This matches the CHTTP1_PAUSED handling of Tier 1. */
      size_t consumed = chttp1_parser_consumed(&ctx->parser);
      bool trailing_this_msg = consumed < data_len;

      if (ctx->pctx.status_code >= 100 && ctx->pctx.status_code < 200) {
        /* This is an interim informational response ahead of the real
         * response. A "103 Early Hints" from RFC 8297 is one example.
         * Discard it and keep the read going on this same connection. This
         * matches the general-path handling of
         * _chttp_read_message_loop in Tier 1.
         *
         * This is never terminal. ctx->hop_completed stays false. Neither
         * _async_handle_redirect nor a fulfil or a teardown runs here.
         *
         * A cap bounds this, exactly like the identical loop of Tier 1.
         * See the comment of CHTTP_MAX_INTERIM_RESPONSES. Without the
         * cap, a server that never stops sending interim responses keeps
         * this ctx alive forever. It also keeps the chain and the future
         * that wait on that ctx alive. */
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
          /* An interim response does not answer the request. A send that
           * it paused goes on; otherwise, wait for more bytes through
           * on_readable. */
          if (ctx->upload_paused) _async_upload_resume(ctx);
          return;
        }
        data += consumed;
        data_len -= consumed;
        continue; /* Parse the bytes that are left as the next message. */
      }

      /* This is a final response of 200 or above, or a redirect. A
       * redirect is a 3xx, so it is always 200 or above. */
      ctx->hop_completed = true;
      if (trailing_this_msg) ctx->pctx.trailing_garbage = true;
      /* A connection whose request did not go out in full is never reused;
       * see _async_upload_pause. */
      bool keep_alive = chttp1_should_keep_alive(&ctx->parser) &&
                        !ctx->pctx.trailing_garbage && _async_request_sent(ctx);

      if (ctx->pctx.will_redirect) {
        _async_handle_redirect(ctx, keep_alive); /* This closes the
                                                  * connection, or pools
                                                  * it. */
      } else {
        /* Copy chain out and build the response BEFORE the
         * _async_finish_connection call.
         *
         * The copy takes a temporary extra retain. See the identical
         * retain in _async_handle_redirect for why. _async_finish_
         * connection below can start a concurrent teardown of the own
         * chain reference of THIS ctx on another reactor thread. That
         * teardown can free chain before the _async_fulfill_chain call
         * below runs, when nothing else holds it.
         *
         * When keep_alive is true, that call can also offer ctx to the
         * idle pool successfully. _async_idle_pool_offer then sets
         * ctx->chain to NULL, and it resets ctx->pctx and ctx->bb for
         * reuse. That is a normal and expected successful outcome. This
         * code must therefore copy ctx->chain out, and extract ctx->pctx
         * and ctx->bb, first. A NULL ctx->chain otherwise crashes the
         * fulfil below, or the code builds the response from fields that
         * the offer already cleared.
         *
         * The finish of the connection before the fulfil matters on its
         * own terms too. A fulfil can unblock the caller at once. A
         * ctpool_future_get on another thread is one such caller. That
         * caller can then destroy cli. The idle-pool-offer path of
         * _async_finish_connection touches cli->lock afterwards, and that
         * is a race with a use-after-free. */
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
    /* The message is not complete yet. Wait for another on_readable. A send
     * that the start of this message paused goes on for a final response of
     * 200 to 299, whose server may stream it while it reads the request,
     * and ends for good for one of 300 or above; see _async_upload_pause. */
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
 * This handles data[0..data_len) that arrives while ctx->state is
 * CHTTP_ASYNC_AWAITING_CONTINUE. The library already sent the header block,
 * and it deliberately holds the body back until the server answers.
 *
 * This matches the post-header wait of _chttp_send_and_read in Tier 1
 * exactly. See the doc comment of that function for the full contract with
 * three outcomes. This module uses event-driven dispatch instead.
 *
 * This function only ever runs from _async_on_readable_impl, after a
 * genuine byte arrives. It never runs from the timeout path that the
 * deadline sweep triggers. See the CHTTP_ASYNC_AWAITING_CONTINUE branch
 * of _async_on_writable_impl for that path.
 *
 * ctx->continue_decided is an _Atomic bool. It is the single guard that
 * decides which of this function and the timeout path gets to act. See the
 * own comment of that field for the full analysis of the race.
 *
 * Every exit from this function that would act on the parsed message claims
 * that guard first, with atomic_exchange. It backs off quietly when it
 * finds that something already made the decision. That happens when the
 * timeout path runs ahead of a response that arrives genuinely late.
 */
static void _async_awaiting_continue_on_data(chttp_async_ctx_t *ctx,
                                             const char *data,
                                             size_t data_len) {
  for (;;) {
    /* Every byte here belongs to a message. See the field comment of
     * continue_msg_started. */
    if (data_len > 0) atomic_store(&ctx->continue_msg_started, true);
    chttp1_errno_t err = chttp1_parser_execute(&ctx->parser, data, data_len);
    if (err == CHTTP1_PAUSED) {
      size_t consumed = chttp1_parser_consumed(&ctx->parser);
      bool trailing_this_msg = consumed < data_len;

      if (ctx->pctx.status_code == 100) {
        /* This is the expected outcome. The server confirms that it wants
         * the body. Claim the decision first. When the timeout path
         * already claimed it, there is nothing left to do. That path
         * already took over the send of the body, and this
         * "100 Continue" arrived too late to matter. */
        if (atomic_exchange(&ctx->continue_decided, true)) return;

        ccol_retval_t rrv = _parse_ctx_reset_for_continue(&ctx->pctx);
        if (rrv != ccol_success) {
          ctx->hop_completed = true;
          _async_fulfill(ctx, ccol_not_enough_memory, NULL);
          _async_ctx_finish(ctx);
          return;
        }
        _async_parser_start(ctx);
        /* The library now hands the body to a connection that the server
         * itself just confirmed is alive and ready to read from. See the
         * own field comment of ctx->retry_unsafe. That confirmation
         * disqualifies the usual retry-once safety net for a reused
         * connection, whatever happens next. */
        ctx->retry_unsafe = true;
        /* A genuine "100 Continue" is itself a message that the library
         * does not discard. The run of CONSECUTIVE discarded interim
         * responses that this field tracks therefore restarts here.
         *
         * See the field comment of ctx->interim_responses_seen.
         * Without this reset, interim responses that the library discarded
         * during THIS wait quietly eat into the budget of the read of
         * the final response. _chttp_send_and_read of Tier 1 is different:
         * it hands the two phases to separate _chttp_read_message_loop
         * calls with independent counters. */
        ctx->interim_responses_seen = 0;

        if (trailing_this_msg) {
          /* The same read holds the start of the next message after the
           * "100 Continue". The send of the body therefore starts paused,
           * exactly as when a message starts to arrive while the body goes
           * out: the read path parses those bytes first, and it lets the
           * send go on only for an interim response or a final one of 200
           * to 299 (_async_upload_may_resume). A final response of 300 or
           * above, or one that is complete, ends the send before a byte of
           * the body goes out, and the connection is then never reused
           * (_async_request_sent). Without the pause, the whole body goes
           * out after a final answer, and a server that answered and stopped
           * reading leaves the send waiting for room until the deadline.
           *
           * See the field comment of ctx->continue_carry, through which
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
        /* Some OTHER interim response arrives ahead of a "100 Continue"
         * or a direct final answer. A "103 Early Hints" is one example.
         * Discard it and keep the wait going inside the same
         * continue_deadline window. This matches
         * _chttp_read_message_loop(stop_at_status=100) of Tier 1, which
         * also discards everything that is not the awaited status. */
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
          /* The interim message ended on a boundary. The window runs on
           * for the first byte of the next message. */
          atomic_store(&ctx->continue_msg_started, false);
          return; /* Wait for more bytes through
                   * on_readable. */
        }
        data += consumed;
        data_len -= consumed;
        continue;
      }

      /* A final response of 200 or above arrived directly, with no
       * "100 Continue" before it. RFC 7231 SS5.1.1 fully permits a server
       * to reject a request this way and never want the body. This
       * response IS the final answer, and the library must never send the
       * body.
       *
       * Such a connection is never eligible for keep-alive, whatever
       * chttp1_should_keep_alive and the Connection header of the
       * response say. See the identical comment of _chttp_send_and_read,
       * the counterpart of this exact branch in Tier 1. The peer can still
       * expect the body that the library held back. A pool of that
       * connection therefore desynchronizes the next unrelated request
       * that reuses it. */
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
    /* The message is not complete yet. It can end in the middle of a
     * status line or a header line. Wait for more bytes through
     * on_readable, still inside continue_deadline. */
    return;
  }
}

static void _async_on_readable_impl(ccol_event_loop loop, ccol_event_reg reg,
                                    ccol_selectable *sel,
                                    chttp_async_ctx_t *ctx) {
  (void)loop;
  (void)sel;

  /* A previous dispatch for this same ctx already brought this hop to a
   * terminal outcome. That outcome is a fulfilled future, which includes a
   * TLS handshake failure that the library found mid-handshake. It can
   * also be a handoff to a redirect hop.
   *
   * A spurious extra on_readable can still arrive afterwards. Every route
   * of the mock server in the tests sends "Connection: close" and closes
   * its end right after it writes the response. The EOF of the peer can
   * therefore appear in a SEPARATE dispatch from the one that already
   * consumed the response bytes and reached CHTTP1_PAUSED.
   *
   * A second run of the completion logic is harmless for a plain fulfil,
   * because chain->fulfilled guards it. It is also harmless for a repeat
   * ctls_conn_handshake_step call on a handshake that already failed. That
   * call only returns another error, and this code ignores it.
   *
   * _async_handle_redirect is NOT idempotent. It queues another hop every
   * time that it runs. Without this guard, one hop can therefore fan out
   * into several redirect chains that share the same chain state. That
   * corrupts the refcount bookkeeping of that chain.
   *
   * This code checks the flag before the TLS_HANDSHAKING branch too. A
   * handshake failure that one dispatch finds must stop a second racing
   * dispatch from entering _async_tls_advance again.
   *
   * The per-registration dispatch_lock of ccol_event_loop already
   * serialises every dispatch for the single registration of this ctx
   * against itself. A plain bool is therefore enough here, and no other
   * lock is needed. */
  if (ctx->hop_completed) {
    /* See the comment of _async_ctx_handle_if_abandoned. This is the
     * PRIMARY reachable path that reaps a deferred teardown of the second
     * mechanism. See the field comment of pending_app_teardown. The
     * shutdown(fd, SHUT_RDWR) call dispatches to exactly this function, or
     * to _async_on_error_impl. It does that before anything reads
     * _async_dispatch_kind below. */
    _async_ctx_handle_if_abandoned(ctx);
    return;
  }
  /* See the field comment of ctx->reg and _async_adopt_reg. */
  _async_adopt_reg(ctx, reg);

  chttp_async_dispatch_kind_t kind = _async_dispatch_kind(ctx);
  if (kind == CHTTP_ASYNC_DISPATCH_ABANDONED) {
    /* A concurrent staleness eviction raced this exact dispatch. Any other
     * change of hop_completed can race it too. The window sits between the
     * hop_completed check above and the lock that _async_dispatch_kind
     * takes. See the comment of that function.
     *
     * This is the narrower race window that the second mechanism can also
     * be reaped through, next to the check at the top of this function.
     * _async_ctx_handle_if_abandoned does nothing when an earlier dispatch
     * already handled it. */
    _async_ctx_handle_if_abandoned(ctx);
    return;
  }
  if (kind == CHTTP_ASYNC_DISPATCH_IDLE) {
    /* N reactor threads all call epoll_wait on one shared epoll instance,
     * and there is no deduplication like EPOLLEXCLUSIVE. One underlying
     * readiness event can therefore legitimately produce more than one
     * sequential dispatch for the same fd.
     *
     * Take a response that arrives in two TCP segments. Two different
     * reactor threads can see the readiness of each segment in their own
     * epoll_wait calls, before either thread drains the socket. The first
     * dispatch then reads and processes the whole response. It moves this
     * ctx to IDLE and pools it. A second dispatch for the very same
     * original event still follows afterwards.
     *
     * A bare dispatch here is therefore NOT enough evidence that the
     * connection is dead, or that it carries genuinely unexpected data. A
     * real non-blocking read is what separates a stale readiness that
     * another thread already handled from real activity.
     *
     * Without this read, the library evicts a pooled connection moments
     * after it offers it. The test
     * async_idle_pool.sequential_requests_reuse_connection is not vacuous
     * here: it fails without this read. */
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
      /* Nothing is available. This was a stale or duplicate dispatch for
       * an event that another thread already handled fully. The connection
       * stays genuinely idle and pooled. There is nothing to do.
       *
       * This code deliberately does NOT read ctls_conn_wants_write, and
       * the read of an active hop does. ctls_conn_read can report
       * EWOULDBLOCK here while it wants the write direction.
       *
       * An idle pooled registration only ever carries the read direction.
       * _async_on_writable_impl depends on exactly that to know that an
       * on_writable dispatch is never for an idle ctx. A flip to write
       * here breaks that.
       *
       * There is nothing to drive on an idle connection in any case. This
       * probe exists only to tell a stale dispatch from a genuinely dead
       * peer. A connection whose TLS layer is mid-renegotiation simply
       * stays pooled. The next request drives it, or
       * CHTTP_IDLE_MAX_AGE_MS retires it. */
      return;
    }
    /* The connection is genuinely no longer safe to reuse. A pn of 0 means
     * that the peer closed it. A pn above 0 means unexpected data. A hard
     * error means the same thing. A take for a new hop can already have
     * removed this ctx from the pool, and the eviction below then finds
     * nothing to do; the mark tells that hop what this read found. */
    ctx->idle_tainted = true;
    _async_idle_ctx_finish(ctx);
    return;
  }

  if (ctx->state == CHTTP_ASYNC_TLS_HANDSHAKING) {
    _async_tls_advance(ctx);
    return;
  }

  if (ctx->state == CHTTP_ASYNC_WRITING) {
    /* A read-direction dispatch arrives while this hop still has request
     * bytes to send. The pending write is therefore what needs driving,
     * and not a read.
     *
     * Two things produce this state. ctls_conn_write can ask for the read
     * direction. See the EWOULDBLOCK branch of _async_tls_try_write.
     * Or the read dispatch that consumed a "100 Continue" just moved this
     * ctx back to CHTTP_ASYNC_WRITING, with the registration still on
     * read.
     *
     * A read here instead consumes response bytes that the request has not
     * finished asking for. It also leaves the send stalled, with nothing
     * left to drive it. */
    _async_drive_write(ctx);
    return;
  }

  /* The rule of _read_deadline_passed, which Tier 1 applies before each of
   * its reads. ctx->timed_out says that the sweep already found the
   * deadline passed. The own check covers the time before the next tick of
   * the sweep. chain->overall_deadline never changes for the lifetime of
   * the chain, and an active dispatch always has its chain, so the read
   * needs no lock. */
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

  /* One TLS record carries at most 16 KB of application data, so a buffer of
   * that size takes a whole record in one read. See the drain below for
   * input that still does not fit. */
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
  /* See the comment of g_force_async_hard_read_error_for_tests, above
   * the "ASYNC IDLE POOL" section. It says why this exists, and why it
   * only takes effect after ctx->any_bytes_read is already true. */
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
          /* An EWOULDBLOCK from ctls_conn_read() does not always mean
           * "wait for readable". OpenSSL can need to WRITE before this
           * exact call can make progress. A flush of a TLS 1.2
           * renegotiation is one such case. A TLS 1.3 KeyUpdate response
           * is another. See the doc comment of ctls_conn_wants_write.
           *
           * The reactor is level-triggered. A return that leaves this
           * registration pinned to the read direction therefore hands the
           * SHARED process-wide reactor thread an event. That thread can
           * make no progress on it. That spins the thread at 100 percent
           * CPU wherever the peer keeps the socket readable. Everywhere
           * else it stalls the hop until its own deadline, which under the
           * default request_timeout_us of 0 never comes.
           *
           * The write-direction dispatch that this code flips to restores
           * the read interest. It then re-enters here through the own
           * CHTTP_ASYNC_READING branch of _async_on_writable_impl.
           *
           * This code deliberately limits the flip to
           * CHTTP_ASYNC_READING. In CHTTP_ASYNC_AWAITING_CONTINUE, a
           * write-direction dispatch is the signal of the deadline
           * sweep. That signal says that the "100 Continue" wait is over,
           * and that the library should send the body. See the
           * "ASYNC DEADLINE SWEEP" section. A flip there is therefore
           * impossible to tell apart from that signal, and it decides the
           * wait early.
           *
           * continue_deadline bounds that wait in any case. Read interest
           * that stays in place therefore costs at most the rest of
           * CHTTP_100_CONTINUE_WAIT_US, and it never spins. The expiry
           * of the sweep then sends the body, which is exactly the
           * documented timeout outcome. */
          /* A paused send goes on when nothing holds it back, as in the
           * plaintext branch below. Its write also flushes whatever
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
        /* This is a genuine error at the TLS or transport level. It is an
         * SSL_ERROR_SYSCALL or an SSL_ERROR_SSL, which
         * _ctls_classify_io_result in ctls.c maps to an errno of
         * ECONNRESET. It is NOT a graceful close_notify. ctls_conn_read
         * reports THAT as n == 0, exactly like a plain socket EOF, in the
         * `else` branch below. This code must never fold it into `eof`.
         * See the handling of hard_error a few lines down for why. */
        hard_error = true;
      } else {
        eof = (n == 0);
      }
      /* Drain the input that the TLS layer already holds. epoll(7) sees
       * only the socket. Input that a read leaves inside the TLS layer
       * therefore produces no further dispatch, and the hop waits for bytes
       * that already arrived, until its deadline, or forever under the
       * default request_timeout_us of 0. The drain has to happen here,
       * before the bytes are processed, because processing can end the hop
       * and free ctx. A read that returns anything other than data ends the
       * drain. EWOULDBLOCK then means that the rest is the start of a record
       * that is not complete yet, and the socket signals its remainder. */
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
          /* Nothing more to read. A paused send goes on when nothing holds
           * it back; see _async_upload_may_resume. */
          if (ctx->state == CHTTP_ASYNC_READING &&
              _async_upload_may_resume(ctx))
            _async_upload_resume(ctx);
          return;
        }
        /* This is the same distinction as in the TLS branch above. A
         * recv() that returns -1 with a real errno, such as ECONNRESET or
         * ENOTCONN, is a hard transport error. It is never the orderly
         * close that a FIN drives, which recv() reports as n == 0. */
        hard_error = true;
      } else {
        eof = (n == 0);
      }
    }
  }
  if (n > 0) ctx->any_bytes_read = true;

  if (hard_error) {
    /* This branch deliberately never reaches chttp1_parser_finish below. A
     * real EOF is different: a hard error gives no guarantee that the peer
     * sent everything that it meant to.
     *
     * chttp1_parser_finish reports a complete and successful
     * EOF-delimited body whenever the parser already sits in
     * CHTTP1_ST_BODY_EOF. A response with neither Content-Length nor a
     * chunked Transfer-Encoding puts it there. The library would then
     * quietly truncate the response instead of a report of the failure.
     *
     * A connection RESET in the middle of a transfer must never look the
     * same as a peer that finishes normally. A fatal TLS alert must not
     * either.
     *
     * This matches _chttp_read_message of Tier 1, which returns
     * ccol_http_transfer_aborted at once for every read error other than
     * EWOULDBLOCK, and never calls chttp1_parser_finish. It also matches
     * the _async_on_error_impl sibling of this function, which treats a
     * genuine EPOLLERR dispatch in the same way. The classification of
     * a read result in this function must keep the difference between an
     * EOF and an error just as sharp.
     *
     * _async_ctx_finish itself checks reused and any_bytes_read, and it
     * retries when that is possible. This code therefore fulfils nothing
     * here. See the comment of that function. */
    _async_ctx_finish(ctx);
    return;
  }

  if (eof) {
    /* This matches the handling of n == 0 and EOF in
     * _chttp_read_message_with of Tier 1. A clean chttp1_parser_finish with a
     * complete message is valid. Some responses signal their end with a
     * close of the connection, and not with Content-Length or chunked
     * framing.
     *
     * A reused connection that produces this before any response byte
     * comes back is the retry-once scenario of Tier 1. This code checks
     * that BEFORE it marks hop_completed or fulfils the future. Nothing
     * has failed for the caller yet, and that is the whole point.
     *
     * An fe of CHTTP1_PAUSED, and not only of CHTTP1_OK, is the EXPECTED
     * outcome for a valid EOF-delimited body. See the doc comment of
     * chttp1_parser_finish, and the identical comment in
     * _chttp_read_message_with of Tier 1. */
    chttp1_errno_t fe = chttp1_parser_finish(&ctx->parser);
    bool ok = ((fe == CHTTP1_OK || fe == CHTTP1_PAUSED) &&
               ctx->pctx.message_complete);
    if (!ok) {
      /* _async_ctx_finish itself checks reused and any_bytes_read, and it
       * retries when that is possible. This code therefore fulfils nothing
       * here. See the comment of that function. */
      _async_ctx_finish(ctx);
      return;
    }
    if (ctx->state == CHTTP_ASYNC_AWAITING_CONTINUE) {
      /* An EOF-delimited final response arrives directly, with no
       * "100 Continue" before it. Such a response has no Content-Length
       * and no chunked framing.
       *
       * This is still the "the server answered directly, the library never
       * sent the body" outcome of Tier 1. See the identical comment of
       * _chttp_send_and_read. Only the framing differs: a close of the
       * connection signals the end, and not a length header.
       *
       * Claim the decision first, exactly like every other exit from
       * _async_awaiting_continue_on_data. Back off quietly when the
       * timeout path of the deadline sweep already claimed it.
       *
       * Such a connection is never reusable either way. This EOF path
       * never pools its connection. See the comment of
       * _async_fulfill_success. No extra override of keep_alive is
       * therefore needed here. */
      if (atomic_exchange(&ctx->continue_decided, true)) return;
    }
    ctx->hop_completed = true;
    if (ctx->pctx.will_redirect) {
      /* Framing that a close by the peer signals is never eligible for
       * keep-alive. Content-Length and chunked framing are different.
       * This matches _chttp_read_message_with of Tier 1, which pins
       * *keep_alive_out to false for this exact case. */
      _async_handle_redirect(ctx, false);
    } else {
      _async_fulfill_success(ctx);
      _async_ctx_finish(ctx);
    }
    return;
  }

  if (drain_timed_out) {
    /* The deadline passed between two reads of the drain. See
     * _read_deadline_passed. */
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
    /* The input that the TLS layer holds could not be taken, and nothing
     * would signal it again. The hop cannot go on. */
    _ccol_mem_free(ctx->mp, heap_data);
    if (ctx->state == CHTTP_ASYNC_AWAITING_CONTINUE &&
        atomic_exchange(&ctx->continue_decided, true))
      return;
    ctx->hop_completed = true;
    _async_fulfill(ctx, ccol_not_enough_memory, NULL);
    _async_ctx_finish(ctx);
    return;
  }

  /* The processing below can end the hop and wake a caller that then
   * destroys its client, and ctx->mp points into that client. The procs of
   * a heap buffer are therefore copied by value first. */
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
  /* See the identical comment of _async_on_writable above. */
  _async_on_readable_impl(loop, reg, sel, (chttp_async_ctx_t *)arg);
}

static void _async_on_error_impl(ccol_event_loop loop, ccol_event_reg reg,
                                 ccol_selectable *sel, chttp_async_ctx_t *ctx) {
  (void)loop;
  (void)sel;
  if (ctx->hop_completed) {
    /* See the comment of _async_ctx_handle_if_abandoned. This is the
     * PRIMARY reachable path that reaps a deferred teardown of the second
     * mechanism. See the field comment of pending_app_teardown. The
     * shutdown(fd, SHUT_RDWR) call dispatches to exactly this function, or
     * to _async_on_readable_impl. It does that before anything reads
     * _async_dispatch_kind below. */
    _async_ctx_handle_if_abandoned(ctx);
    return;
  }
  /* See the field comment of ctx->reg and _async_adopt_reg. The fd can
   * error out almost at once after the library registers it. */
  _async_adopt_reg(ctx, reg);
  chttp_async_dispatch_kind_t kind = _async_dispatch_kind(ctx);
  if (kind == CHTTP_ASYNC_DISPATCH_ABANDONED) {
    /* See the identical check in _async_on_readable_impl. This is the
     * narrower race window that the second mechanism can also be reaped
     * through. */
    _async_ctx_handle_if_abandoned(ctx);
    return;
  }
  if (kind == CHTTP_ASYNC_DISPATCH_IDLE) {
    _async_idle_ctx_finish(ctx);
    return;
  }
  /* The write-direction dispatch policy of cthreadcomm.c always puts an
   * EPOLLERR or EPOLLHUP bit above EPOLLOUT. It computes is_error as
   * is_err, and is_writable as !is_err. The read direction is different,
   * because it has a carve-out around has_writer.
   *
   * A failed non-blocking connect() therefore dispatches HERE. That is the
   * ordinary case for any host that is not on loopback. The kernel reports
   * EPOLLOUT, EPOLLERR and EPOLLHUP together once the RST or the timeout
   * arrives. It never dispatches to the CHTTP_ASYNC_CONNECTING branch
   * of _async_on_writable_impl, which diagnoses the failure with
   * getsockopt(SO_ERROR).
   *
   * The same holds for a TLS handshake that an RST kills mid-handshake. It
   * never reaches the CTLS_HANDSHAKE_ERROR branch of _async_tls_advance
   * either, which diagnoses with ctls_conn_verify_result.
   *
   * Without the two checks below, both cases fall through to the generic
   * backstop ccol_http_transfer_aborted. chttpclient.h promises a specific
   * code once the request is really in flight. Those codes are
   * ccol_http_connection_failed, ccol_http_tls_handshake_failed and
   * ccol_http_tls_cert_verification_failed.
   *
   * A reused ctx never reaches either state. A pooled connection
   * re-enters at CHTTP_ASYNC_WRITING, and never at CONNECTING or
   * TLS_HANDSHAKING. A fulfil directly here, exactly like the sibling
   * diagnosis sites do, can therefore never suppress a legitimate retry of
   * a reused connection. */
  /* Both branches below check ctx->timed_out first. That field is _Atomic,
   * so a plain read is already free of a race.
   *
   * The deadline sweep makes a shutdown(fd, SHUT_RDWR) call against a stuck
   * connect or handshake. That call dispatches here exactly like a genuine
   * failure on the side of the peer. Without this check, the library
   * reports an expired connect_timeout_us or request_timeout_us as
   * ccol_http_connection_failed, ccol_http_tls_handshake_failed or
   * ccol_http_tls_cert_verification_failed. It should report
   * ccol_timed_out, which chttpclient.h documents. */
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
  /* This is an error at the fd level with no more specific diagnosis
   * available. Leave it to the logic of _async_ctx_finish, which
   * checks for a retry first and then falls back to its backstop. That is
   * exactly what the cases above do. A fulfil here instead would disable a
   * legitimate retry of a reused connection. */
  _async_ctx_finish(ctx);
}

static void _async_on_error(ccol_event_loop loop, ccol_event_reg reg,
                            ccol_selectable *sel, void *arg) {
  /* See the identical comment of _async_on_writable above. */
  _async_on_error_impl(loop, reg, sel, (chttp_async_ctx_t *)arg);
}

/*
 * ASYNC HAPPY EYEBALLS
 *
 * A TCP target with more than one address connects through a race of
 * connect attempts; see chttp_addr_list_t for the policy, which Tier 1
 * applies in _tcp_connect. In Tier 2 each attempt is a socket with a
 * registration of its own on the reactor, whose arg is a
 * chttp_connect_attempt_t and not the ctx. The ctx of the hop is not
 * registered while the race runs. The attempt that connects first hands its
 * fd to the ctx, which then registers it through _async_connect_register
 * exactly as a single connect does. The race closes every other attempt at
 * that moment. When every candidate failed, the attempt that failed last
 * reports the failure for the hop.
 *
 * The deadline sweep drives the two timed parts of the race through
 * _async_race_tick: the start of the next candidate once the attempts in
 * flight had CHTTP_CONNECT_ATTEMPT_DELAY_MS with no result, and the end of
 * every attempt once the connect deadline expired. A candidate therefore
 * starts between that delay and that delay plus one tick of the sweep
 * (CHTTP_DEADLINE_SWEEP_INTERVAL_MS) after the one before it, and at once
 * after a failure.
 *
 * race->lock guards every field of the race. The lock order is the mutex of
 * a deadline stripe, then race->lock, then the internal locks of the
 * reactor. No path holds race->lock while it takes a deadline stripe or a
 * lock of a ctx: the attempt that decides the race releases the lock before
 * it hands the result to the ctx.
 *
 * The race is reference counted. The ctx holds one reference, from the
 * start of the race until the ctx is freed, which is after the ctx left the
 * deadline registry, so the sweep never reads a race that is gone. Every
 * attempt registration holds one more, which its on_removed releases,
 * because a dispatch of a removed registration can still be in flight. The
 * race carries its own copy of the allocator procs: an on_removed can run
 * after the client that started the race is destroyed.
 */
typedef struct chttp_connect_race_s chttp_connect_race_t;

typedef struct {
  chttp_connect_race_t *race;
  int fd;             /* -1 when the attempt is not running. */
  ccol_event_reg reg; /* CCOL_EVENT_REG_INVALID when the attempt is not
                       * registered. */
} chttp_connect_attempt_t;

struct chttp_connect_race_s {
  ccol_mutex_t lock;
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
  ccol_mutex_lock(r->lock);
  bool last = (--r->refs == 0);
  ccol_mutex_unlock(r->lock);
  if (!last) return;
  ccol_memmgmt_procs_t procs = r->procs;
  ccol_memmgmt_procs_t *mp = r->has_procs ? &procs : NULL;
  ccol_mutex_destroy(r->lock);
  _ccol_mem_free(mp, r->addrs);
  _ccol_mem_free(mp, r);
}

static void _async_race_on_removed(void *arg) {
  _async_race_release(((chttp_connect_attempt_t *)arg)->race);
}

/* The caller holds r->lock. A registration is removed before its fd is
 * closed, so that the fd number cannot reach another socket while the
 * reactor still watches it. */
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

/* This starts the next candidate that accepts a connect. The caller holds
 * r->lock. A connect that completes at once is registered like a pending
 * one: its writable dispatch follows at once and takes the ordinary path of
 * a winner. A candidate whose socket, connect or registration fails at once
 * gives way to the one after it. */
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
    /* A dispatch of this registration waits for r->lock, which this thread
     * holds, so it always finds these fields set. */
    a->fd = fd;
    a->reg = reg;
    r->refs++;
    r->pending++;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    r->next_start = _timespec_add_ms(now, _connect_attempt_delay_ms());
    return;
  }
}

/* This decides the race for the attempt `winner`, or for none when winner
 * is NULL. The caller holds r->lock and releases it afterwards, and then
 * acts on the ctx that this returns. */
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
  ccol_mutex_lock(r->lock);
  if (r->decided || a->fd < 0) {
    /* A dispatch of an attempt that the race already closed. */
    ccol_mutex_unlock(r->lock);
    return;
  }
  int soerr = 0;
  socklen_t slen = sizeof(soerr);
  if (getsockopt(a->fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) == 0 &&
      soerr == 0) {
    int fd = a->fd;
    /* The fd moves to the ctx. Only the registration of the attempt goes. */
    ccol_event_loop_remove(cli_engine_bundler.reactor, a->reg);
    a->reg = CCOL_EVENT_REG_INVALID;
    a->fd = -1;
    r->pending--;
    chttp_async_ctx_t *ctx = _async_race_decide_locked(r, a);
    ccol_mutex_unlock(r->lock);
    _async_connect_register(ctx, fd);
    return;
  }
  _async_race_close_attempt_locked(r, a);
  chttp_async_ctx_t *ctx = r->ctx;
  /* A failure hands over to the next candidate at once, unless the connect
   * deadline already ended the race. */
  if (!ctx->timed_out) _async_race_launch_locked(r);
  if (r->pending > 0) {
    ccol_mutex_unlock(r->lock);
    return;
  }
  ctx = _async_race_decide_locked(r, NULL);
  ccol_mutex_unlock(r->lock);
  _async_fulfill(
      ctx, ctx->timed_out ? ccol_timed_out : ccol_http_connection_failed, NULL);
  _async_ctx_teardown(ctx);
}

/* The deadline sweep calls this for a ctx whose race runs, under the mutex
 * of the deadline stripe of that ctx. An expired connect deadline shuts
 * every attempt down, and each of them then fails through its own dispatch;
 * the last one reports ccol_timed_out, because the sweep marked the ctx
 * timed out first. Otherwise the next candidate starts once its delay has
 * passed. The start never decides the race here: it only registers an
 * attempt, so nothing on this path takes a lock of a ctx or of a stripe. */
static void _async_race_tick(chttp_connect_race_t *r, bool expired) {
  ccol_mutex_lock(r->lock);
  if (!r->decided) {
    if (expired) {
      for (size_t i = 0; i < r->addrs->count; i++)
        if (r->attempts[i].fd >= 0) shutdown(r->attempts[i].fd, SHUT_RDWR);
    } else if (r->next < r->addrs->count) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (_ms_until(&r->next_start, &now) == 0) _async_race_launch_locked(r);
    }
  }
  ccol_mutex_unlock(r->lock);
}

/* This starts the race for ctx over ctx->addrs, which the race takes over.
 * It runs on the worker of the connect task, before anything else can reach
 * the race. It returns false when the race could not be built; the caller
 * then reports the failure. */
static bool _async_race_start(chttp_async_ctx_t *ctx) {
  size_t n = ctx->addrs->count;
  chttp_connect_race_t *r = (chttp_connect_race_t *)_ccol_mem_calloc(
      ctx->mp, 1, sizeof(*r) + n * sizeof(r->attempts[0]));
  if (!r) return false;
  if (ccol_mutex_init(r->lock) != 0) {
    _ccol_mem_free(ctx->mp, r);
    return false;
  }
  if (ctx->mp) {
    r->procs = *ctx->mp;
    r->has_procs = true;
  }
  r->refs = 1; /* The reference of the ctx. */
  r->ctx = ctx;
  r->addrs = ctx->addrs;
  ctx->addrs = NULL;
  for (size_t i = 0; i < n; i++) {
    r->attempts[i].race = r;
    r->attempts[i].fd = -1;
  }
  ctx->state = CHTTP_ASYNC_CONNECTING;
  atomic_store(&ctx->race, r);

  ccol_mutex_lock(r->lock);
  _async_race_launch_locked(r);
  bool failed = (r->pending == 0);
  if (failed) (void)_async_race_decide_locked(r, NULL);
  ccol_mutex_unlock(r->lock);
  if (failed) {
    _async_fulfill(ctx, ccol_http_connection_failed, NULL);
    _async_ctx_teardown(ctx);
  }
  return true;
}

/* This lets go of the race of ctx when ctx is freed. The race is decided by
 * then on every path, and the check here only keeps that true: an attempt
 * never outlives the ctx that it connects for. */
static void _async_race_detach(chttp_async_ctx_t *ctx) {
  chttp_connect_race_t *r = atomic_load(&ctx->race);
  if (!r) return;
  atomic_store(&ctx->race, NULL);
  ccol_mutex_lock(r->lock);
  if (!r->decided) (void)_async_race_decide_locked(r, NULL);
  ccol_mutex_unlock(r->lock);
  _async_race_release(r);
}

/*
 * This runs on a worker of cli_engine_bundler.dns_pool. For TCP it resolves
 * the DNS name. For a unix target it builds the sockaddr_un. It then issues
 * ONE non-blocking connect(). Last, it registers the resulting fd with the
 * reactor for write readiness. It never blocks the worker while it waits
 * for the connect to complete.
 *
 * That division of labour is deliberate. It does a synchronous DNS
 * resolution plus one non-blocking connect attempt, on an offload thread. A
 * worker that blocks through the whole connect is worse. It ties up a
 * worker pool of a fixed size for the whole length of that connect. That
 * costs throughput under high concurrency. A registration for write
 * readiness instead lets one epoll instance track thousands of pending
 * connects cheaply.
 *
 * For a TCP target with one address, this function starts the connect on
 * that address and registers its fd. A target with more than one address
 * starts the Happy Eyeballs race of the "ASYNC HAPPY EYEBALLS" section
 * instead, which registers one fd for each attempt and hands the winner to
 * the ctx. The policy is the one of chttp_addr_list_t, which Tier 1 shares.
 * This function never touches the thread of the caller.
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
    ccol_retval_t rrv = _addr_list_resolve_for_target(ctx->mp, ctx->host,
                                                      ctx->port, &ctx->addrs);
    if (rrv != ccol_success) {
      _async_fulfill(ctx, rrv, NULL);
      _async_ctx_teardown(ctx);
      return;
    }
    _addr_list_interleave(ctx->addrs);
    if (ctx->addrs->count > 1) {
      /* See the "ASYNC HAPPY EYEBALLS" section. A connect deadline that
       * expired while this task sat in the queue of the pool ends the
       * connect before any attempt starts. */
      if (ctx->timed_out) {
        _async_fulfill(ctx, ccol_timed_out, NULL);
        _async_ctx_teardown(ctx);
        return;
      }
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
    /* This matches _tcp_connect of Tier 1, which stops at a pending
     * attempt once the connect deadline has passed. */
    int remaining_ms;
    if (in_progress &&
        !_deadline_remaining_ms(&ctx->connect_deadline, &remaining_ms))
      ctx->timed_out = true;
  }
  _async_connect_register(ctx, fd);
}

/* This hands the socket fd, whose connect is pending or complete, to ctx
 * and registers it with the reactor for write readiness. The connect task
 * calls it for a single connect, and the attempt that wins a Happy Eyeballs
 * race calls it from its own dispatch. */
static void _async_connect_register(chttp_async_ctx_t *ctx, int fd) {
  /* ctx->fd and ctx->state are _Atomic. See their own field comments.
   * These plain assignments are therefore already free of a race against
   * the deadline sweep. That sweep is an independent thread. This ctx has
   * been registered with it since before anything submitted the ctx here.
   * See _async_submit_hop. The sweep can run at any time, and that
   * includes concurrently with this exact assignment.
   *
   * This also covers a connect_timeout_us that expired while this task
   * merely sat in the queue of cli_engine_bundler.dns_pool. A saturated
   * pool produces that case. The sweep cannot shut down an fd that does
   * not exist yet. On that path it can only mark ctx->timed_out and wait.
   * The check below reads that mark, the moment that a real fd exists. */
  ctx->fd = fd;
  ctx->state = CHTTP_ASYNC_CONNECTING;

  if (ctx->timed_out) {
    /* Nothing registered this fd with the reactor, so nothing else can act
     * on it yet. Report the timeout here, and let _async_ctx_teardown
     * close the fd, through _async_ctx_destroy_now.
     *
     * A close by hand here, before _client_deadline_unregister runs,
     * breaks the "unregister before close" invariant of this module.
     * See the doc comment of _async_ctx_destroy_now. The deadline registry
     * already holds this ctx at this point, because the library registered
     * it before it submitted this task.
     *
     * _async_ctx_destroy_now always closes ctx->fd itself, AFTER the
     * unregister. That is the only order that keeps the invariant
     * intact. */
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
  /* This code holds idle_lock across the ccol_event_loop_add call and
   * across the read-back of ctx->reg right after it.
   *
   * The moment that ccol_event_loop_add makes this registration live, a
   * reactor thread is free to dispatch it. A loopback connect is very
   * often writable at once. That thread can then drive the whole hop to
   * completion and reach _async_ctx_free. It races the still in-flight
   * `ctx->reg = ...` write of this thread, and the `if (!ctx->reg)` read
   * right after it. That is a genuine write and read race on ctx->reg
   * itself. ThreadSanitizer reports it, and a read of the code does not.
   *
   * _async_ctx_free takes the same lock briefly, as its very first action.
   * That is enough to guarantee that this window is fully over before the
   * free can go on. By then ctx->reg is published, and this function has
   * stopped touching ctx.
   *
   * No dispatch path needs idle_lock to read ctx->reg itself, because
   * every dispatch first adopts the handle that ccol_event_loop passes to
   * it; see _async_adopt_reg. A dispatch that reaches a path which takes
   * idle_lock waits here only until ccol_event_loop_add returns, and
   * ccol_event_loop_add never waits for a dispatch. */
  ccol_mutex_lock(ctx->idle_lock);
  ctx->reg = ccol_event_loop_add(cli_engine_bundler.reactor,
                                 ccol_selectable_from_fd(fd, ccol_select_write),
                                 handlers, ctx, &err);
  bool reg_failed = !ctx->reg;
  ccol_mutex_unlock(ctx->idle_lock);
  if (reg_failed) {
    _async_fulfill(ctx, ccol_not_enough_memory, NULL);
    _async_ctx_teardown(ctx);
  }
}

/*
 * The library calls this when a REUSED connection turns out to be dead
 * before it reads any response byte. A write failure, an immediate EOF, or
 * an error that the reactor finds all lead here. This matches the identical
 * recovery of Tier 1 in chttp_do_internal: retry exactly once against a
 * brand-new connection, after the liveness probe fails.
 *
 * This function allocates a fresh ctx. It does not reuse the struct of
 * old_ctx in place for the new attempt. Such a reuse needs the dispatch
 * callbacks of old_ctx neutralised, to guard against a dispatch that is
 * already in flight for the OLD registration. Such a dispatch would
 * otherwise arrive and read fields of the ctx that now serve the NEW
 * attempt. That neutralisation is itself unsafe when some OTHER independent
 * teardown was already in flight at that moment.
 *
 * This function therefore moves out of old_ctx only what the retry needs.
 * That is the already serialized wire bytes, the host and port, the
 * origin_key, the empty map of response headers, and the metadata of the
 * hop. It sets those fields in old_ctx to NULL, so that the ordinary
 * teardown of old_ctx does not free them a second time. Its caller runs
 * that teardown, exactly as it would for an ordinary failure.
 *
 * This function leaves old_ctx otherwise untouched. old_ctx then goes
 * through its NORMAL teardown path. This function neither frees it nor
 * touches its chain reference. It only sets ctx->hop_completed. That flag
 * stops the on_data and on_close of old_ctx from triggering this a
 * second time. It is also true from the point of view of old_ctx, which
 * genuinely reached a terminal outcome.
 *
 * This function retains a SECOND chain reference for the new ctx. old_ctx
 * keeps its own until its own teardown frees it. The retain happens before
 * any possible release by old_ctx. The refcount of the chain therefore
 * never reaches zero too early during the handoff. This is the same
 * retain-before-release order that a redirect hop uses.
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

  /* ctx is a brand-new object here, and it is still private. Neither the
   * deadline registry nor ccol_event_loop holds it yet. No concurrent
   * reader can therefore race this write. This code still takes
   * deadline_lock, so that every write site looks the same. See the own
   * field comment of that lock. */
  ccol_mutex_lock(ctx->deadline_lock);
  ctx->overall_deadline = chain->overall_deadline;
  ccol_mutex_unlock(ctx->deadline_lock);
  ctx->chain = chain;
  ctx->cli = old_ctx->cli;
  ctx->hop = old_ctx->hop;
  ctx->cur_method = old_ctx->cur_method;
  ctx->is_https = old_ctx->is_https;
  ctx->verify_host = old_ctx->verify_host;
  /* The retry connects fresh, and ctx->reused is false below. Its
   * handshake runs under chain->tls_ctx, which is the configuration that
   * this chain pinned at its creation. The retry therefore carries the own
   * generation of the chain, and not the one that the dead connection used.
   *
   * A carry-forward of the value of old_ctx labels a connection with the
   * current configuration although it was established under a superseded
   * one. The eligibility check of _async_idle_pool_take then hands that
   * connection to a later request which believes that it runs on the new
   * policy. */
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
  /* The retry sends the exact same request again. It therefore carries
   * forward the exact same decision about want_100_continue and header_len
   * that old_ctx already made. This matches how the redirect machinery of
   * this file carries cur_method and cur_body forward elsewhere.
   *
   * Code that reaches this function at all proves that old_ctx never got
   * past the send of the header block. See the retry-eligibility check of
   * _async_ctx_finish, which needs !retry_unsafe. retry_unsafe only ever
   * becomes true after the library really sends the body, following a
   * confirmed "100 Continue". ctx->wire therefore still holds the
   * complete unsent body, ready to go.
   *
   * continue_decided starts as false again for this fresh attempt, exactly
   * like a brand-new hop. */
  ctx->want_100_continue = old_ctx->want_100_continue;
  ctx->header_len = old_ctx->header_len;
  atomic_store(&ctx->continue_decided, false);
  atomic_store(&ctx->continue_msg_started, false);

  ctx->pctx.mp = chain->mp;
  ctx->pctx.is_head_request = old_ctx->pctx.is_head_request;
  /* A streaming request sets chain->write_fn. It delivers body bytes
   * straight to the callback of the caller. A buffered request uses
   * ctx->bb instead. See the comment of _async_build_response for why
   * this choice must agree with what that function does later. */
  ctx->pctx.requested_sink_fn =
      chain->write_fn ? chain->write_fn : _sink_buffered;
  ctx->pctx.requested_sink_ctx = chain->write_fn ? chain->write_ctx : &ctx->bb;
  ctx->pctx.headers = old_ctx->pctx.headers; /* This map is empty. Nothing
                                              * ever parsed into it,
                                              * because a retry needs
                                              * !any_bytes_read. */
  old_ctx->pctx.headers = NULL;
  ctx->bb.mp = chain->mp;
  ctx->bb.max_size = chain->max_response_body_size;

  ctx->reused = false; /* The retry itself is a fresh connection. */
  ctx->any_bytes_read = false;
  /* A retry of a dead reused connection connects fresh, as above. It
   * therefore needs its own connect_deadline, exactly like the fresh path
   * of _async_submit_hop. The overall_deadline of the chain does not
   * change. It was never a per-hop value. */
  ctx->connect_deadline = _deadline_make(chain->connect_timeout_us);

  _async_parser_start(ctx);

  /* This registers the ctx BEFORE it submits the task. See the identical
   * comment of _async_submit_hop for why. Without that order, a worker can
   * run this hop to completion and free ctx before this thread registers
   * it. */
  _client_deadline_register(ctx);

  ccol_retval_t sr = ctpool_submit(cli_engine_bundler.dns_pool,
                                   _async_connect_task, ctx, NULL);
  if (sr != ccol_success) {
    _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
    _async_ctx_free(ctx); /* This unregisters ctx too. */
    _async_chain_release(chain);
  }
}

/*
 * This prepares one hop of a redirect chain and submits it. The library
 * uses it for the very first hop, from _chttp_do_async_internal. It also
 * uses it for every later redirect hop, from _async_handle_redirect.
 *
 * On success it retains one chain reference. The library frees that
 * reference when it tears the ctx of this hop down. On a failure this
 * function frees that retain again itself, so the refcount does not change
 * at all. It also fulfils the future of the chain with a specific error
 * code before it returns. A caller therefore needs no fallback logic of its
 * own that fulfils on a failure here.
 *
 * Only the generic backstop of _async_chain_release stays as a true last
 * resort. It covers paths that no more specific error can reach, such as a
 * connection that dies while it is already in flight.
 *
 * body_data, body_len and body_content_type describe the body of THIS hop.
 * They are usually chain->body_data, chain->body_len and
 * chain->body_content_type exactly as they are. They are all empty after a
 * redirect that does not preserve the method rewrites that method to GET.
 *
 * The caller passes them explicitly, and this function does not always read
 * them from the chain. The caller is the equivalent of the loop of Tier 1.
 * It is the one that knows, from the status code of the previous hop,
 * whether to keep them or to drop them.
 *
 * This returns true when it queues the hop successfully.
 */

/*
 * This handles a setup failure that happens AFTER the library already
 * popped a reused connection from the idle pool. A failed allocation of the
 * headers map is one such failure. A failed serialization of the request is
 * another, and so is a failed allocation of the origin_key.
 *
 * In that situation ctx->fd is a live registered connection, and not a
 * fresh ctx that never connected. ctx->idle_lock is also still held,
 * following the return contract of _async_idle_pool_take.
 *
 * This function reports the error and marks the ctx terminal. The ctx is
 * terminal in any case. The mark also stops the retry check of
 * _async_ctx_finish from queuing a pointless retry. This is a failed
 * allocation, and not a dead connection.
 *
 * This function runs on an ordinary application thread. That is whatever
 * thread called chttpclient_do_async, or a caller that a redirect drives.
 * The ccol_event_loop registration of the ctx is still fully live at this
 * point. A reactor dispatch callback for it can legitimately be in flight,
 * or about to run, on another thread at this exact moment.
 *
 * A direct call to _async_ctx_teardown here is therefore a use-after-free.
 * See the field comment of pending_app_teardown for the full reasoning.
 * This function is one of the two sites that the comment describes.
 *
 * This function does something else instead. It marks pending_app_teardown,
 * together with hop_completed, in that order. See the note in that field
 * comment: the order does not matter here, and the code keeps it consistent
 * anyway. It then calls shutdown() on the fd, which forces a genuine
 * EPOLLIN or EPOLLERR dispatch on this fd, which is still registered for
 * read. Only after that does it unlock idle_lock. Whichever dispatch
 * callback sees the mark runs the real destructive teardown safely, from a
 * dispatch context. See _async_ctx_handle_if_abandoned.
 *
 * The shutdown must come before that unlock. Once idle_lock is free, the
 * parked dispatch runs the teardown, and that teardown close()s ctx->fd. A
 * shutdown after that point names an fd number that the kernel is free to
 * give to an unrelated socket in this process. Nothing may touch ctx again
 * after the unlock of idle_lock.
 *
 * A fresh ctx that never connected is different. Nothing ever locked its
 * idle_lock, and it has no such attachment. ctx->reg is still NULL, so no
 * dispatch can be in flight for it. This function simply frees it directly,
 * exactly like every other failure path before a connect.
 */
static void _async_submit_hop_fail(chttp_async_ctx_t *ctx, ccol_retval_t rv) {
  chttp_async_chain_t *chain = ctx->chain;
  _async_fulfill_chain(chain, rv, NULL);
  if (ctx->reused) {
    ctx->pending_app_teardown = true;
    ctx->hop_completed = true;
    /* This runs while idle_lock is still held, and that is deliberate. A
     * reactor dispatch for this still registered fd can already be parked
     * inside _async_dispatch_kind, and it waits for exactly this lock. The
     * instant that this code unlocks it, that dispatch sees hop_completed
     * and runs the real teardown. That teardown close()s ctx->fd.
     *
     * A shutdown of the fd after that point acts on a bare int. The kernel
     * is free to have given that number to a completely unrelated socket
     * that any thread of this process opened in the meantime.
     *
     * A hold of the lock across the shutdown costs nothing and carries no
     * risk of reentrancy. shutdown() is a plain kernel-level operation
     * with no synchronous application callback. The shutdown of the
     * deadline sweep under a stripe mutex already depends on the same
     * property. */
#ifdef RUNNING_UNIT_TESTS
    atomic_store(&g_abandon_shutdown_under_lock_for_tests,
                 ctx->idle_lock_held_for_tests ? 1 : 0);
#endif
    if (ctx->fd >= 0) shutdown(ctx->fd, SHUT_RDWR);
#ifdef RUNNING_UNIT_TESTS
    ctx->idle_lock_held_for_tests = false;
#endif
    ccol_mutex_unlock(ctx->idle_lock);
    /* The _client_engine_release() call below frees the SEPARATE engine
     * reference that _async_idle_pool_offer took for this ctx while it sat
     * in the idle pool. _async_ctx_teardown only ever frees the chain
     * reference of the ctx, once the deferred dispatch finally runs it. It
     * knows nothing about this one.
     *
     * This is the fourth exit from the idle pool that must free this
     * reference. The other three are a successful reuse, below in
     * _async_submit_hop, a natural death of an idle connection, in
     * _async_idle_ctx_finish, and a staleness eviction, in
     * _async_idle_pool_take.
     *
     * Without this call, each setup failure on a reused connection leaks
     * one engine reference. Such a failure is a failed allocation or
     * serialization after the library already popped a pooled connection.
     *
     * This call is safe directly from this thread. It only ever touches a
     * global counter, and never ctx. */
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
    /* This matches the identical per-hop check of Tier 1. The configured
     * cert, key or CA path was not readable at the set_tls call, or it
     * failed to load or parse there. See the identical check of
     * chttp_do_internal for why ccol_http_tls_cert_load_failed is the
     * correct code here. */
    _url_free(chain->mp, &url);
    _async_fulfill_chain(chain, ccol_http_tls_cert_load_failed, NULL);
    _async_chain_release(chain);
    return false;
  }

  /* The library leaves off the credential headers and the Host header that
   * the caller set with chttp_request_set_header on every hop whose origin
   * or host differs from chain->initial_origin_key, which is the origin of
   * hop 0. See CHTTP_REDIRECT_DROP_CREDENTIALS. This matches the identical
   * initial_origin_key and redirect_drops locals of chttp_do_internal in
   * Tier 1 exactly.
   *
   * A change of chain->redirect_drops here needs no lock, for the same
   * reason as the changes to chain->carried_auth and
   * chain->carried_auth_origin below. See the next comment of this
   * function. */
  chain->redirect_drops =
      _redirect_header_drops(chain->initial_origin_key, url.origin_key);

  /* This carries an Authorization value forward that the library injected
   * from the userinfo of a URL. It matches the identical carried_auth and
   * carried_auth_origin locals of chttp_do_internal in Tier 1 exactly. It
   * carries the value on the same origin, and drops it permanently across
   * origins.
   *
   * A change of chain->carried_auth and chain->carried_auth_origin here
   * needs no lock. _async_submit_hop runs strictly one hop at a time for
   * each chain. The setup of one hop, this change included, always finishes
   * before anything submits the next hop. Every other unguarded change to a
   * chain field in the redirect machinery of this file already depends on
   * that same invariant. The decision about a method and body downgrade in
   * _async_handle_redirect is one example. */
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
     * CHTTP_ASYNC_TLS_HANDSHAKING. A reused one goes straight to WRITING
     * below. Nothing ever reads connect_deadline for such a connection.
     * See the "ASYNC DEADLINE SWEEP" section. */
    ctx->connect_deadline = _deadline_make(chain->connect_timeout_us);
  }
  /* In the reused case, _async_idle_pool_take already set ctx->chain,
   * ctx->overall_deadline and ctx->state, which it set to
   * CHTTP_ASYNC_WRITING. It did all of that under ctx->idle_lock, and it
   * returned with that lock STILL HELD. See its own doc comment for why.
   *
   * This function must keep that lock held for everything below. It unlocks
   * only after the write attempt at the bottom of this function really
   * happens, whether that attempt succeeds or fails. No concurrent dispatch
   * may safely touch ctx before then.
   *
   * The assignment below is a harmless no-op in the reused case, because
   * those fields already hold the same values. It is the only assignment in
   * the fresh case. It still goes through deadline_lock in both cases. See
   * the field comment of that lock: in the reused case, the deadline
   * sweep can already hold this ctx from an earlier fresh connect. */
  ccol_mutex_lock(ctx->deadline_lock);
  ctx->overall_deadline = chain->overall_deadline;
  ccol_mutex_unlock(ctx->deadline_lock);
  ctx->chain = chain;
  ctx->hop = hop;
  ctx->cur_method = method;
  ctx->is_https = url.is_https;
  ctx->is_ipv6 = url.is_ipv6;
  ctx->verify_host = chain->verify_host;
  /* A reused ctx keeps the generation that its own handshake used. Only a
   * fresh connection is about to run a handshake under the pinned
   * configuration of the chain. An overwrite on the reused path erases
   * exactly the fact that _async_idle_pool_take just checked. */
  if (!reused) ctx->tls_generation = chain->tls_generation;
  ctx->reused = reused;
  ctx->reuse_checked = false;
  ctx->any_bytes_read = false;
  ctx->hop_completed = false;
  ctx->interim_responses_seen = 0;

  /* This code captures the field again on EVERY hop, a reused connection
   * included. The request path can differ from the previous hop that used
   * this same pooled connection. That is true even when the origin does not
   * change, and host, port and origin_key are different in that respect.
   *
   * _async_handle_redirect needs this field to resolve a Location header
   * with a relative path against the URL of THIS hop. This matches the
   * per-hop chttp_url_t url local of Tier 1. See the struct comment of
   * this field for the crash that it prevents.
   *
   * This code frees the field first, because a reused ctx can still carry
   * the copy of its previous owner. The offer to the idle pool clears it
   * only when it can, and not as a requirement for correctness. */
  _ccol_mem_free(chain->mp, ctx->path_and_query);
  ctx->path_and_query = ccol_strdup(chain->mp, url.path_and_query);
  if (!ctx->path_and_query) {
    _url_free(chain->mp, &url);
    _async_submit_hop_fail(ctx, ccol_not_enough_memory);
    return false;
  }

  ctx->pctx.mp = chain->mp;
  ctx->pctx.is_head_request = (method == CHTTP_HEAD);
  /* A streaming request sets chain->write_fn. It delivers body bytes
   * straight to the callback of the caller. A buffered request uses
   * ctx->bb instead. See the comment of _async_build_response for why
   * this choice must agree with what that function does later. */
  ctx->pctx.requested_sink_fn =
      chain->write_fn ? chain->write_fn : _sink_buffered;
  ctx->pctx.requested_sink_ctx = chain->write_fn ? chain->write_ctx : &ctx->bb;
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
  /* Without this line, _serialize_request never writes
   * "expect: 100-continue" onto the wire. It reads req->expect_continue,
   * and it reads nothing at the level of the chain.
   *
   * ctx->want_100_continue below still comes from chain->expect_continue
   * on its own. It therefore still engages the machinery that splits the
   * write and waits. The wait genuinely happens, and the header never
   * reaches the peer.
   *
   * The test async_step_a.expect_continue_field_reaches_the_wire is not
   * vacuous here. Without this line it fails, and it counts zero such
   * headers on the wire. */
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
  /* This matches the identical use_100_continue local of chttp_do_internal
   * in Tier 1 exactly. See the comment of that function. It says why
   * has_explicit_expect must come from the scan of _serialize_request
   * that ignores case, and not from a second naive lookup.
   *
   * This code computes body_carrying_method again here. It does not thread
   * the value through from hop_req, because chttp_method_t has no public
   * accessor for it outside the internal use of
   * _serialize_request. */
  bool body_carrying_method_ac =
      (method == CHTTP_POST || method == CHTTP_PUT || method == CHTTP_PATCH);
  ctx->want_100_continue = chain->expect_continue && !hop_hp.has_expect &&
                           body_carrying_method_ac && body_data && body_len > 0;
  ctx->header_len = ctx->want_100_continue ? ctx->wire_len - body_len : 0;
  atomic_store(&ctx->continue_decided, false);
  atomic_store(&ctx->continue_msg_started, false);

  if (!ctx->origin_key) {
    /* This code always sets the field, except on the reused path. There
     * the field already carries the origin that the library pooled this
     * connection under. That value is identical to url.origin_key by
     * construction: _async_idle_pool_take only ever returns a connection
     * that is filed under the exact origin_key that it looked up. */
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
    /* This connection is already connected. For HTTPS its handshake is
     * already done too. It therefore skips CONNECTING and TLS_HANDSHAKING
     * entirely. The code above already moved ctx->state to
     * CHTTP_ASYNC_WRITING, under idle_lock and together with ctx->chain.
     * It did that before it touched any of the other per-hop fields below.
     *
     * The direction of the registration must flip from read, which is its
     * steady state in the idle pool, to write. This code deliberately does
     * NOT make the write attempt itself, on this calling thread, which is
     * not a reactor thread. It leaves that entirely to the dispatch of
     * _async_on_writable. The first write of a fresh connection already
     * works in exactly that way.
     *
     * This is not only simpler. Correctness needs it. ccol_event_loop_modify
     * flips this registration to write interest. From that moment, a reactor
     * thread that already runs is free to dispatch _async_on_writable for
     * it. It can do that at the same time as this calling thread.
     * The guarantee of ccol_event_loop only stops two dispatches of
     * the same registration from running concurrently with EACH OTHER. It
     * says nothing about a caller like this function, which is not a
     * callback, that races a dispatch which it just made possible.
     *
     * A write attempt here directly races that concurrent dispatch. It
     * also fails to move ctx->state and the direction to READING after a
     * write that completes fully. That hangs a sequential reuse of the
     * same pooled connection. */
    ccol_retval_t reactivate_rv;
#ifdef RUNNING_UNIT_TESTS
    if (atomic_exchange(&g_force_reactivate_fail_for_tests, false)) {
      /* This simulates a failed ccol_event_loop_modify call. The code
       * deliberately does NOT make the real call. ctx->reg therefore stays
       * exactly as a genuine failure leaves it, which is still registered
       * for read. See the comment of this branch below for why
       * ccol_event_loop_modify never changes the direction on any failure
       * path that it can return through. See the comment of
       * g_force_reactivate_fail_for_tests for why the real call can never
       * fail here. */
      reactivate_rv = ccol_unexpected_failure;
    } else
#endif
    {
      reactivate_rv = ccol_event_loop_modify(cli_engine_bundler.reactor,
                                             ctx->reg, ccol_select_write);
    }
    if (reactivate_rv != ccol_success) {
      /* See the field comment of pending_app_teardown. The
       * ccol_event_loop registration of the ctx is still fully live here,
       * because the library just popped it from the idle pool for reuse. A
       * reactor dispatch callback can therefore legitimately be in flight,
       * or about to run, on another thread at this exact moment.
       *
       * A call to _async_ctx_teardown directly from this thread, which is
       * not a dispatch thread, is a use-after-free. This code marks ctx as
       * terminal instead.
       *
       * It sets pending_app_teardown BEFORE the _async_retry_hop call.
       * That function sets ctx->hop_completed to true as its own very
       * first statement. The write order between the two flags does not
       * matter for correctness today. The top check of
       * _async_on_writable_impl cannot structurally be dispatched during
       * this window in any case, as the comment of that check says. This
       * code still sets pending_app_teardown first, so that correctness
       * never comes to depend on that.
       *
       * It then shuts the fd down below, which forces a genuine EPOLLIN or
       * EPOLLERR dispatch. Whichever dispatch callback notices it runs the
       * real destructive teardown safely, from a dispatch context. See
       * _async_ctx_handle_if_abandoned. Nothing may touch ctx again after
       * the unlock of idle_lock. */
      ctx->pending_app_teardown = true;
      _async_retry_hop(ctx); /* This sets ctx->hop_completed to true
                              * itself. It also queues a brand-new ctx to
                              * retry the request. The library abandons
                              * this ctx from here on. This is the reused
                              * one whose reactivation just failed. */
      /* This runs while idle_lock is still held, for the same reason as in
       * the reused branch of _async_submit_hop_fail. An unlock first
       * lets a dispatch that is parked in _async_dispatch_kind run the
       * real teardown. That teardown close()s ctx->fd. A shutdown() after
       * that point acts on a bare int, and the kernel can already have
       * given that number to an unrelated socket in this process. */
#ifdef RUNNING_UNIT_TESTS
      atomic_store(&g_abandon_shutdown_under_lock_for_tests,
                   ctx->idle_lock_held_for_tests ? 1 : 0);
#endif
      if (ctx->fd >= 0) shutdown(ctx->fd, SHUT_RDWR);
#ifdef RUNNING_UNIT_TESTS
      ctx->idle_lock_held_for_tests = false;
#endif
      ccol_mutex_unlock(ctx->idle_lock);
      /* This frees the SEPARATE engine reference that
       * _async_idle_pool_offer took for this ctx while it sat in the idle
       * pool. _async_ctx_teardown only ever frees the chain reference of
       * the ctx, once the deferred dispatch finally runs it. It knows
       * nothing about this one.
       *
       * This is the same class of leak that the reused branch of
       * _async_submit_hop_fail guards against. See the comment there.
       * This exit reaches it through a failed ccol_event_loop_modify
       * instead.
       *
       * This call is safe directly from this thread. It only ever touches
       * a global counter, and never ctx. */
      _client_engine_release();
      return true;
    }
    /* ctx is fully consistent again. Every per-hop field above is set, and
     * the registration now correctly reflects WRITING. idle_lock can
     * therefore be unlocked. A concurrent dispatch that waited for it now
     * goes on against a coherent ctx. That can happen almost at once,
     * because the fd is freshly reused and is almost certainly already
     * writable. */
#ifdef RUNNING_UNIT_TESTS
    ctx->idle_lock_held_for_tests = false;
#endif
    ccol_mutex_unlock(ctx->idle_lock);
    /* The connection of this ctx now belongs to the chain, and not to the
     * idle pool. Free the engine reference that it held while the pool had
     * it. The single reference of the chain covers it from here on,
     * and the chain holds that reference for its whole lifetime.
     *
     * No deadline registration is needed here. The library already
     * registered this ctx when it created it, on its very first fresh hop
     * attempt. That registration lasts across every cycle through the idle
     * pool. See the comment of _client_deadline_register. */
    _client_engine_release();
    return true;
  }

  /* This registers ctx BEFORE it submits the task, and not after. Once
   * ctpool_submit hands ctx to a worker, that worker can connect, run the
   * whole hop to completion, and free ctx. It can do all of that before
   * this thread gets a chance to register it. A registration first
   * guarantees that the registry safely holds ctx for the whole time that
   * any other thread can touch it. */
  _client_deadline_register(ctx);

  ccol_retval_t sr = ctpool_submit(cli_engine_bundler.dns_pool,
                                   _async_connect_task, ctx, NULL);
  if (sr != ccol_success) {
    _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
    _async_ctx_teardown(ctx); /* This unregisters ctx too, through
                               * _async_ctx_free. */
    return false;
  }
  return true;
}

/*
 * The library calls this after it fully parses the response of a hop as a
 * redirect. That parse sets pctx.will_redirect. This function resolves the
 * Location header against the URL of this hop. It then applies the same
 * rules about the method and the body that Tier 1 applies. A 307 or a 308
 * keeps the method and the body. Every other redirect status downgrades the
 * method to GET and drops the body. The one exception is a current method
 * that is already HEAD. This function then finishes the connection of this
 * hop. It offers that connection to the idle pool when `reusable` is true,
 * and it closes the connection otherwise. See _async_finish_connection.
 * Only THEN does it queue the next hop.
 *
 * That order is deliberate and load-bearing for the close case. The
 * close(fd) call inside _async_ctx_free runs synchronously, on THIS thread,
 * before _async_finish_connection returns. The connect() call of the next
 * hop runs on a completely different ctpool worker thread. A loopback
 * redirect chain opens and closes a fresh fd on every hop, in quick
 * succession. The kernel can therefore hand that worker back the EXACT SAME
 * fd number that it just freed. That can happen only after the worker
 * thread really runs, and it cannot run until ctpool_submit queues the next
 * hop. This function finishes the connection FIRST, with no condition. The
 * old fd is then fully closed, and its ccol_event_loop registration is
 * fully removed, before _async_submit_hop ever calls ctpool_submit. There is
 * therefore no window where the socket() of the new hop can receive the
 * number of an old fd that the reactor still holds. A connection that the
 * library pools frees no fd at all, so this order costs nothing there
 * either.
 *
 * The job of this connection is done whether or not the handoff to the next
 * hop succeeds. On a failure, _async_submit_hop already fulfilled the future
 * with a specific error. A Location that does not resolve is different.
 * This function then creates no next hop at all, and the refcount of the
 * chain does not change. Either way, the teardown of this ctx, inside
 * _async_finish_connection, is the one that is guaranteed to notice that
 * nothing fulfilled the future. It notices that through the backstop of
 * _async_chain_release, and it then fulfils the future itself with a generic
 * error.
 */
static void _async_handle_redirect(chttp_async_ctx_t *ctx, bool reusable) {
  chttp_async_chain_t *chain = ctx->chain;
  /* This is a temporary extra reference. The _async_finish_connection call
   * below can start a full teardown of the chain reference of THIS ctx. It
   * starts that teardown through the teardown of _async_ctx_finish, or
   * through the explicit release of a successful idle-pool offer. That
   * teardown runs on a DIFFERENT reactor thread that runs at the same time.
   * If that reference is the last live one, the library frees chain while
   * this function still uses its local `chain` pointer below. That pointer
   * goes into the arguments of _async_submit_hop and into
   * _ccol_mem_free(chain->mp, ...). This is the same class of race that this
   * whole section documents: a concurrent teardown frees something that a
   * synchronous caller still needs. It sits one level above the races that
   * are local to a ctx. A retain here first, and a release only after this
   * function is completely done with `chain`, guarantees that the count
   * never reaches zero too early. That holds however fast a concurrent
   * teardown runs. */
  _async_chain_retain(chain);

  if (ctx->hop >= CHTTP_MAX_REDIRECTS) {
    /* This response is itself the 51st request in the chain, and it is ALSO
     * a redirect. To follow it would pass the budget of 50 redirects. The
     * library reports an error here and does not deliver the response
     * quietly. This matches the identical cap check of chttp_do_internal in
     * Tier 1. A caller that uses ccol_http_too_many_redirects to detect a
     * redirect loop needs a real error here. It does not need a stale 3xx
     * response that it has to notice and read itself. This code tries no URL
     * resolution and no work for a next hop at all. */
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
  /* Two fields must come from ctx, and not stay at the zero that memset
   * leaves. They are is_ipv6 above and path_and_query below. A false
   * is_ipv6 quietly produces a redirect target of the shape
   * "scheme://<ipv6-literal>:port/path" with no brackets. The next hop then
   * fails to parse it. A NULL path_and_query crashes the process, through a
   * strchr() on a NULL pointer inside _merge_ref_path. That happens the
   * moment that a server sends a genuinely relative Location header. Such a
   * header is not an absolute path, not a full URL, and not
   * protocol-relative. _async_submit_hop captures both ctx fields again on
   * every hop. See their own struct comments. */
  base.path_and_query = ctx->path_and_query;

  char *next_url = _resolve_redirect_url(chain->mp, &base, ctx->pctx.location);
  /* See _redirect_transport_allowed. The library reports a refused target as
   * ccol_http_invalid_url. That matches the identical check of
   * chttp_do_internal in Tier 1 and Tier 3. This code checks it here, while
   * `base` still describes the hop that this Location arrived on. The
   * library must permit the target against that hop. */
  bool transport_refused = false;
  if (next_url &&
      !_redirect_transport_allowed(chain->mp, &base, next_url,
                                   chain->prevent_tls_downgrade_on_redirect)) {
    _ccol_mem_free(chain->mp, next_url);
    next_url = NULL;
    transport_refused = true;
  }
  bool preserve =
      (ctx->pctx.status_code == 307 || ctx->pctx.status_code == 308);
  chttp_method_t next_method = ctx->cur_method;
  /* This code reads chain->body_dropped. It does not read body_data,
   * body_len and body_content_type with no condition. An earlier hop on this
   * chain can already have dropped the body, through a redirect that does
   * not preserve the method. The body must then STAY dropped for every later
   * hop. That holds for a later hop that is itself a 307 or a 308. Such a
   * hop preserves whatever the CURRENT body is. It does not restore the
   * original hop-0 body of the chain. See the field comment of
   * chain->body_dropped for the full reasoning. That comment also says why
   * the equivalent in Tier 1 needs no such flag. That equivalent is the
   * cur_body loop local, which the code overwrites in place. */
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
  /* This code captures the value BEFORE the _async_finish_connection call,
   * and never after it. That call can start the ordinary teardown of THIS
   * ctx, through a dispatch on a concurrent reactor thread. That teardown
   * frees the ctx. Nothing may therefore read ctx->hop afterwards. The
   * temporary retain of the chain above protects `chain` only. It does
   * nothing for ctx. Nothing may touch ctx after its own connection goes to
   * _async_finish_connection. */
  int next_hop = ctx->hop + 1;

  _async_finish_connection(ctx, reusable);

  if (next_url) {
    _async_submit_hop(chain, next_url, next_method, next_body_data,
                      next_body_len, next_body_ct, next_hop);
    _ccol_mem_free(chain->mp, next_url);
  } else if (transport_refused) {
    /* This code fulfils the future explicitly. It does not leave the case to
     * the ccol_http_transfer_aborted backstop of _async_chain_release
     * below. Every tier therefore reports the identical
     * ccol_http_invalid_url for a refused redirect target. */
    _async_fulfill_chain(chain, ccol_http_invalid_url, NULL);
  }
  _async_chain_release(chain); /* This frees the temporary reference that
                                * the code took above. */
}

/*
 * This is the pre-flight validation of Tier 2 and Tier 3, which both queue
 * through _chttp_do_async_internal. It validates cli and req. It parses
 * req->url into *url_out, and the caller must call _url_free on that value
 * after a ccol_success return. It also checks whether TLS is usable. These
 * are the same three checks that chttp_do_internal of Tier 1 makes. The
 * result codes are the same too. They are ccol_invalid_args, whatever
 * _parse_chttp_url returns, and ccol_http_tls_cert_load_failed.
 * _chttp_do_async_internal hands the code on through its rv_out, so that
 * Tier 3 reports it exactly as Tier 1 does.
 *
 * Tier 1 does NOT call this function. Its equivalent checks sit inside the
 * per-hop loop of chttp_do_internal, and that loop runs them again on every
 * hop. A redirect can change the URL and the scheme from one hop to the
 * next. There is therefore no single check at the start to move out of
 * Tier 1. Tier 2 and Tier 3 need this check once only, before any chain or
 * future exists.
 */
static ccol_retval_t _chttp_async_preflight_check(struct chttpclient *cli,
                                                  const chttp_request_t *req,
                                                  chttp_url_t *url_out) {
  if (!cli || !req) return ccol_invalid_args;

  ccol_retval_t prv = _parse_chttp_url(cli->m_procs, req->url, url_out);
  if (prv != ccol_success) return prv;

  ccol_mutex_lock(cli->lock);
  bool tls_ctx_usable = cli->tls_ctx_usable;
  ccol_mutex_unlock(cli->lock);

  if (url_out->is_https && !tls_ctx_usable) {
    /* The configured cert, key or CA path was not readable at the set_tls
     * call. Or ctls itself failed to load or parse it there. The library
     * defers that failure to here, and it does not stop the process. See
     * _rebuild_tls_ctx_locked. This matches the identical check of Tier 1
     * and its choice of error code. See the comment of chttp_do_internal
     * on this exact check. That comment says why
     * ccol_http_tls_cert_load_failed is correct here, and why
     * ccol_http_tls_handshake_failed is not. */
    _url_free(cli->m_procs, url_out);
    return ccol_http_tls_cert_load_failed;
  }
  return ccol_success;
}

/*
 * This is an internal entry point. It submits req to run asynchronously
 * against cli. It returns a future, and the caller must pair that future
 * with exactly one ctpool_future_free call. An optional
 * ctpool_future_get or ctpool_future_done call can come first.
 *
 * It returns NULL when the library cannot even queue the request. The
 * causes are bad arguments, an invalid URL, TLS that is not usable, an
 * allocation that fails, and an engine that does not start. This matches
 * the "NULL on a failure" convention of ctpool_submit_future. *rv_out then
 * holds the specific cause, and it holds ccol_success when a future
 * returns.
 *
 * After the library creates a future, it returns that future for every
 * later failure. It fulfils the future with a specific error instead of a
 * NULL return. A failure on a later redirect hop is one such failure. This
 * matches how Tier 1 returns a ccol_retval_t and never stops quietly.
 *
 * The library frees every queued hop, with no condition. It also frees the
 * one engine reference of the whole chain. Both happen after the redirect
 * chain reaches a terminal connection state. See the file-level comment of
 * chttp_async_chain_t.
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
  /* This code captures the value before it frees url. Nothing else needs
   * url, because the pre-check above is its only other consumer.
   * _async_submit_hop parses req->url again on every hop by itself. The
   * chain tracks whether the origin changed since hop 0. See the comment
   * of chttp_async_chain_t.initial_origin_key. That tracking needs the
   * origin_key of hop 0 to live for the whole lifetime of the chain, and not
   * only for the stack frame of this function. */
  char *initial_origin_key = ccol_strdup(mp, url.origin_key);
  _url_free(mp, &url);
  if (!initial_origin_key) return NULL;

  /* This reads and pins the TLS context of the client under its lock. It
   * matches what chttp_do_internal of Tier 1 does. ctls_ctx_retain pins that
   * context against a concurrent chttpclient_set_tls call. Such a call frees
   * or rebuilds the context while this request still uses it.
   *
   * This code pins the context with no condition. It does not pin it only
   * for a first hop that uses https. A redirect chain can hop between http
   * and https, exactly like the tls_ctx local of Tier 1.
   * _async_chain_release frees the pinned reference once, with
   * ctls_ctx_release, when the library tears the last hop of the whole chain
   * down. */
  ctls_ctx_t *tls_ctx;
  bool tls_ctx_usable;
  bool verify_host;
  /* This code takes the snapshot under the same lock as tls_ctx itself. The
   * chain therefore carries one coherent TLS identity. See the field
   * comment of chttpclient.tls_generation. */
  uint64_t tls_generation;
  uint64_t connect_timeout_us;
  uint64_t request_timeout_us;
  size_t max_response_body_size;
  ccol_mutex_lock(cli->lock);
  tls_ctx = cli->tls_ctx;
  tls_ctx_usable = cli->tls_ctx_usable;
  /* The hostname match runs only when the chain is verified at all. A match
   * against a certificate that nothing validated proves nothing; see the
   * field comments of chttp_tls_config_t. */
  verify_host =
      !cli->tls.insecure_skip_verify && !cli->tls.insecure_skip_hostname_check;
  tls_generation = cli->tls_generation;
  connect_timeout_us = cli->connect_timeout_us;
  request_timeout_us = cli->request_timeout_us;
  max_response_body_size = cli->max_response_body_size;
  if (tls_ctx) ctls_ctx_retain(tls_ctx);
  ccol_mutex_unlock(cli->lock);

  ccol_retval_t arv = _client_engine_acquire_chain();
  if (arv != ccol_success) {
    if (tls_ctx) ctls_ctx_release(tls_ctx);
    _ccol_mem_free(mp, initial_origin_key);
    *rv_out = arv;
    return NULL;
  }

  /* The result that the future will carry is allocated here, before the
   * future exists, so that no later step can fail to report its code. See
   * _async_fulfill_chain. ctpool_future_create_detached fails only when an
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
  /* From here on a future exists, and this function always returns it to the
   * caller. The caller owns the pairing of that future with exactly one
   * ctpool_future_free call. Every later failure fulfils the future with a
   * specific error in place of a NULL return.
   *
   * This code captures the future into a local now. After the code below
   * queues hop 0, a worker can run the request to completion. It can then
   * free the chain through _async_chain_release, before the thread of
   * this function runs one more instruction. A read of chain->future
   * afterwards is a use-after-free. */
  ctpool_future *f = future;

  chttp_async_chain_t *chain = _async_chain_create(
      mp, cli, future, (chmap)req->headers, req->body.data, req->body.len,
      req->body.content_type, initial_origin_key, tls_ctx, tls_ctx_usable,
      verify_host, tls_generation, connect_timeout_us, request_timeout_us,
      max_response_body_size, write_fn, write_ctx, req->expect_continue,
      req->prevent_tls_downgrade_on_redirect);
  /* _async_chain_create makes a deep copy of initial_origin_key. It treats
   * req_headers, body_data and body_content_type in the same way. The own
   * local copy of this function never lives past this call. That holds
   * whether or not the creation of the chain succeeded. */
  _ccol_mem_free(mp, initial_origin_key);
  if (!chain) {
    if (tls_ctx) ctls_ctx_release(tls_ctx);
    /* _async_chain_create fails only when an allocation fails. That
     * allocation is the calloc, the ccol_mutex_init, or one of the deep
     * copies that the function makes. ccol_not_enough_memory is therefore
     * always the accurate cause here.
     *
     * This code reports it through the chttpcli_async_result_t that the
     * request already owns. It does not fulfil the future with a bare NULL.
     * chttpclient_async_result_get therefore returns a genuine result that
     * carries the error code. It does not collapse this specific and common
     * failure into the same NULL that a cancelled future produces, which
     * Tier 3 could not tell from any other cause. */
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

/* These are white-box test helpers. They expose internal state of the
 * engine. They are not part of the public API. A gate keeps these symbols
 * out of a production build of libccollections.so. The engine code around
 * them carries no such gate, because chttpclient_do_async and
 * chttpclient_do_async_streaming are real public callers. These functions
 * exist only for test instrumentation. */
#ifdef RUNNING_UNIT_TESTS
/* This exposes sizeof(chttp_async_chain_t), which is an internal type that
 * is not public. A test can therefore build a fault-injecting allocator that
 * targets a size. That allocator fails exactly the calloc call of the
 * chain struct inside _async_chain_create. It does that whatever number of
 * other allocations of other sizes come first. The parse of a URL makes some
 * of those. This is deterministic. A later change that shifts the number of
 * earlier allocation calls cannot break it. A fault injector that counts an
 * index does break in that way. */
size_t _chttp_async_chain_struct_size_for_tests(void) {
  return sizeof(chttp_async_chain_t);
}

int _chttpclient_engine_ref_count_for_tests(void) {
  ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  ccol_mutex_lock(cli_engine_bundler.mutex);
  int n = (int)cli_engine_bundler.reactor_refs;
  ccol_mutex_unlock(cli_engine_bundler.mutex);
  return n;
}

clog _chttpclient_engine_logger_for_tests(void) {
  ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  ccol_mutex_lock(cli_engine_bundler.mutex);
  clog l = cli_engine_bundler.logger;
  ccol_mutex_unlock(cli_engine_bundler.mutex);
  return l;
}

bool _chttpclient_engine_running_for_tests(void) {
  ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  ccol_mutex_lock(cli_engine_bundler.mutex);
  bool running = (cli_engine_bundler.reactor != CCOL_EVENT_LOOP_INVALID);
  ccol_mutex_unlock(cli_engine_bundler.mutex);
  return running;
}

ccol_retval_t _chttpclient_engine_acquire_for_tests(void) {
  return _client_engine_acquire();
}

void _chttpclient_engine_release_for_tests(void) { _client_engine_release(); }

void _chttpclient_engine_wait_for_quiescence_for_tests(void) {
  _client_engine_wait_for_quiescence();
}

/* See g_reaper_final_hold_ms_for_tests. */
void _chttpclient_hold_next_reaper_final_section_for_tests(int ms) {
  atomic_store(&g_reaper_final_hold_ms_for_tests, ms);
}

bool _chttpclient_reaper_final_section_is_held_for_tests(void) {
  return atomic_load(&g_reaper_final_held_for_tests);
}

/* This waits until no teardown of the engine is in flight, and leaves the
 * handle of the reaper that finished it unjoined, which is the state that
 * a process holds between the stop of its engine and its next acquire. */
void _chttpclient_engine_wait_stopped_unjoined_for_tests(void) {
  ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  ccol_mutex_lock(cli_engine_bundler.mutex);
  while (cli_engine_bundler.stopping)
    ccol_cond_var_wait(cli_engine_bundler.stopped_cv, cli_engine_bundler.mutex);
  ccol_mutex_unlock(cli_engine_bundler.mutex);
}

/* This copies the handle of the reaper that nobody joined yet into *out,
 * and returns false when there is none. */
bool _chttpclient_engine_unjoined_reaper_for_tests(ccol_thread_id_t *out) {
  ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  ccol_mutex_lock(cli_engine_bundler.mutex);
  bool joinable = cli_engine_bundler.reaper_joinable;
  if (joinable) *out = cli_engine_bundler.reaper_thread;
  ccol_mutex_unlock(cli_engine_bundler.mutex);
  return joinable;
}

/* See the comment of g_abandon_shutdown_under_lock_for_tests. This
 * returns -1 when no shutdown for an abandoned ctx has run yet. It returns 1
 * when the last one ran under the idle_lock of that ctx, which is the
 * only safe order. It returns 0 when that shutdown ran after the unlock. */
int _chttpclient_abandon_shutdown_under_lock_for_tests(void) {
  return atomic_load(&g_abandon_shutdown_under_lock_for_tests);
}

void _chttpclient_reset_abandon_shutdown_probe_for_tests(void) {
  atomic_store(&g_abandon_shutdown_under_lock_for_tests, -1);
}

/* See the block for TLS direction injection near the top of this file. A
 * mode of 0 disarms the injection. A mode of 1 means "a TLS write needs the
 * connection to become readable first". A mode of 2 means "a TLS read needs
 * it to become writable first". To arm the injection also clears every
 * counter, so that each test starts from a known state. A test must disarm
 * the injection before it returns. The reactor threads of this module
 * keep running between tests. */
void _chttpclient_tls_dir_inject_for_tests(int mode) {
  atomic_store(&g_tls_dir_blocked, false);
  atomic_store(&g_tls_dir_released, false);
  atomic_store(&g_tls_dir_block_attempts, 0);
  atomic_store(&g_tls_dir_read_interest_while_writing, 0);
  atomic_store(&g_tls_dir_write_interest_while_reading, 0);
  atomic_store(&g_tls_dir_inject_mode, mode);
}

/* This is true after this module really releases the injected block. It
 * releases that block when it waits or registers in the direction that the
 * injection asked for. */
bool _chttpclient_tls_dir_released_for_tests(void) {
  return atomic_load(&g_tls_dir_released);
}

unsigned _chttpclient_tls_dir_read_interest_while_writing_for_tests(void) {
  return atomic_load(&g_tls_dir_read_interest_while_writing);
}

unsigned _chttpclient_tls_dir_write_interest_while_reading_for_tests(void) {
  return atomic_load(&g_tls_dir_write_interest_while_reading);
}

/* This is the number of TLS I/O attempts that the armed injection above has
 * refused so far. A caller that reacts to the reported direction pays
 * exactly one refusal. The wait or the registration that it makes in that
 * direction releases the injection, and the next attempt goes through. A
 * caller that keeps a retry in its own home direction, on a connection that
 * is already ready, is a busy spin. This counter is what counts that
 * spin. */
unsigned _chttpclient_tls_dir_block_attempts_for_tests(void) {
  return atomic_load(&g_tls_dir_block_attempts);
}

/* See the comment of g_force_reaper_spawn_fail_for_tests. This reports
 * every attempt to create a reaper thread as a failure. A test can therefore
 * exercise the give-up path with no real ceiling on threads. That path
 * leaves the engine running for the next release to try again. */
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

void _chttpclient_force_reaper_spawn_fail_for_tests(bool on) {
  atomic_store(&g_force_reaper_spawn_fail_for_tests, on);
}

unsigned _chttpclient_engine_reaper_abandoned_count_for_tests(void) {
  return atomic_load(&g_reaper_abandoned_count_for_tests);
}

size_t _chttpclient_engine_num_reactor_threads_for_tests(void) {
  ccol_call_once(cli_engine_bundler.once, _client_engine_globals_init);
  ccol_mutex_lock(cli_engine_bundler.mutex);
  size_t n = cli_engine_bundler.last_resolved_num_reactor_threads;
  ccol_mutex_unlock(cli_engine_bundler.mutex);
  return n;
}

/* This rewrites the last_used timestamp of every idle connection that the
 * pool of Tier 2 and Tier 3 holds now. It moves each one far enough into the
 * past. The CHTTP_IDLE_MAX_AGE_MS staleness check of
 * _async_idle_pool_take then treats that connection as too old on the very
 * next pop. A test therefore never has to wait out the real window of 60
 * seconds. This helper is for a test only. It exists to make the staleness
 * eviction path in _async_idle_pool_take reachable in a deterministic
 * way. */
void _chttpclient_force_async_idle_stale_for_tests(struct chttpclient *cli) {
  ccol_mutex_lock(cli->lock);
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
  ccol_mutex_unlock(cli->lock);
}

/* This shuts down every fd that sits in the async idle pool of Tier 2 and
 * Tier 3 now. It leaves each connection exactly where it is. The own
 * IDLE-state dispatch of each connection then removes it from the pool. That
 * dispatch is _async_idle_ctx_finish. It frees the connection and it frees
 * the engine reference that the connection held. It does that from a reactor
 * dispatch context, and asynchronously against this call. The teardown
 * sweep of __chttpclient_destroy drives the identical mechanism. shutdown()
 * is equally safe under cli->lock there, because it has no synchronous
 * application callback of its own.
 *
 * This helper is for a test only. It exists so that a test can put the
 * release of the LAST engine reference on a reactor thread on purpose. The
 * test then does not wait for a server to close a pooled connection at a
 * moment that nothing controls. */
void _chttpclient_shutdown_async_idle_connections_for_tests(
    struct chttpclient *cli) {
  ccol_mutex_lock(cli->lock);
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
  ccol_mutex_unlock(cli->lock);
}

/* This reads cli->idle_total_count_async. That is the number of connections
 * that sit in the async idle pool of Tier 2 and Tier 3 now, across every
 * origin. This helper is for a test only. It lets a test check live pool
 * membership directly, in place of a check that derives it indirectly. A
 * test needs that to state an invariant that does not depend on an ordinal.
 * That invariant is "whenever this pool is empty, the reference count that
 * it contributes to the engine must be zero too". It holds whatever internal
 * allocation an injected out-of-memory failure lands on. */
size_t _chttpclient_async_idle_total_count_for_tests(struct chttpclient *cli) {
  ccol_mutex_lock(cli->lock);
  size_t n = cli->idle_total_count_async;
  ccol_mutex_unlock(cli->lock);
  return n;
}

/* This counts the connections of the async idle pool of cli, across every
 * origin, that carry a timed_out mark. An idle connection never carries
 * one: the mark belongs to the hop that ended when the connection became
 * idle. This helper is for a test only. */
size_t _chttpclient_async_idle_timed_out_count_for_tests(
    struct chttpclient *cli) {
  size_t n = 0;
  ccol_mutex_lock(cli->lock);
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
  ccol_mutex_unlock(cli->lock);
  return n;
}

/* This reads the number of distinct origin keys that cli->idle_pools of
 * Tier 1 holds an entry for now. This helper is for a test only. It lets a
 * test check that a pop of the last connection for an origin really prunes
 * the chmap entry of that origin. That entry is empty at that point.
 * Without the prune, an empty cvec for that origin stays behind forever. See
 * the comment about the prune in _idle_pool_take. */
size_t _chttpclient_idle_pools_key_count_for_tests(struct chttpclient *cli) {
  ccol_mutex_lock(cli->lock);
  size_t n = cli->idle_pools ? chmap_elem_count(cli->idle_pools) : 0;
  ccol_mutex_unlock(cli->lock);
  return n;
}

/* This is the async counterpart of
 * _chttpclient_idle_pools_key_count_for_tests above, for Tier 2 and Tier 3.
 * It reads cli->idle_pools_async. */
size_t _chttpclient_idle_pools_async_key_count_for_tests(
    struct chttpclient *cli) {
  ccol_mutex_lock(cli->lock);
  size_t n =
      cli->idle_pools_async ? chmap_elem_count(cli->idle_pools_async) : 0;
  ccol_mutex_unlock(cli->lock);
  return n;
}

/* This overrides the effective value of CHTTP_MAX_IDLE_ORIGINS for the rest
 * of this process. _idle_pool_offer and _async_idle_pool_offer share that
 * value. See the comment of g_max_idle_origins_override_for_tests. Pass
 * 0 to restore the real compile-time constant. This setting covers the whole
 * process, and not one client. Every other fault-injection hook in this file
 * whose name ends in _for_tests works in the same way. */
void _chttpclient_set_max_idle_origins_for_tests(size_t n) {
  atomic_store(&g_max_idle_origins_override_for_tests, n);
}

/* This resolves h to its underlying struct chttpclient*, and it does NOT pin
 * it. It touches the pin index not at all. It is a bare lookup in the slot
 * table. It is safe for a test, because test code that calls it runs
 * synchronously on one thread. There is therefore no concurrent destroy to
 * race with.
 *
 * _chttpcli_resolve is different. It has a matching _unpin call that a test
 * has to remember, and that is an easy gap to leave behind. A forgotten
 * unpin leaves one pin outstanding on that client. Every later
 * chttpclient_destroy call against that client then hangs with no report.
 *
 * This function returns NULL under exactly the same conditions as
 * _chttpcli_resolve. The library flips the slot table record that this code
 * reads, and the pin index record that the other function reads, under the
 * same lock. The two can therefore never disagree. */
struct chttpclient *_chttpcli_resolve_for_tests(chttpcli h) {
  ccol_call_once(chttpcli_slot_table.once, _chttpcli_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  ccol_rw_lock_rdlock(chttpcli_slot_table.rwlock);
  struct chttpclient *raw = NULL;
  if (idx < cvector_elem_count(chttpcli_slot_table.slots)) {
    chttpcli_slot_t *slot =
        (chttpcli_slot_t *)cvector_at(chttpcli_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
  return raw;
}

/* This reads the number of slots that the chttpcli handle table holds now.
 * That number covers the slots that the table grew, and the slots that the
 * library freed and has not reused yet. It lets a test assert that a loop
 * which creates and destroys clients reuses freed slots. Such a loop must
 * not grow the table without a bound. */
/* This reads the indices that sit on the free list now. Read it next to the
 * capacity above. A rollback that loses a slot shows up as a table that
 * grew. But it shows up only while the free list was empty. A test that
 * measures the growth alone therefore passes or fails according to how many
 * handles earlier tests happened to hold at one time. The two figures
 * together describe the table without that dependency. */
size_t _chttpcli_free_index_count_for_tests(void) {
  ccol_call_once(chttpcli_slot_table.once, _chttpcli_slot_table_init_globals);
  ccol_rw_lock_rdlock(chttpcli_slot_table.rwlock);
  size_t n = cvector_elem_count(chttpcli_slot_table.free_indices);
  ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
  return n;
}

size_t _chttpcli_slot_table_capacity_for_tests(void) {
  ccol_call_once(chttpcli_slot_table.once, _chttpcli_slot_table_init_globals);
  ccol_rw_lock_rdlock(chttpcli_slot_table.rwlock);
  size_t n = cvector_elem_count(chttpcli_slot_table.slots);
  ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
  return n;
}

/* This reads the number of pins that are outstanding against the slot of h
 * now. It lets a test assert directly that a resolve and an unpin stay
 * balanced. A test does not have to derive that from a destroy that would
 * simply never return. This function takes the handle and not a resolved
 * pointer. A caller therefore needs no pin of its own to ask. */
size_t _chttpcli_pin_count_for_tests(chttpcli h) {
  return ccol_pintable_pins_for(&chttpcli_pintable, h);
}

/* This makes the library treat the very next cvector_push_back call of
 * _async_idle_pool_offer as a failure. That holds for any client and any
 * origin. It touches neither the real allocator nor the real list of that
 * origin. The library consumes the flag the first time that it reaches that
 * call site afterwards. Call this function again before each attempt that
 * must exercise this path. See the comment of
 * g_force_offer_push_fail_for_tests, above the "ASYNC IDLE POOL" section.
 * That comment says why ordinary injection of an allocator failure cannot
 * reach this scenario. */
void _chttpclient_force_offer_push_fail_once_for_tests(void) {
  atomic_store(&g_force_offer_push_fail_for_tests, true);
}

/* This arms the next plain-HTTP hop that finishes the write of its request.
 * That hop then flips its own registration back to the write direction at
 * once. This reproduces a continue-timeout flip of the deadline sweep that
 * lands after the hop already moved on. See the comment of
 * g_force_stray_write_arm_for_tests, above the "ASYNC IDLE POOL" section.
 * The first such hop afterwards consumes the flag. */
void _chttpclient_force_stray_write_arm_once_for_tests(void) {
  atomic_store(&g_force_stray_write_arm_for_tests, true);
}

/* This arms the next connection that reaches the async idle pool. That
 * connection then flips its own registration back to the write direction at
 * once. This is the counterpart of the hook above for a pooled connection.
 * See the comment of g_force_stray_idle_write_arm_for_tests. */
void _chttpclient_force_stray_idle_write_arm_once_for_tests(void) {
  atomic_store(&g_force_stray_idle_write_arm_for_tests, true);
}

/* This sets the delay on either side of the carry-over replay for
 * "Expect: 100-continue". The value is in milliseconds. A value of 0 turns
 * the delay off. See the comment of
 * g_continue_carry_replay_delay_ms_for_tests. */
/* See g_idle_probe_hold_for_tests. */
void _chttpclient_arm_idle_probe_hold_for_tests(void) {
  atomic_store(&g_idle_probe_hold_for_tests, 1);
}

/* This waits up to timeout_ms for the IDLE branch to reach the hold, and
 * reports whether it did. */
bool _chttpclient_wait_idle_probe_held_for_tests(long timeout_ms) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
  for (long i = 0; i < timeout_ms; i++) {
    if (atomic_load(&g_idle_probe_hold_for_tests) == 2) return true;
    nanosleep(&ts, NULL);
  }
  return atomic_load(&g_idle_probe_hold_for_tests) == 2;
}

/* This ends the hold, or disarms a hold that nothing reached. */
void _chttpclient_release_idle_probe_hold_for_tests(void) {
  int holding = 2;
  if (!atomic_compare_exchange_strong(&g_idle_probe_hold_for_tests, &holding,
                                      3))
    atomic_store(&g_idle_probe_hold_for_tests, 0);
}

void _chttpclient_set_continue_carry_replay_delay_ms_for_tests(long ms) {
  atomic_store(&g_continue_carry_replay_delay_ms_for_tests, ms);
}

/* This is the number of times that the write-direction fall-through of
 * _async_on_writable_impl restored the read direction of a registration. It
 * counts from the start of this process. */
unsigned long long _chttpclient_stray_write_arm_restores_for_tests(void) {
  return atomic_load_explicit(&g_stray_write_arm_restores_for_tests,
                              memory_order_relaxed);
}

/* This makes the library treat the very next ccol_event_loop_modify call of
 * _async_submit_hop on a reused connection as a failure. That holds for any
 * client. It touches the real registration not at all. The library consumes
 * the flag the first time that it reaches that call site afterwards. See the
 * own comment of g_force_reactivate_fail_for_tests, above the "ASYNC IDLE
 * POOL" section. That comment says why ordinary injection of an allocator
 * failure cannot reach this scenario. */
void _chttpclient_force_reactivate_fail_once_for_tests(void) {
  atomic_store(&g_force_reactivate_fail_for_tests, true);
}

/* This makes the library treat the very next read dispatch of
 * _async_on_readable_impl as a hard error. It applies only to a ctx that
 * already had at least one real successful read. It therefore never applies
 * to the very first read of a fresh or reused connection. The library acts
 * exactly as if recv() or ctls_conn_read() returned -1 with ECONNRESET. It
 * touches the real socket not at all. The library consumes the flag the
 * first time that it reaches that condition afterwards. See the comment
 * of g_force_async_hard_read_error_for_tests, above the "ASYNC IDLE POOL"
 * section. That comment says why nobody can pin the timing of a real TCP RST
 * down over a real socket. */
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
  /* _m_procs points into the block that this call frees. */
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
 * Both functions below are thin wrappers. Each one submits the request
 * through Tier 2. It then blocks on the future. It unwraps the result into
 * the same shape that chttpclient_do and chttpclient_do_streaming use. That
 * shape is a ccol_retval_t plus resp_out, or a ccol_retval_t plus
 * status_code_out. It then frees the future and its result before it
 * returns. The caller never sees a ctpool_future or a
 * chttpcli_async_result_t at all.
 *
 * They call _chttp_do_async_internal directly, and not the public
 * chttpclient_do_async, because only the internal function reports WHY it
 * could not queue the request: a bad URL or a TLS configuration that is not
 * usable gets the same specific code that chttpclient_do uses, an
 * allocation that fails gets ccol_not_enough_memory, and an engine that does
 * not start gets the code of that failure. The public functions collapse
 * every one of those into a NULL future.
 */

ccol_retval_t chttpclient_do_pooled(chttpcli h, const chttp_request_t *req,
                                    chttpcli_response **resp_out) {
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;

  struct chttpclient *raw = _chttpcli_resolve(h);
  if (!raw) return ccol_invalid_args;
  ccol_retval_t qrv;
  ctpool_future *f = _chttp_do_async_internal(raw, req, NULL, NULL, &qrv);
  /* The chain that the call above queued keeps the client alive on its
   * own. See the async_in_flight_count of struct chttpclient. */
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
  /* The chain that the call above queued keeps the client alive on its
   * own. See the async_in_flight_count of struct chttpclient. */
  _chttpcli_resolve_unpin(raw);
  if (!f) return qrv;

  chttpcli_async_result_t *result = chttpclient_async_result_get(f);
  if (!result) {
    ctpool_future_free(f);
    return ccol_unexpected_failure;
  }

  ccol_retval_t rv = result->rv;
  if (result->resp) {
    /* A streaming request always builds a chttpcli_response internally. Its
     * body and headers stay NULL there. See _async_build_response. It exists
     * only so that status_code has somewhere to travel, through the single
     * chttpcli_async_result_t.resp field. The public contract of this
     * function has no resp_out at all, which matches chttpclient_do_streaming
     * exactly. This code therefore unwraps that response and frees it here.
     * It never exposes it to the caller. */
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

chttpcli ccol_create_chttpclient_mp(ccol_memmgmt_procs_t *mprocs,
                                    char **err_str) {
  if (mprocs && !ccol_verify_memmgmt_procs(mprocs, err_str))
    return CHTTPCLI_INVALID;

  ccol_memmgmt_procs_t *mp = NULL;
  if (mprocs) {
    mp = (ccol_memmgmt_procs_t *)mprocs->malloc(sizeof(ccol_memmgmt_procs_t));
    if (!mp) {
      if (err_str) *err_str = CCOL_ERR_STR("failed to allocate mprocs");
      return CHTTPCLI_INVALID;
    }
    memcpy(mp, mprocs, sizeof(ccol_memmgmt_procs_t));
  }

  struct chttpclient *cli =
      (struct chttpclient *)_ccol_mem_calloc(mp, 1, sizeof(struct chttpclient));
  if (!cli) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate client");
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }

  cli->m_procs = mp;
  cli->tls = CHTTP_TLS_DEFAULT;
  if (ccol_mutex_init(cli->lock) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to initialize mutex");
    _ccol_mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }
  /* This uses CLOCK_MONOTONIC. _slot_acquire gives this condition variable a
   * timespec from that same clock, through ccol_cond_var_timedwait.
   * _deadline_make builds that timespec. See the comment of
   * _ccol_cond_var_init_monotonic for why the two clocks must match. */
  if (_ccol_cond_var_init_monotonic(&cli->available) != 0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("failed to initialize condition variable");
    ccol_mutex_destroy(cli->lock);
    _ccol_mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }
  if (ccol_cond_var_init(cli->idle_async_drained) != 0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("failed to initialize condition variable");
    ccol_mutex_destroy(cli->lock);
    ccol_cond_var_destroy(cli->available);
    _ccol_mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }
  if (ccol_mutex_init(cli->async_count_lock) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to initialize mutex");
    ccol_mutex_destroy(cli->lock);
    ccol_cond_var_destroy(cli->available);
    ccol_cond_var_destroy(cli->idle_async_drained);
    _ccol_mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }
  if (ccol_cond_var_init(cli->async_count_drained) != 0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("failed to initialize condition variable");
    ccol_mutex_destroy(cli->lock);
    ccol_cond_var_destroy(cli->available);
    ccol_cond_var_destroy(cli->idle_async_drained);
    ccol_mutex_destroy(cli->async_count_lock);
    _ccol_mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }

  char *herr = NULL;
  cli->idle_pools =
      chmap_create_full(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                        ccol_pointer, mp, NULL, NULL, &herr);
  if (!cli->idle_pools) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate idle pool map");
    ccol_mutex_destroy(cli->lock);
    ccol_cond_var_destroy(cli->available);
    ccol_cond_var_destroy(cli->idle_async_drained);
    ccol_mutex_destroy(cli->async_count_lock);
    ccol_cond_var_destroy(cli->async_count_drained);
    _ccol_mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }

  cli->idle_pools_async =
      chmap_create_full(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                        ccol_pointer, mp, NULL, NULL, &herr);
  if (!cli->idle_pools_async) {
    if (err_str)
      *err_str = CCOL_ERR_STR("failed to allocate async idle pool map");
    __chmap_destroy(cli->idle_pools);
    ccol_mutex_destroy(cli->lock);
    ccol_cond_var_destroy(cli->available);
    ccol_cond_var_destroy(cli->idle_async_drained);
    ccol_mutex_destroy(cli->async_count_lock);
    ccol_cond_var_destroy(cli->async_count_drained);
    _ccol_mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }

  if (_rebuild_tls_ctx_locked(cli) != ccol_success) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to build default TLS context");
    __chmap_destroy(cli->idle_pools);
    __chmap_destroy(cli->idle_pools_async);
    ccol_mutex_destroy(cli->lock);
    ccol_cond_var_destroy(cli->available);
    ccol_cond_var_destroy(cli->idle_async_drained);
    ccol_mutex_destroy(cli->async_count_lock);
    ccol_cond_var_destroy(cli->async_count_drained);
    _ccol_mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }

  chttpcli h = _chttpcli_handle_slot_acquire(cli);
  if (h == 0) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate client slot");
    if (cli->tls_ctx) ctls_ctx_release(cli->tls_ctx);
    __chmap_destroy(cli->idle_pools);
    __chmap_destroy(cli->idle_pools_async);
    ccol_mutex_destroy(cli->lock);
    ccol_cond_var_destroy(cli->available);
    ccol_cond_var_destroy(cli->idle_async_drained);
    ccol_mutex_destroy(cli->async_count_lock);
    ccol_cond_var_destroy(cli->async_count_drained);
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
  ccol_mutex_lock(cli->lock);
  cli->configured_pool_size = n;
  cli->pool_cap = _resolve_pool_cap(n);
  cli->pool_initialized = true;
  ccol_cond_var_broadcast(cli->available);
  ccol_mutex_unlock(cli->lock);
  _chttpcli_resolve_unpin(cli);
  return ccol_success;
}

ccol_retval_t chttpclient_set_connect_timeout(chttpcli h, uint64_t us) {
  struct chttpclient *cli = _chttpcli_resolve(h);
  if (!cli) return ccol_invalid_args;
  ccol_mutex_lock(cli->lock);
  cli->connect_timeout_us = us;
  ccol_mutex_unlock(cli->lock);
  _chttpcli_resolve_unpin(cli);
  return ccol_success;
}

ccol_retval_t chttpclient_set_request_timeout(chttpcli h, uint64_t us) {
  struct chttpclient *cli = _chttpcli_resolve(h);
  if (!cli) return ccol_invalid_args;
  ccol_mutex_lock(cli->lock);
  cli->request_timeout_us = us;
  ccol_mutex_unlock(cli->lock);
  _chttpcli_resolve_unpin(cli);
  return ccol_success;
}

ccol_retval_t chttpclient_set_max_response_body_size(chttpcli h,
                                                     size_t max_bytes) {
  struct chttpclient *cli = _chttpcli_resolve(h);
  if (!cli) return ccol_invalid_args;
  ccol_mutex_lock(cli->lock);
  cli->max_response_body_size = max_bytes;
  ccol_mutex_unlock(cli->lock);
  _chttpcli_resolve_unpin(cli);
  return ccol_success;
}

ccol_retval_t chttpclient_set_tls(chttpcli h, const chttp_tls_config_t *tls) {
  if (!h) return ccol_invalid_args;
  /* A client certificate and its private key are a pair. Exactly one of the
   * two is never a valid configuration. Without this check, the library
   * treats that input as "no client certificate is configured", and it
   * reports nothing. That is the effect of the have_cert_pair check of
   * _rebuild_tls_ctx_locked further down, which simply needs both. A mutual
   * TLS deployment would then believe that it presents a client certificate
   * although it never does. This check reads only the tls argument that the
   * caller gives, and never cli. It therefore stays here, before any resolve,
   * and it needs no pin and no unpin of its own. */
  if (tls && ((tls->cert_path && !tls->key_path) ||
              (!tls->cert_path && tls->key_path)))
    return ccol_invalid_args;
  /* A CA bundle exists to be verified against. To name one and to switch
   * verification off in the same struct states two incompatible policies, and
   * whichever one the library picked would surprise half of the callers that
   * wrote it. The mistake is also the dangerous direction if the library
   * guessed the opt-out: a deployment that pinned a private CA would silently
   * accept every peer. The library therefore refuses the pair here, at the
   * call that declares the configuration, instead of at the first request
   * that would have used it. Like the cert/key check above, this reads only
   * the argument of the caller, so it needs no resolve and no pin. */
  if (tls && tls->insecure_skip_verify && tls->ca_bundle_path)
    return ccol_invalid_args;

  struct chttpclient *cli = _chttpcli_resolve(h);
  if (!cli) return ccol_invalid_args;

  ccol_mutex_lock(cli->lock);

  _ccol_mem_free(cli->m_procs, cli->owned_cert_path);
  _ccol_mem_free(cli->m_procs, cli->owned_key_path);
  _ccol_mem_free(cli->m_procs, cli->owned_ca_bundle_path);
  cli->owned_cert_path = cli->owned_key_path = cli->owned_ca_bundle_path = NULL;

  if (!tls) {
    cli->tls = CHTTP_TLS_DEFAULT;
    ccol_retval_t rv = _rebuild_tls_ctx_locked(cli);
    ccol_mutex_unlock(cli->lock);
    _chttpcli_resolve_unpin(cli); /* This is exit 1 of 4. */
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
    ccol_mutex_unlock(cli->lock);
    _chttpcli_resolve_unpin(cli); /* This is exit 2 of 4. */
    return rv;
  }

oom:
  _ccol_mem_free(cli->m_procs, cli->owned_cert_path);
  _ccol_mem_free(cli->m_procs, cli->owned_key_path);
  _ccol_mem_free(cli->m_procs, cli->owned_ca_bundle_path);
  cli->owned_cert_path = cli->owned_key_path = cli->owned_ca_bundle_path = NULL;
  cli->tls = CHTTP_TLS_DEFAULT;
  /* cli->tls_ctx and cli->tls_ctx_usable still describe the configuration
   * that was in force before this call started. An earlier successful
   * chttpclient_set_tls call built it. The code above just reset cli->tls
   * itself to the default. To leave tls_ctx and tls_ctx_usable untouched
   * would therefore split the declared configuration of the client from its
   * real TLS behavior at run time. Nothing would report that split. Requests
   * would keep using the OLD certificate and CA material forever, and
   * cli->tls would show no sign of it.
   *
   * A rebuild now brings tls_ctx back in step with the default configuration
   * that this call really leaves in place. Its own return value is not the
   * result of this function. The real failure to report is the failed strdup
   * above, and not whatever a rebuild of a plain default configuration needs
   * to allocate. */
  _rebuild_tls_ctx_locked(cli);
  ccol_mutex_unlock(cli->lock);
  _chttpcli_resolve_unpin(cli); /* This is exit 3 of 4. All three checks for
                                   a failed strdup above reach it. */
  return ccol_not_enough_memory;
}

/* ========================================================================== */
/*                         CLIENT DESTRUCTION                                 */
/* ========================================================================== */

void __chttpclient_destroy(chttpcli cli) {
  if (!cli) return;

  /* This is a defence. The caller can pass the handle that
   * chttp_default_client() returns. The doc comment of that function
   * invites a caller to pass it to a chttpclient_set_* function. Nothing
   * stops a caller from passing it here too. This code therefore clears the
   * own copy of that handle inside the singleton first.
   *
   * Without this, default_client_bundler.client keeps a handle that this call
   * is about to invalidate. Two things then go wrong. Any later
   * chttp_default_client(), chttp_do() or chttp_get() call in this process
   * hands that handle straight back out. That is a use-after-destroy, because
   * default_client_bundler.once never fires again to build the client anew.
   * And the process-exit destructor of this file destroys the client a
   * second time. The generation-checked handle design reports that as a fatal
   * double destroy, and not as a quiet double free.
   *
   * This does nothing for any client that ccol_create_chttpclient or
   * ccol_create_chttpclient_mp really created. The compare-and-swap fails
   * harmlessly there, because such a handle can never equal the handle of
   * this singleton. */
  chttpcli expected = cli;
  atomic_compare_exchange_strong(&default_client_bundler.client, &expected, 0);

  /* This resolves cli through the slot table. It marks the slot not-in-use
   * in the same critical section as the lookup. That is what makes a second
   * destroy call on the same handle value see a resolve failure. It holds
   * for a concurrent second call and for a later sequential one. Such a call
   * therefore never races the teardown of this call. See the file-level
   * comment of the slot table, and the comment of _chttpcli_resolve, for the
   * full design.
   *
   * A stale handle, or one that the library already destroyed, is exactly
   * the misuse that the generation-checked handle design exists to catch. It
   * is fatal here. It is never a quiet use-after-free or double free. */
  ccol_call_once(chttpcli_slot_table.once, _chttpcli_slot_table_init_globals);
  uint32_t idx = (uint32_t)(cli >> 32);
  uint32_t gen = (uint32_t)(cli & 0xFFFFFFFFu);
  ccol_rw_lock_wrlock(chttpcli_slot_table.rwlock);
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
    ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
    ccol_fatal_err(
        "chttpclient_destroy: handle is stale or already destroyed "
        "(double-destroy / use-after-destroy of a chttpcli handle)");
  }
  slot->in_use = false; /* This blocks ALL later resolves for this handle
                            from this instant. That includes a second
                            concurrent destroy attempt. */
  /* This is the same step, under the same lock. From here the library grants
   * no new pin. That is what lets the count below reach zero and stay there.
   * The library flips both records of "is this handle resolvable" together,
   * so the two can never disagree. */
  ccol_pintable_retire(&chttpcli_pintable, idx);
  ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);

  /* The mark and the wake come BEFORE the pin drain, and never after it. A
   * caller that parks and waits for pool capacity holds a resolve pin while
   * it sleeps. A drain first would therefore wait out the whole remaining
   * request of that caller. The wake instead makes that caller return
   * ccol_not_permitted at once. The occupant that frees its pool slot would
   * release that caller in the end either way. This order therefore bounds
   * how long a destroy takes, and it closes no deadlock. The equivalent
   * order in cthreadpool does close a real deadlock. Its workers block on
   * the queue of the pool, and only its shutdown can release them. */
  ccol_mutex_lock(raw->lock);
  raw->destroying = true;
  ccol_cond_var_broadcast(raw->available);
  ccol_mutex_unlock(raw->lock);

  /* This then waits out every in-flight caller that resolved before the
   * retire above. It polls, and it does not sleep on a condition variable.
   * The unpin side deliberately delivers no wakeup, because its whole point
   * is to touch nothing but the pin. This code deliberately holds no lock
   * either. A pinned caller commonly needs raw->lock to finish its own call
   * and to release its pin. A wait here with that lock held would deadlock
   * against exactly the callers that it waits for. */
  {
    long delay_ns = 1000;
    while (ccol_pintable_pins(&chttpcli_pintable, idx) > 0) {
      struct timespec ts = {.tv_sec = 0, .tv_nsec = delay_ns};
      nanosleep(&ts, NULL);
      if (delay_ns < 1000000L) delay_ns *= 2;
    }
  }

  /* in_flight_count is a separate gate, and it keeps its condition variable.
   * A caller adds to it while it still holds its pin. It frees that pin only
   * after it has done so. The drain above can therefore never have missed
   * one. */
  ccol_mutex_lock(raw->lock);
  while (raw->in_flight_count > 0)
    ccol_cond_var_wait(raw->available, raw->lock);
  ccol_mutex_unlock(raw->lock);

  /* This waits for every ACTIVE async chain of Tier 2 and Tier 3 that the
   * library created for this client. An active chain is one that the idle
   * pool does not hold yet. The wait comes before anything else below.
   *
   * An in-flight chain can still connect, run a handshake, write or read. It
   * does that on a reactor thread, or on a thread of the DNS and connect
   * pool. It dereferences chain->cli and ctx->cli at almost any point. It
   * reaches raw->lock, raw->idle_pools_async and raw->m_procs through them.
   * That lasts until the chain tears down, or until it joins the idle pool
   * that this function drains further below.
   *
   * Without this wait, a caller that writes
   * `f = chttpclient_do_async(cli, req); chttpclient_destroy(cli);` frees raw
   * under a request that is still in flight. See the comment of this
   * field on struct chttpclient. That comment says why this code uses a
   * dedicated lock and condition variable here, and not raw->lock and
   * raw->available.
   *
   * This must run before the drain of the idle pool below. An active chain
   * that completes while this wait still runs is exactly what feeds fresh
   * entries into that pool. The drain of the idle pool then cleans them
   * up. */
  ccol_mutex_lock(raw->async_count_lock);
  while (raw->async_in_flight_count > 0)
    ccol_cond_var_wait(raw->async_count_drained, raw->async_count_lock);
  ccol_mutex_unlock(raw->async_count_lock);

  if (raw->idle_pools) {
    cmap_iterator *it = chashmap_begin_iter(raw->idle_pools, NULL);
    for (; it; it = it->_next_fn(it)) {
      cvec list = _read_cvec(it->val_pair->ptr);
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

  /* This handles the idle pool of Tier 2. See the "ASYNC IDLE POOL"
   * section earlier in this file. It shuts down the fd of every connection
   * that the pool holds now. The IDLE-state dispatch of each connection
   * then removes it from the pool and frees it. That dispatch is
   * _async_idle_ctx_finish, and it also frees the engine reference that the
   * idle connection held. It runs asynchronously, after the reactor sees the
   * fd.
   *
   * This code then waits for idle_total_count_async to reach zero before it
   * goes on. The code below is about to free raw, and those deferred
   * teardowns read raw->lock and raw->idle_pools_async.
   *
   * shutdown() is safe to call while this code still holds raw->lock. The
   * identical use of it in the deadline sweep depends on the same property.
   * It has no synchronous callback at the application level. There is
   * therefore no hazard of reentrancy and no hazard of lock order in a call
   * to it from inside this loop. This code calls shutdown() and does not
   * close the fd directly here. */
  ccol_mutex_lock(raw->lock);
  if (raw->idle_pools_async) {
    cmap_iterator *ait = chashmap_begin_iter(raw->idle_pools_async, NULL);
    for (; ait; ait = ait->_next_fn(ait)) {
      cvec list = _read_cvec(ait->val_pair->ptr);
      if (!list) continue;
      size_t n = cvector_elem_count(list);
      for (size_t i = 0; i < n; i++) {
        chttp_async_ctx_t *actx = *(chttp_async_ctx_t **)cvector_at(list, i);
        int afd = actx->fd;
        if (afd >= 0) shutdown(afd, SHUT_RDWR);
      }
    }
  }
  /* idle_total_count_async reaches zero only after _async_idle_ctx_finish
   * really removes and tears down every pooled connection. That subtraction
   * happens strictly after _async_ctx_free returns. See the comment of
   * that function. It covers every stale candidate that the shutdown sweep
   * above is only now forcing an EOF or an error dispatch for.
   *
   * A concurrent walk of _async_idle_pool_take can leave a stale candidate
   * with a shutdown and no reap yet. That is the first mechanism, and the own
   * comment of that function describes it. This count still covers such a
   * candidate, because it still sits in the vector.
   *
   * The deferred teardowns of the second mechanism use
   * pending_app_teardown. See the comment of that field. The wait on
   * async_in_flight_count above already covers them. Such a ctx still holds
   * ctx->chain, and therefore the reference of the chain. It holds it
   * until its own _async_ctx_teardown call really runs from a dispatch. That
   * earlier wait can therefore not have returned while one is still
   * pending. */
  while (raw->idle_total_count_async > 0)
    ccol_cond_var_wait(raw->idle_async_drained, raw->lock);
  ccol_mutex_unlock(raw->lock);

  if (raw->idle_pools_async) {
    cmap_iterator *it = chashmap_begin_iter(raw->idle_pools_async, NULL);
    for (; it; it = it->_next_fn(it)) {
      cvec list = _read_cvec(it->val_pair->ptr);
      if (list) __cvector_destroy(list);
    }
    __chmap_destroy(raw->idle_pools_async);
  }

  if (raw->tls_ctx) ctls_ctx_release(raw->tls_ctx);

  ccol_mutex_destroy(raw->lock);
  ccol_cond_var_destroy(raw->available);
  ccol_cond_var_destroy(raw->idle_async_drained);
  ccol_mutex_destroy(raw->async_count_lock);
  ccol_cond_var_destroy(raw->async_count_drained);

  ccol_memmgmt_procs_t *mp = raw->m_procs;
  _ccol_mem_free(mp, raw->owned_cert_path);
  _ccol_mem_free(mp, raw->owned_key_path);
  _ccol_mem_free(mp, raw->owned_ca_bundle_path);
  _ccol_mem_free(mp, raw);
  if (mp) mp->free(mp);

  /* This frees the slot last, only after the library fully tore raw down and
   * freed it. The rise of the generation of the slot, and the push of the
   * index onto the free list, are what mark the handle as reusable. No
   * earlier step does that.
   *
   * This code fetches the slot again by idx. It does not reuse `slot`. A
   * concurrent ccol_create_chttpclient_mp call can run its own
   * _chttpcli_handle_slot_acquire in between. That call can grow the backing
   * array of slots with cvector_push_back. Every pointer into that array
   * from before this second lock is then stale. idx itself stays valid. */
  ccol_rw_lock_wrlock(chttpcli_slot_table.rwlock);
  chttpcli_slot_t *slot2 =
      (chttpcli_slot_t *)cvector_at(chttpcli_slot_table.slots, idx);
  slot2->ptr = NULL;
  slot2->generation++; /* This raises the generation of this slot past
      whatever value the handle of the client that the library just freed
      carried. That stale handle can therefore never match the generation of
      a LATER acquire for this same index. */
  cvector_push_back(chttpcli_slot_table.free_indices, &idx);
  /* The process-exit destructor can already have run and found this client
     live. The release that it could not do then belongs to whoever frees the
     last slot, and that can be this call. */
  _chttpcli_release_slot_table_if_deferred_locked();
  ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
}

/* ========================================================================== */
/*                         REQUEST EXECUTION                                  */
/* ========================================================================== */

/*
 * A response that starts to arrive while Tier 1 still sends the request.
 *
 * A server may answer before it has read the whole request. A 401 from an
 * authentication check that never reads the body, and a 413 from a limit on
 * the size of a body, are the everyday cases. Such a server often closes the
 * connection, or stops reading, once it has answered. A client that only
 * writes until the request is complete then sees its write fail with EPIPE
 * or ECONNRESET, or block for good, and loses an answer that already sits
 * in its receive buffer. curl and Go's net/http read that answer, and so
 * does every tier of this client.
 *
 * While it sends, Tier 1 therefore also waits for readability, and it feeds
 * whatever arrives to a parser that persists across the send and the read
 * that follows. The rules are those of curl:
 *   - An interim 1xx response is discarded, and the send goes on.
 *   - A final response of 300 or above stops the send once its status line
 *     and header block have arrived. The rest of the request is never sent.
 *   - A final response of 200 to 299 lets the send go on while it arrives,
 *     because a server may stream its answer while it reads the request. A
 *     final response that is complete stops the send, because nothing waits
 *     for the rest of the request any more.
 *   - A write that fails because the peer is gone (EPIPE, ECONNRESET,
 *     ENOTCONN, or a TLS close) stops the send, and the read then takes
 *     whatever answer the peer left before it went.
 * A connection whose response started before its request was sent in full
 * is never reused, whatever the response says about keep-alive: the peer
 * may not read the rest of the request, may still be reading it, or may have
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

static void _early_resp_init(chttp_early_resp_t *e) {
  memset(e, 0, sizeof(*e));
  e->error = ccol_success;
}

static void _early_resp_release(chttp_early_resp_t *e) {
  if (e->parser_live) chttp1_parser_release(&e->parser);
  e->parser_live = false;
}

static void _early_resp_start_parser(chttp_early_resp_t *e,
                                     chttp_parse_ctx_t *pctx) {
  ccol_call_once(client_http1_settings_bundler.once, _init_chttp1_settings);
  chttp1_parser_init(&e->parser, &client_http1_settings_bundler.settings);
  e->parser.data = pctx;
  (void)chttp1_parser_enable_line_spill(&e->parser, pctx->mp);
  e->parser_live = true;
}

/* This feeds data to the parser of e, with the rules above. It discards an
 * interim response and goes on with the bytes after it, exactly as
 * _chttp_read_message_loop does. */
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
         * request is still going out. */
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

/* The peer closed its side while the request was going out. A message that
 * the parser holds may end at that close; with none, there is no answer. */
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

/* This sends data[0..len), as _chttp_send_all does, and takes in a response
 * that arrives meanwhile; see chttp_early_resp_t. It returns ccol_success
 * when the send is complete or stopped by the rules above, and the failure
 * code otherwise. *sent_out gets the number of bytes that went out. */
static ccol_retval_t _chttp_send_all_watch(
    chttp_conn_t *conn, const char *data, size_t len, chttp_deadline_t *overall,
    chttp_parse_ctx_t *pctx, chttp_early_resp_t *e, bool *any_bytes_read_out,
    size_t *sent_out) {
  size_t sent = 0;
  *sent_out = 0;
  /* See _chttp_send_all for why the direction of the wait is derived again
   * after an EWOULDBLOCK of a TLS write. */
  short want_events = POLLOUT;
  char buf[8192];
  /* This is true after a read that returned EWOULDBLOCK, and false after a
   * read that took bytes or saw the end of the stream. See below. */
  bool read_blocked = false;
  while (sent < len && !e->stop_send) {
    int wait_ms;
    if (!_deadline_remaining_ms(overall, &wait_ms)) return ccol_timed_out;
    /* Plaintext that the TLS layer already holds is invisible to poll(2),
     * so a read goes first while ctls_conn_has_pending_input reports input.
     * That report also covers the start of a record that is not complete
     * yet, which no read can take until the rest arrives on the socket.
     * After a read that blocks, the report is therefore not trusted until a
     * read makes progress again, and the poll below waits for the socket,
     * as _chttp_read_message_with does. Without that, a peer that sends
     * part of a record and stops reading turns this loop into a spin: the
     * report stays true, every read blocks, and the write never gets the
     * poll that would wait for room. */
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
        /* An error or a hang-up makes the next write fail, and that failure
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
        /* The connection is gone. The read after the send reports it. */
        e->stop_send = true;
        continue;
      }
      /* A TLS record with no application data, such as a session ticket,
       * or the part of a record. The write goes on. */
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
        /* The peer is gone. It may have answered first. */
        e->stop_send = true;
        continue;
      }
      return ccol_http_transfer_aborted;
    }
    if (n == 0) {
      /* The TLS peer closed. It may have answered first. */
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

/* This reads the rest of the response after _chttp_send_all_watch: the
 * message that its parser holds, or the next one. request_complete says
 * whether every byte of the request went out before a response started; a
 * connection for which that is false is never reused. */
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
      /* The message that the send saw start is an interim one. The final
       * response follows it, and the budget of interim responses is the
       * one of the whole message sequence. */
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
 * This sends `wire` and reads the response. `wire` is the fully serialized
 * request, of `wire_len` bytes. It carries the body at the end, exactly as
 * it is, and that body is exactly `body_len` bytes. There may be no body.
 *
 * This function honours chttp_request_t.expect_continue when
 * `use_100_continue` is true. The caller already confirmed that this hop
 * genuinely has a body to hold back. That condition is
 * body_carrying_method && body.data && body.len > 0. It matches the own
 * condition of _serialize_request for the write of the
 * "expect: 100-continue" header.
 *
 * The case without a 100-continue is one send and then one read.
 *
 * The 100-continue case sends only the header part first. It then waits up
 * to CHTTP_100_CONTINUE_WAIT_US, bounded by whatever is left of `overall`,
 * for the first byte of a response. That window bounds only the wait for
 * the first byte of each message: a message that starts inside it is read
 * to its end under `overall` alone, and a complete interim message other
 * than "100 Continue" leaves the window running for the next one. The wait
 * ends in one of three ways:
 *   - A "100 Continue" interim response. This function then resets pctx with
 *     a fresh header map. That matches the "fresh state for each message"
 *     convention of this codebase. The headers of the interim response
 *     must never leak into the final one. It then sends the body
 *     and reads the real final response. It carries forward any bytes that a
 *     fast server already sent past the boundary of the interim message,
 *     in the same read. See the doc comment of _chttp_read_message for
 *     why that can happen, and for why the library must not drop those bytes
 *     as garbage.
 *   - An answer from the server with no "100 Continue" at all. RFC 7231
 *     SS5.1.1 explicitly permits this, for example to reject a request
 *     without a wish for the body. That response IS the final response, and
 *     the library never sends the body.
 *   - A window that ends before any byte of a response arrived. The library
 *     then sends the body anyway and reads the final response as usual.
 *     This matches the own CURLOPT_EXPECT_100_TIMEOUT_MS behaviour of curl.
 *     A final response that is still arriving when the window ends is the
 *     direct answer above, and the body is never sent.
 *
 * _chttp_read_message_loop drives that wait, with stop_at_status = 100. One
 * _chttp_read_message call is not enough. A server may legitimately send
 * some OTHER interim 1xx status ahead of a "100 Continue" or of its final
 * answer. A "103 Early Hints" from RFC 8297 is one such status. The library
 * must discard it and keep the wait going, still inside this same window. It
 * must not read it as the final response.
 *
 * This function sets *retry_unsafe_out to true the moment that this hop
 * sends the body onto the wire. It does that only AFTER an explicit
 * "100 Continue" from the server on THIS connection. It stays false in every
 * other case. That includes the branch that times out and then sends the
 * body anyway. That branch received no confirmation at all.
 *
 * The retry-once safety net of chttp_do_internal for a reused connection
 * usually assumes one thing. It assumes that such a connection MIGHT have
 * died before this hop wrote one byte to it. See the comment of that
 * function. An explicit "100 Continue" response disproves that assumption
 * outright for this one connection. A later read failure with zero bytes of
 * a final response is therefore no longer safe to read. It no longer means
 * "nothing ever went out, so a retry is free". The library already handed
 * the body to a peer that proved itself alive and willing to receive it
 * moments earlier. To
 * send it again blindly on a second, unrelated connection carries a risk. A
 * server can then process a request that is not idempotent two times.
 */
static ccol_retval_t _chttp_send_and_read(
    chttp_conn_t *conn, const char *wire, size_t wire_len, size_t body_len,
    bool use_100_continue, chttp_deadline_t *overall, chttp_parse_ctx_t *pctx,
    bool *keep_alive_out, bool *any_bytes_read_out, bool *retry_unsafe_out) {
  *retry_unsafe_out = false;
  chttp_early_resp_t early;
  _early_resp_init(&early);
  size_t sent = 0;
  ccol_retval_t prv;
  if (!use_100_continue) {
    prv = _chttp_send_all_watch(conn, wire, wire_len, overall, pctx, &early,
                                any_bytes_read_out, &sent);
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
    /* The read reports a timeout for one of two deadlines. Once a byte of a
     * message reached the parser, only `overall` bounds the read, and its
     * expiry ends the request here: the body is never sent after a part of
     * the answer arrived. */
    int overall_ms;
    if (!_deadline_remaining_ms(overall, &overall_ms)) return ccol_timed_out;
    /* The window ended before a byte of a message arrived. pctx is clean:
     * the read touches it only once a byte reaches the parser, and the loop
     * resets it after every interim message that it discards. The library
     * sends the body anyway, and a response that the server starts while
     * the body goes out is taken in as the case without a continue does.
     *
     * *any_bytes_read_out stays as the wait left it. It is true only when
     * the loop discarded a complete interim response, and such a response
     * proves that the server received this request and started to answer
     * it. A silent retry of the whole request on a fresh connection is then
     * no longer safe; see _chttp_read_message_loop. */
    prv = _chttp_send_all_watch(conn, wire + header_len, body_len, overall,
                                pctx, &early, any_bytes_read_out, &sent);
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
    /* The server answered directly. This already IS the final response, and
     * the library must never send the body.
     *
     * This connection is never safe to reuse. That holds whatever
     * chttp1_should_keep_alive() concluded from the Connection header of
     * the response. RFC 7231 SS5.1.1 says only that a server SHOULD close the
     * connection after it rejects a request in this way. It does not REQUIRE
     * that. A server that obeys the RFC can therefore leave the connection
     * open while it still expects the body that this hop never sent.
     *
     * Without this rule, the library pools that connection. The next
     * unrelated request on this client then writes its own bytes onto that
     * connection. The server still parses those bytes as the leftover body
     * of this hop. The two requests then lose step with each other on one
     * shared, reused connection.
     *
     * Any bytes past the boundary of the response are genuine trailing
     * garbage as well. Nothing legitimate can follow a final response on a
     * connection whose body the library never sent. This code tracks those
     * bytes for diagnostics only. */
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

  /* The library now hands the body to a connection that the server itself
   * just confirmed. It confirmed it with a "100 Continue", which says that
   * the connection is alive and ready to read from. See the doc comment
   * of this function. That confirmation disqualifies the usual retry-once
   * safety net of the caller for a reused connection, from the first byte
   * of the body on, whatever happens next. */
  *retry_unsafe_out = true;
  *any_bytes_read_out = false;
  /* Bytes that arrived with the "100 Continue" are the start of the final
   * response. They go to the parser that also takes whatever arrives while
   * the body goes out. */
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
  /* This code checks resp_out and sets *resp_out to NULL FIRST. It does that
   * before the checks of cli and req below. A failure from a NULL cli, a
   * NULL req or a NULL user_write_fn therefore ALSO leaves *resp_out
   * deterministically NULL. That is not only true for a failure past this
   * point.
   *
   * Every one of the many later failure paths of this function, which return
   * early or break, then leaves *resp_out untouched. That is correct,
   * because the value is already NULL from here on. It is not garbage from
   * the uninitialized local of the caller.
   *
   * chttpclient_resp_free() is documented as safe to call with NULL. That
   * licenses a cleanup idiom that frees with no condition, such as
   * `resp = NULL; rv = chttpclient_do(...); ...;
   * chttpclient_resp_free(resp);`. chttpclient_do_pooled and
   * chttpclient_do_pooled_streaming of Tier 3 depend on exactly this.
   * Tier 1 must behave in the same way. */
  if (!streaming) {
    if (!resp_out) return ccol_invalid_args;
    *resp_out = NULL;
  }
  if (!cli || !req) return ccol_invalid_args;
  if (streaming && !user_write_fn) return ccol_invalid_args;

  /* This code reads request_timeout_us and anchors the overall deadline
   * BEFORE it takes a slot of the concurrency limiter below. The documented
   * contract of chttpclient_set_request_timeout is "the maximum time from
   * when chttpclient_do is called...". It is not "from when a pool slot
   * becomes available". The time that the call spends blocked on a full
   * pool must therefore count against that deadline too.
   * chttpclient_set_pool_size sets the size of that pool. _slot_acquire
   * knows about the deadline for exactly this reason. See its own
   * comment. */
  uint64_t request_timeout_us;
  ccol_mutex_lock(cli->lock);
  request_timeout_us = cli->request_timeout_us;
  ccol_mutex_unlock(cli->lock);
  chttp_deadline_t overall_dl = _deadline_make(request_timeout_us);

  ccol_retval_t rv = _slot_acquire(cli, &overall_dl);
  if (rv != ccol_success) return rv;

  ccol_memmgmt_procs_t *mp = cli->m_procs;

  uint64_t connect_timeout_us;
  bool verify_host;
  ctls_ctx_t *tls_ctx;
  bool tls_ctx_usable;
  size_t max_response_body_size;
  /* This code takes the snapshot together with tls_ctx, under the same lock.
   * The two therefore describe one configuration. The library stamps this
   * value onto every connection that this request opens. See _conn_open. It
   * compares the value against the current value of the client before it
   * reuses a pooled connection. */
  uint64_t tls_generation;
  ccol_mutex_lock(cli->lock);
  connect_timeout_us = cli->connect_timeout_us;
  tls_ctx = cli->tls_ctx;
  tls_ctx_usable = cli->tls_ctx_usable;
  tls_generation = cli->tls_generation;
  max_response_body_size = cli->max_response_body_size;
  /* A skipped chain check makes the hostname match meaningless, so it also
   * turns the hostname check off. This mirrors the Tier 2 derivation in
   * _chttpclient_do_async. */
  verify_host =
      !cli->tls.insecure_skip_verify && !cli->tls.insecure_skip_hostname_check;
  if (tls_ctx)
    ctls_ctx_retain(tls_ctx); /* This pins the context. A concurrent
                                 chttpclient_set_tls call must not free it
                                 while this request still uses it. */
  ccol_mutex_unlock(cli->lock);

  char *cur_url = ccol_strdup(mp, req->url);
  if (!cur_url) {
    if (tls_ctx) ctls_ctx_release(tls_ctx);
    _slot_release(cli);
    return ccol_not_enough_memory;
  }

  chttp_request_body_t empty_body = CHTTP_NO_BODY;
  chttp_method_t cur_method = req->method;
  chttp_request_body_t cur_body = req->body;

  /* This is an Authorization value that the library injects from the
   * userinfo of a URL. It carries across hops while the origin does not
   * change. The origin is the scheme, the host and the port. The library
   * drops the value permanently the first time that the origin changes. That
   * is the default behaviour of curl, with no opt-in for the forwarding
   * of credentials to a trusted redirect. The library never takes the value
   * up again, even when a later hop comes back to the original origin. */
  char *carried_auth = NULL;
  char *carried_auth_origin = NULL;

  /* The credential headers and the Host header that the caller sets with
   * chttp_request_set_header have no origin of their own. carried_auth above
   * does track an origin. The library therefore leaves those headers off
   * every hop whose origin, or host, differs from the one that the ORIGINAL
   * request named. See CHTTP_REDIRECT_DROP_CREDENTIALS.
   *
   * The library captures initial_origin_key once, from the URL of hop 0.
   * redirect_drops holds the CHTTP_REDIRECT_DROP_* flags of the current hop
   * against it, computed again for each hop. See the redirect_drops parameter
   * of _serialize_request, and its own adjustment of has_auth and
   * has_host. Those show how this interacts with the userinfo case for each
   * hop above. */
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
      /* The configured cert, key or CA path was not readable at the set_tls
       * call. Or ctls itself failed to load or parse it there. The library
       * defers that failure to here, and it does not stop the process. See
       * _rebuild_tls_ctx_locked.
       *
       * The code is ccol_http_tls_cert_load_failed, and not
       * ccol_http_tls_handshake_failed. No handshake ever ran. The own
       * ctls_ctx_cert_add and ctls_ctx_trust of ctls.c already report this
       * exact failure mode with this exact code. See ctls.h. This code
       * reuses it in place of a generic handshake-failure code. A caller can
       * then tell three cases apart. The first is "my local cert, key or CA
       * file is bad". The second is "the peer failed the handshake". The
       * third is "the certificate of the peer did not verify". */
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
    /* This matches the check of _serialize_request. That check writes an
     * "expect: 100-continue" header only when the caller has not already set
     * an explicit Expect header. It reads
     * chttp_header_presence_t.has_expect.
     *
     * Without this, a caller can set both req->expect_continue and an own
     * Expect header whose value is not "100-continue". No "expect:" line
     * then reaches the wire, which is correct. But this hop still stalls for
     * CHTTP_100_CONTINUE_WAIT_US. It waits for an interim response that can
     * never arrive, by construction.
     *
     * This code reuses hop_hp. _serialize_request already computed that
     * presence struct over this exact same header map, a few lines above. It
     * does not run a second full scan over req->headers here. */
    bool has_explicit_expect = hop_hp.has_expect;
    bool use_100_continue = req->expect_continue && !has_explicit_expect &&
                            body_carrying_method && cur_body.data &&
                            cur_body.len > 0;

    bool keep_alive = false;
    bool any_bytes_read = false;
    bool retry_unsafe = false;
    prv = _chttp_send_and_read(&conn, wire, wire_len, cur_body.len,
                               use_100_continue, &overall_dl, &pctx,
                               &keep_alive, &any_bytes_read, &retry_unsafe);
    if (prv != ccol_success && reused && !any_bytes_read && !retry_unsafe) {
      /* The reused connection can have died between the liveness probe and
       * this attempt. There are two shapes. The write quietly succeeded into
       * the local send buffer before the close by the peer became visible.
       * Or the read produced not one byte. Either way, the library has
       * parsed nothing and handed nothing to the caller yet. It is therefore
       * safe to retry exactly one time against a brand-new connection.
       *
       * retry_unsafe overrides this. It means that the library already sent
       * the body to this connection. It sent it AFTER an explicit
       * "100 Continue" proved that both the connection and the server were
       * alive moments earlier. A retry would then risk a server that
       * processes a body that is not idempotent two times. See the doc
       * comment of _chttp_send_and_read. */
      _conn_teardown(mp, &conn);
      /* This is a fresh connect deadline for the brand-new connection of
       * this retry. It is NOT the hop-level `connect_dl` above. That
       * deadline covers only the fresh-connect branch for `!reused`, and
       * nothing else consumes it against the clock before this point.
       *
       * The failed attempt against the dead reused connection just above
       * went through _chttp_send_and_read. Only `overall_dl` bounded it. It
       * can legitimately take a real amount of time to fail. A half-open or
       * blackholed peer that answers nothing until part of overall_dl passes
       * is one such case.
       *
       * Without a fresh deadline, that unrelated elapsed time quietly eats
       * into the configured connect_timeout_us budget of the fresh
       * connection. The connect attempt of this retry then times out, or it
       * gets far less than connect_timeout_us, although the caller
       * configured a full budget. This matches the identical fresh connect
       * deadline of _async_retry_hop for the same scenario in Tier 2 and
       * Tier 3. See the connect_deadline field comment of that
       * function. */
      chttp_deadline_t retry_connect_dl = _deadline_make(connect_timeout_us);
      prv = _conn_open(mp, &url, url.is_https, tls_ctx, verify_host,
                       tls_generation, &retry_connect_dl, &overall_dl, &conn);
      if (prv == ccol_success) {
        reused = false;
        prv = _chttp_send_and_read(&conn, wire, wire_len, cur_body.len,
                                   use_100_continue, &overall_dl, &pctx,
                                   &keep_alive, &any_bytes_read, &retry_unsafe);
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

    bool reusable = keep_alive && !pctx.trailing_garbage;
    if (reusable) {
      _idle_pool_offer(cli, &conn);
    } else {
      _conn_teardown(mp, &conn);
    }

    if (pctx.will_redirect) {
      if (hop >= CHTTP_MAX_REDIRECTS) {
        /* This response is itself the 51st request in the chain, and it is
         * ALSO a redirect. To follow it would pass the budget of 50
         * redirects. The library reports an error here and does not deliver
         * the response quietly. A caller that uses
         * ccol_http_too_many_redirects to detect a redirect loop needs a real
         * error here. It does not need a stale 3xx response that it has to
         * notice and read itself. */
        _parse_ctx_free_fields(&pctx);
        _ccol_mem_free(mp, bb.buf);
        _url_free(mp, &url);
        result = ccol_http_too_many_redirects;
        break;
      }
      char *next_url = _resolve_redirect_url(mp, &url, pctx.location);
      /* See _redirect_transport_allowed. The library reports a refused
       * target as ccol_http_invalid_url. That is the same code that an
       * unsupported scheme in the original request URL gets. The code below
       * tells it apart from a Location that does not resolve, where next_url
       * is NULL. This code checks it here, while `url` still describes the
       * hop that this Location arrived on. The library must permit the
       * target against that hop. */
      bool transport_refused = false;
      if (next_url &&
          !_redirect_transport_allowed(
              mp, &url, next_url, req->prevent_tls_downgrade_on_redirect)) {
        _ccol_mem_free(mp, next_url);
        next_url = NULL;
        transport_refused = true;
      }
      bool preserve = (pctx.status_code == 307 || pctx.status_code == 308);
      _parse_ctx_free_fields(&pctx);
      _ccol_mem_free(mp, bb.buf);
      _url_free(mp, &url);
      _ccol_mem_free(mp, cur_url);
      /* This code sets the pointer to NULL immediately after the free. It
       * does not only assign it again on the success path below.
       *
       * A redirect status can carry an empty Location header, or one that
       * does not resolve for another reason. That reason can be a failure of
       * the resolution of RFC 3986 SS5.2 and SS5.3, and not only an
       * allocation failure. The very first check of _resolve_redirect_url is
       * `if (!location || !*location) return NULL;`. An ordinary server
       * response reaches it easily, and no malformed input is needed.
       *
       * next_url is then NULL. The code falls through to the own
       * _ccol_mem_free(mp, cur_url) call of the cleanup after the loop below.
       * Without this assignment, that pointer still holds the value that the
       * code just freed. That is a real double free that a remote peer can
       * trigger. The static analyzer of clang reports it, and no dynamic test
       * covers it, because no mock route sends a redirect with an empty
       * Location. */
      cur_url = NULL;

      if (!next_url) {
        result = transport_refused ? ccol_http_invalid_url
                                   : ccol_http_transfer_aborted;
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
  /* *resp_out is NULL before anything can fail, a stale or invalid handle
   * included. See the same order in chttp_do_internal. */
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;
  struct chttpclient *raw = _chttpcli_resolve(h);
  if (!raw) return ccol_invalid_args;
  ccol_retval_t rv =
      chttp_do_internal(raw, req, false, NULL, NULL, resp_out, NULL);
  /* chttp_do_internal above runs its own _slot_acquire and _slot_release
   * pair inside itself. That pair brackets the whole request, and the
   * blocking network I/O with it. */
  _chttpcli_resolve_unpin(raw);
  return rv;
}

ccol_retval_t chttpclient_do_streaming(chttpcli h, const chttp_request_t *req,
                                       chttpcli_write_fn write_fn,
                                       void *write_ctx, int *status_code_out) {
  struct chttpclient *raw = _chttpcli_resolve(h);
  if (!raw) return ccol_invalid_args;
  ccol_retval_t rv = chttp_do_internal(raw, req, true, write_fn, write_ctx,
                                       NULL, status_code_out);
  _chttpcli_resolve_unpin(raw);
  return rv;
}

/* ========================================================================== */
/*                         DEFAULT CLIENT                                     */
/* ========================================================================== */

static void _init_default_client(void) {
  /* The default client belongs to the library, and _client_at_exit
   * destroys it before OpenSSL releases its global state. */
  ccol_call_once(cli_engine_bundler.exit_hook_once, _client_exit_hook_install);
  atomic_store(&default_client_bundler.client, ccol_create_chttpclient(NULL));
}

/* This destroys the default client from _client_at_exit, unless a call of
 * Tier 1 still runs on it. Such a call can be the caller of exit() itself,
 * from inside its write_fn, and a destroy waits for it. A later
 * chttp_default_client() then gives CHTTPCLI_INVALID, exactly as it does
 * once the destructor of this library ran. */
static void _default_client_destroy_if_idle(void) {
  chttpcli h = atomic_load(&default_client_bundler.client);
  if (!h) return;
  struct chttpclient *raw = _chttpcli_resolve(h);
  if (!raw) return;
  ccol_mutex_lock(raw->lock);
  bool busy = raw->in_flight_count > 0;
  ccol_mutex_unlock(raw->lock);
  _chttpcli_resolve_unpin(raw);
  if (busy) return;
  if (atomic_compare_exchange_strong(&default_client_bundler.client, &h, 0))
    __chttpclient_destroy(h);
}

chttpcli chttp_default_client(void) {
  ccol_call_once(default_client_bundler.once, _init_default_client);
  return atomic_load(&default_client_bundler.client);
}

__attribute__((destructor)) static void _cleanup_default_client(void) {
  /* This clears the handle first, and destroys the client after. The own
   * defensive compare-and-swap of __chttpclient_destroy would otherwise race
   * the read of the handle by this function. See the doc comment of that
   * function. That race needs another thread to destroy the same handle at
   * the exit of the process, which is very unlikely. A clear here first
   * makes that compare-and-swap inside __chttpclient_destroy do nothing at
   * all. It is then never a second writer that races this one. */
  chttpcli cli = atomic_exchange(&default_client_bundler.client, 0);
  if (cli) __chttpclient_destroy(cli);

  /* The library just destroyed the one client that it can still own itself,
   * and it freed the slot of that client. This code now frees the own
   * bookkeeping arrays of the slot table. Without that, the
   * --show-leak-kinds=all run of make memtest reports them as still
   * reachable. It frees them ONLY when no other chttpcli handle that an
   * application owns is still in use.
   *
   * This library does not control the order of __attribute__((destructor))
   * functions and atexit handlers across the shared objects of a process. An
   * application can rely on the exit of the process to reclaim a client that
   * it created directly. It then makes no chttpclient_destroy call of its
   * own. Such an application can still have a live handle that a destructor
   * or an atexit handler touches after this one runs. That code can call
   * chttpclient_destroy, chttpclient_do or a chttpclient_set_* function. It
   * can even call ccol_create_chttpclient again.
   *
   * A free of the shared slot table under a handle that is still live turns
   * that into a use-after-free. A skip of the free instead leaves exactly
   * the leak that this comment already documents for the struct itself. That
   * leak is a caller that never destroyed its client. It now covers the
   * bookkeeping arrays of the slot table as well.
   *
   * The ccol_call_once call here is mandatory, although the line above can
   * already have made one. When cli was 0, the library never created the
   * default client in this process at all. An application that links this
   * library only for cvector, chashmap or chttpserver, and never touches
   * chttpclient, produces that state. The `if (cli)
   * __chttpclient_destroy(cli);` line above is then skipped completely.
   * Nothing in this process ever called ccol_call_once, and the block below
   * would lock a mutex that nothing initialized.
   *
   * This is a deterministic trigger and not a rare race. It fires on every
   * run of any process that links this shared object and never creates a
   * chttpcli handle. A __attribute__((destructor)) function runs for the
   * whole shared object, whatever parts of it the process really used. */
  ccol_call_once(chttpcli_slot_table.once, _chttpcli_slot_table_init_globals);
  ccol_rw_lock_wrlock(chttpcli_slot_table.rwlock);
  if (_chttpcli_any_slot_live_locked()) {
    /* This hands the release to whichever destroy frees the last slot. It
       does not skip the release. An application that does destroy its
       clients therefore leaves nothing behind, whatever order the
       destructors ran in. */
    chttpcli_slot_table.release_deferred = true;
  } else {
    _chttpcli_release_slot_table_locked();
    /* The library deliberately never frees the slot storage of the pin
     * index while the process runs. A resolve indexes that storage with no
     * lock held. Left alone, a leak checker that treats memory which is still
     * reachable as an error reports it.
     *
     * This code frees it here, and not from a destructor of its own. It
     * therefore inherits the same "no handle is still live" guard as the two
     * vectors above. Its order against them is then fixed, and not left to
     * whatever order the destructors happen to run in.
     *
     * Note what that guard buys and what it does not. It establishes that no
     * handle is still resolvable, and that is the condition that matters.
     * Every resolve that is still possible at this point names a handle that
     * this table would reject anyway. It does NOT exclude a concurrent
     * resolver. A resolve takes no lock at all, and the write lock that this
     * code holds orders this only against a change to the slot table.
     *
     * To unpublish each chunk before the free narrows that to a resolver
     * which has not loaded the chunk pointer yet. It does not close the case.
     * A thread that still calls into this module while the process tears
     * itself down therefore stays the contract of the caller to avoid.
     * That is exactly how it works for the two vectors above. */
  }
  ccol_rw_lock_unlock(chttpcli_slot_table.rwlock);
}

/* This answers whether any slot still names a client. The caller holds the
 * write lock.
 *
 * This code reads slot->ptr, and not slot->in_use. A destroy clears in_use as
 * its first step, so that it rejects a second destroy or a new resolve as
 * early as it can. The rest of the teardown runs after that. That teardown
 * quiesces the engine, drains the pins, and makes the final locked release of
 * the index.
 *
 * A scan that trusted in_use alone would free this table under a destroy that
 * is still inside that window. The last step of that destroy then indexes the
 * freed table. The library writes ptr only after it fully acquires a slot,
 * and it clears ptr only in that final locked step. ptr is therefore true for
 * exactly as long as the library must not release the table. */
static bool _chttpcli_any_slot_live_locked(void) {
  size_t slot_count = cvector_elem_count(chttpcli_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++) {
    chttpcli_slot_t *slot =
        (chttpcli_slot_t *)cvector_at(chttpcli_slot_table.slots, i);
    if (slot->ptr != NULL) return true;
  }
  return false;
}

/* This frees the bookkeeping of the table and the pin index. The caller
 * holds the write lock, and it has established that no slot is live. The code
 * sets each vector to NULL as it goes. That is what makes a later call answer
 * "already released" in place of an index into a freed vector. */
/* The check and the release both sit behind one out-of-line call. The destroy
 * path that has to make that call therefore keeps the shape that it would
 * have with none of this. Cold code in a hot object file is not free. Inlined
 * here, the same handful of instructions measurably slows the push path of an
 * unrelated container. It does that by a shift in what the linker lays out
 * around it, with no change to the instruction count. */
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
  /* This code checks resp_out and sets *resp_out to NULL here, before the
   * chttp_default_client() call. It does not leave that to the identical
   * check inside chttpclient_do. Without this, the early return for a NULL
   * cli just below leaves *resp_out untouched on that path. That is the same
   * class of gap that the identical order of chttp_do_internal closes. See
   * the comment of that function. */
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;
  chttpcli cli = chttp_default_client();
  if (!cli) return ccol_unexpected_failure;
  return chttpclient_do(cli, req, resp_out);
}

ccol_retval_t chttp_run_query(chttp_method_t method, const char *url,
                              const chttp_request_body_t *body, chmap headers,
                              chttpcli_response **resp_out) {
  /* This code checks resp_out and sets *resp_out to NULL before the check of
   * url. A failure from a NULL url therefore ALSO leaves *resp_out
   * deterministically NULL. That is not only true for a failure past this
   * point. See the reasoning for the identical order in chttp_do_internal. */
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;
  if (!url) return ccol_invalid_args;
  chttp_request_t *req = chttp_request_new(method, url, body, NULL);
  if (!req) return ccol_not_enough_memory;

  req->headers = headers;
  ccol_retval_t rv = chttp_do(req, resp_out);
  req->headers = NULL; /* req does not own headers. The free below must
                          therefore not destroy that map. */
  chttp_request_free(req);
  return rv;
}

ccol_retval_t chttp_get(const char *url, chttpcli_response **resp_out) {
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;
  if (!url) return ccol_invalid_args;
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  if (!req) return ccol_not_enough_memory;
  ccol_retval_t rv = chttp_do(req, resp_out);
  chttp_request_free(req);
  return rv;
}

ccol_retval_t chttp_post(const char *url, const chttp_request_body_t *body,
                         chttpcli_response **resp_out) {
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;
  if (!url) return ccol_invalid_args;
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, body, NULL);
  if (!req) return ccol_not_enough_memory;
  ccol_retval_t rv = chttp_do(req, resp_out);
  chttp_request_free(req);
  return rv;
}

ccol_retval_t chttp_put(const char *url, const chttp_request_body_t *body,
                        chttpcli_response **resp_out) {
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;
  if (!url) return ccol_invalid_args;
  chttp_request_t *req = chttp_request_new(CHTTP_PUT, url, body, NULL);
  if (!req) return ccol_not_enough_memory;
  ccol_retval_t rv = chttp_do(req, resp_out);
  chttp_request_free(req);
  return rv;
}

ccol_retval_t chttp_delete(const char *url, chttpcli_response **resp_out) {
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;
  if (!url) return ccol_invalid_args;
  chttp_request_t *req = chttp_request_new(CHTTP_DELETE, url, NULL, NULL);
  if (!req) return ccol_not_enough_memory;
  ccol_retval_t rv = chttp_do(req, resp_out);
  chttp_request_free(req);
  return rv;
}

ccol_retval_t chttp_patch(const char *url, const chttp_request_body_t *body,
                          chttpcli_response **resp_out) {
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;
  if (!url) return ccol_invalid_args;
  chttp_request_t *req = chttp_request_new(CHTTP_PATCH, url, body, NULL);
  if (!req) return ccol_not_enough_memory;
  ccol_retval_t rv = chttp_do(req, resp_out);
  chttp_request_free(req);
  return rv;
}

/* ========================================================================== */
/*                         RESPONSE API                                       */
/* ========================================================================== */

/* This writes the lower-case form of name, with its NUL, into stack_buf when
 * it fits there, and into a buffer from the procs of resp when it does not.
 * It returns that buffer, or NULL when the allocation fails, and sets
 * *len_out to the length of the name. The caller frees a returned buffer
 * that is not stack_buf through resp->_m_procs.
 *
 * A caller that makes several lookups of known header names for each
 * response is a realistic and probably hot pattern. A copy on the stack
 * saves one pair of _ccol_mem_alloc and _ccol_mem_free on the heap for each
 * lookup. Every real HTTP header name is far shorter than the stack buffer.
 * That covers the "token" grammar of RFC 9110 section 5.1, every name in
 * the IANA registry, and any realistic custom name. Only a name of a
 * pathological length falls back to the heap. */
enum { CHTTP_RESP_NAME_STACK_BUF = 128 };
static char *_resp_lower_name(const chttpcli_response *resp, const char *name,
                              char *stack_buf, size_t *len_out) {
  size_t nlen = strlen(name);
  char *lower = stack_buf;
  if (nlen >= CHTTP_RESP_NAME_STACK_BUF) {
    lower = (char *)_ccol_mem_alloc(resp->_m_procs, nlen + 1);
    if (!lower) return NULL;
  }
  for (size_t i = 0; i <= nlen; i++)
    lower[i] = (char)tolower((unsigned char)name[i]);
  *len_out = nlen;
  return lower;
}

/* This looks up the lower-case name in the header map of resp. */
static const char *_resp_map_value(const chttpcli_response *resp,
                                   const char *lower, size_t nlen) {
  cmap_pair kp = {.ptr = (void *)lower, .size = nlen + 1};
  const cmap_pair *vp = NULL;
  if (chmap_get_elem_ref((chmap)resp->headers, &kp, &vp) != ccol_success || !vp)
    return NULL;
  return (const char *)vp->ptr;
}

/* This finds occurrence `index` of the lower-case name, and counts every
 * occurrence into *count_out when count_out is not NULL. A repeated name
 * has a record for each occurrence in the field lines of resp. A name that
 * occurs once has only its map entry. */
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
  /* _m_procs points into the block that holds resp itself, so the free of
   * that block goes through a local copy. The header map carries its own
   * copy of the procs. */
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
