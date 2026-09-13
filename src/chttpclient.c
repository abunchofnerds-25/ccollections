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

#include <chashmap.h>
#include <chttp1_parser.h>
#include <chttpclient.h>
#include <cthreadcomm.h>
#include <cthreadpool.h>
#include <ctls.h>
#include <ctype.h>
#include <cvector.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
/* Bounds the number of DISTINCT origin keys the idle pool's chmap will ever
 * hold an entry for, independent of CHTTP_MAX_IDLE_TOTAL/_PER_ORIGIN above
 * (which bound live pooled CONNECTIONS, never the number of distinct
 * origins that have ever had one). Without this, a client that contacts
 * many different origins over its lifetime and never revisits most of them
 * (a crawler, an outbound-heavy service fanning out to many hosts) would
 * accumulate one permanent chmap entry per origin ever seen, since an
 * origin whose pool is later capped-out or aged-out has no other trigger
 * that removes its (by then empty, or never-revisited) entry - the
 * reuse-triggered pruning in _idle_pool_take/_async_idle_pool_take only
 * reclaims an origin's entry once THAT origin is revisited and its pool
 * drained to zero, which never happens for an origin visited exactly once.
 * Once this cap is hit, offering a connection for a genuinely new origin is
 * simply not pooled (matching this file's own established "doesn't fit ->
 * don't pool it, a lost optimisation, never a correctness issue" policy for
 * the other two caps), rather than growing the map further. */
#define CHTTP_MAX_IDLE_ORIGINS 128
#ifdef RUNNING_UNIT_TESTS
/* Overrides CHTTP_MAX_IDLE_ORIGINS's effective value for a test (0 = use the
 * real constant); a real cap of 128 needs 128 genuinely distinct origins to
 * exercise directly, impractical for an ordinary functional test, so a test
 * shrinks this to a small number instead. Set via
 * _chttpclient_set_max_idle_origins_for_tests (near the other white-box test
 * helpers). */
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
 * (or the server's real final response, if it answers directly without
 * waiting) before giving up and sending the body anyway; matches curl's
 * own CURLOPT_EXPECT_100_TIMEOUT_MS default. See chttp_request_t.
 * expect_continue's own doc comment. */
#define CHTTP_100_CONTINUE_WAIT_MS 1000L
/* Soft cap on how many interim (1xx, other than the one being waited for)
 * responses _chttp_read_message_loop will transparently discard before
 * giving up. Without this, a misbehaving or malicious server that never
 * stops emitting interim responses (e.g. an endless stream of "103 Early
 * Hints") could pin a caller thread and its concurrency-limiter slot
 * indefinitely against the default request_timeout_ms == 0 (no timeout)
 * configuration; every other unbounded-repetition vector in this file
 * (redirects, chttp1_parser's own header count/byte budgets) is already
 * capped, and this loop was the one exception. */
#define CHTTP_MAX_INTERIM_RESPONSES 64

/* ========================================================================== */
/*                         INTERNAL TYPES                                     */
/* ========================================================================== */

/* Parsed request-target. All fields are owned copies (mp-allocated).
 *
 * A "http+unix://" URL (see _parse_chttp_url) sets is_unix and
 * unix_socket_path instead of host/port/is_ipv6: host is left NULL, port is
 * left 0, and is_ipv6 is left false for a unix target. origin_key is always
 * populated, using a distinct "unix://<path>" literal prefix (never
 * colliding with a "http://"/"https://" TCP origin_key) so the idle-pool
 * chmaps can key a unix-socket target the same way a TCP one is keyed. */
typedef struct {
  bool is_https;
  bool is_ipv6; /* host is a raw (unbracketed) IPv6 literal; connect()/TLS
                 * need it unbracketed, but the Host header, origin_key, and
                 * redirect-URL reconstruction all need it re-bracketed. */
  bool is_unix;
  char *unix_socket_path; /* owned, percent-decoded; NULL unless is_unix */
  char *host;
  uint16_t port;
  char *path_and_query;
  char *origin_key; /* "scheme://host:port" or "unix://<path>"; used for SNI
                     * (TCP only) and idle-pool keying (both). */
  char *userinfo_authorization; /* owned "Basic <b64>" derived from THIS
                                 * URL's own "user:pass@" component, or NULL
                                 * if the URL had none. */
} chttp_url_t;

/* Absolute-time deadline helper; `active == false` means "no limit". */
typedef struct {
  bool active;
  struct timespec deadline;
} chttp_deadline_t;

/* A single (possibly pooled) connection. Always stack/value-resident (never
 * individually heap-allocated) so it can be stored by value in the idle
 * pool's cvector without an extra allocation layer. */
typedef struct {
  int fd;
  ctls_conn_t *tls; /* NULL for plain HTTP */
  char *origin_key; /* owned copy, matches chttp_url_t.origin_key */
  struct timespec last_used;
} chttp_conn_t;

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  ccol_memmgmt_procs_t *mp;
  bool oom;
  /* max_size == 0 means unlimited (chttpclient_set_max_response_body_size's
   * own default). Checked on every append; the first append that would push
   * len past max_size sets too_large instead of growing further, so a
   * malicious or misbehaving server's oversized body is bounded well before
   * the whole thing is ever buffered in memory. Only meaningful for the
   * buffered (non-streaming) sink; a streaming caller controls its own
   * memory via chttpcli_write_fn's own return value instead. */
  size_t max_size;
  bool too_large;
} chttp_bodybuf_t;

/* Drives one HTTP/1.1 response parse (one hop). A fresh instance is used for
 * every hop of a redirect chain, which is what makes "intermediate redirect
 * headers never leak into the final response" fall out naturally; there is
 * no shared, reset-in-place header map to leak from. */
typedef struct {
  ccol_memmgmt_procs_t *mp;
  chmap headers; /* chmap(char* -> char*); owned until transferred/destroyed */

  bool is_head_request;
  bool will_redirect;
  char *location; /* owned; set only when will_redirect */

  bool message_complete;
  bool trailing_garbage;
  bool error;     /* allocation failure inside a callback */
  bool aborted;   /* sink_fn returned short (streaming caller aborted) */
  bool too_large; /* buffered body exceeded chttpclient_set_max_response_
                   * body_size's configured cap (declared Content-Length
                   * rejected up front in _on_headers_complete, or the
                   * cumulative body rejected reactively in _sink_buffered);
                   * never set for a streaming request, which has no such
                   * cap (see chttp_bodybuf_t.max_size's own comment). */
  int status_code;

  chttpcli_write_fn requested_sink_fn;
  void *requested_sink_ctx;
  chttpcli_write_fn sink_fn; /* resolved once headers are complete */
  void *sink_ctx;
} chttp_parse_ctx_t;

/* ========================================================================== */
/*                         CHTTPCLI HANDLE SLOT TABLE                         */
/* ========================================================================== */

/* chttpcli is an opaque value handle (top 32 bits = slot index, bottom 32
 * bits = generation; see include/chttpclient.h's own doc comment on the
 * typedef), resolved through this table before the underlying struct
 * chttpclient* is ever touched. This is what lets __chttpclient_destroy
 * detect BOTH a concurrent double-destroy (racing another destroy on the
 * same still-live handle) AND a sequential one (a stale handle, from an
 * earlier, already-completed destroy) as a fatal_err rather than a
 * use-after-free/double-free: a slot is marked not-in-use the instant it is
 * released, and its generation is bumped on every reuse, so a stale handle
 * can never alias a later, unrelated client occupying the same slot index.
 *
 * The table's own lock is a read-write lock, not a plain mutex: _chttpcli_
 * resolve (read-only: bounds-check idx, compare generation, read slot->ptr)
 * runs once per outbound request across every API tier (chttpclient_do,
 * _do_async, _do_pooled, ...); _chttpcli_handle_slot_acquire/
 * __chttpclient_destroy (the only mutators) each run once per client's
 * entire lifetime, not once per request. Mirrors cthreadcomm.c's/
 * cthreadpool.c's own identical slot-table rwlock conversions. Unlike
 * those two, this table has no pthread_atfork() protection of its own at
 * all (chttpclient.c registers none), so this conversion carries none of
 * their own TID-tracked-write-lock reinit-in-child subtlety: there being
 * nothing to preserve doesn't change what fork() safety this table already
 * did or didn't have. */
typedef struct {
  struct chttpclient *ptr; /* NULL when slot is free */
  uint32_t generation;     /* minted fresh on every acquire; monotonic per
                               slot index, starts at 0 (pre-first-use),
                               becomes 1 on first acquire */
  bool in_use;
} chttpcli_slot_t;

static struct {
  rw_lock_t rwlock;
  once_flag_t once;
  cvec slots;        /* cvec of chttpcli_slot_t; grows via push_back only,
                         indices permanent once allocated */
  cvec free_indices; /* cvec of uint32_t; LIFO free list, O(1) reuse */
} chttpcli_slot_table = {0};

static void _chttpcli_slot_table_init_globals(void) {
  if (rw_lock_init(chttpcli_slot_table.rwlock) != 0)
    fatal_err("chttpcli slot table: failed to initialize rwlock");
  chttpcli_slot_table.slots = cvector_create(sizeof(chttpcli_slot_t), NULL);
  if (!chttpcli_slot_table.slots)
    fatal_err("chttpcli slot table: failed to allocate slots vector");
  chttpcli_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!chttpcli_slot_table.free_indices)
    fatal_err("chttpcli slot table: failed to allocate free-index vector");
}

struct chttpclient {
  mutex_t lock;
  cond_var_t available;

  /* Pinned by _chttpcli_resolve (lock-free atomic increment) for as long as
   * some caller holds a just-resolved struct chttpclient* it hasn't yet
   * handed off to its own tier-specific protection (in_flight_count /
   * async_in_flight_count); released by _chttpcli_resolve_unpin (under
   * `lock`, together with the broadcast that wakes a waiting destroy; see
   * that function's own comment for why the decrement itself, not just the
   * broadcast, must happen under the lock). __chttpclient_destroy blocks
   * until this reaches 0 before freeing the object, closing a real
   * resolve-then-use race a naive "look up, unlock, return the pointer"
   * resolve step would otherwise leave open. */
  _Atomic size_t pending_resolve_count;

  /* Concurrency limiter: bounds simultaneous in-flight requests. */
  size_t pool_cap;
  size_t configured_pool_size;
  size_t in_flight_count;
  bool pool_initialized;
  bool destroying;

  /* Keep-alive idle pool: chmap(char *origin_key -> cvec of chttp_conn_t),
   * independent of the concurrency limiter above. */
  chmap idle_pools;
  size_t idle_total_count;

  /* Tier 2's own keep-alive idle pool: chmap(char *origin_key -> cvec of
   * chttp_async_ctx_t*). Separate from idle_pools above since the stored
   * value type differs (a plain fd this thread owns vs. a connection
   * attached to the shared async reactor); see the "ASYNC IDLE POOL"
   * section further down for the full design. idle_async_drained is
   * broadcast whenever idle_total_count_async reaches 0, so
   * __chttpclient_destroy can wait for any in-flight idle-connection
   * teardowns (triggered by its own drain, or already in progress from a
   * natural death) to actually finish before freeing this struct out from
   * under them. */
  chmap idle_pools_async;
  size_t idle_total_count_async;
  cond_var_t idle_async_drained;

  /* Tracks active (not-yet-idle-pooled) Tier 2/3 async chains created for
   * THIS client: incremented once per chain in _async_chain_create,
   * decremented in _async_chain_release once a chain's refcount reaches
   * zero (its last hop is torn down or joins the idle pool above).
   * Independent of both in_flight_count above (Tier 1 only) and
   * idle_total_count_async (idle-pooled connections are the OPPOSITE of
   * in-flight). Guarded by a DEDICATED leaf lock/condvar, deliberately
   * never cli->lock/available: _async_chain_release (the only place this is
   * decremented) runs as often from inside an event_loop dispatch callback,
   * already holding that registration's own dispatch_lock, as it does from
   * a safe synchronous context; reusing cli->lock here would introduce a
   * brand new dispatch_lock -> cli->lock ordering this module has otherwise
   * never needed to reason about, for no benefit over a lock that is never
   * held across any other call. __chttpclient_destroy waits on
   * async_count_drained until this reaches zero before freeing cli, closing
   * a real use-after-free: chain->cli/ctx->cli are dereferenced throughout
   * an active hop's lifecycle (cli->lock, cli->idle_pools_async,
   * cli->m_procs), well after chttpclient_do_async/_streaming has already
   * returned to the caller. */
  mutex_t async_count_lock;
  cond_var_t async_count_drained;
  size_t async_in_flight_count;

  long connect_timeout_ms;
  long request_timeout_ms;
  /* Caps the buffered (non-streaming) response body size across all three
   * tiers; 0 (the default) means unlimited. See chttpclient_set_max_
   * response_body_size's own doc comment. Read under cli->lock into a
   * per-request/per-chain snapshot (chttp_do_internal's local, or
   * chttp_async_chain_t.max_response_body_size), exactly like connect_
   * timeout_ms/request_timeout_ms above, rather than re-read mid-flight. */
  size_t max_response_body_size;
  chttp_tls_config_t tls;
  char *owned_cert_path;
  char *owned_key_path;
  char *owned_ca_bundle_path;
  ctls_ctx_t *tls_ctx; /* rebuilt whenever chttpclient_set_tls is called */
  bool tls_ctx_usable; /* false if the configured cert/key/ca paths are not
                          readable; see _rebuild_tls_ctx_locked */

  ccol_memmgmt_procs_t *m_procs;
};

/* ========================================================================== */
/*                    CHTTPCLI HANDLE RESOLVE / UNPIN                         */
/* ========================================================================== */

/* Resolves h and pins the result against concurrent destroy, or returns
 * NULL if h is 0, garbage, or references a currently-free or
 * already-reused (wrong-generation) slot. On success, the caller MUST call
 * _chttpcli_resolve_unpin(result) exactly once, as soon as its own
 * tier-specific protection (in_flight_count / async_in_flight_count) has
 * taken over, or immediately if the call is short and non-blocking. */
static struct chttpclient *_chttpcli_resolve(chttpcli h) {
  call_once(chttpcli_slot_table.once, _chttpcli_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  rw_lock_rdlock(chttpcli_slot_table.rwlock);
  struct chttpclient *raw = NULL;
  if (idx < cvector_elem_count(chttpcli_slot_table.slots)) {
    chttpcli_slot_t *slot =
        (chttpcli_slot_t *)cvector_at(chttpcli_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  /* No raw->lock acquisition here at all, so nothing can ever block while
   * chttpcli_slot_table.rwlock is held; a nested-lock version would let a
   * slow, per-client operation holding raw->lock (e.g. chttpclient_set_
   * tls's blocking disk I/O in _rebuild_tls_ctx_locked) transiently stall
   * every other client's resolve calls process-wide. Safe because raw is
   * guaranteed still-allocated here regardless: the only thing that could
   * make it unsafe to touch, __chttpclient_destroy's slot-release step,
   * also requires chttpcli_slot_table.rwlock's write side, which cannot
   * run concurrently with this read side regardless. */
  if (raw) atomic_fetch_add(&raw->pending_resolve_count, 1);
  rw_lock_unlock(chttpcli_slot_table.rwlock);
  return raw;
}

static void _chttpcli_resolve_unpin(struct chttpclient *raw) {
  /* The decrement itself MUST happen under raw->lock, not as a bare atomic
   * op outside it: a bare-atomic decrement could bring pending_resolve_
   * count to 0 before this function acquires raw->lock, letting a
   * concurrent __chttpclient_destroy acquire raw->lock first, see BOTH
   * pending_resolve_count == 0 and in_flight_count == 0 true on its very
   * first check (never entering cond_var_wait at all), and proceed straight
   * through teardown (including mutex_destroy(raw->lock) and freeing raw)
   * before this function ever calls mutex_lock(raw->lock), which would
   * then be a use-after-free. The standard condition-variable pattern
   * requires the signaling side to modify the predicate AND broadcast under
   * the SAME lock the waiter uses for its own predicate-check-and-sleep;
   * moving only the decrement outside the lock does not satisfy that. */
  mutex_lock(raw->lock);
  atomic_fetch_sub(&raw->pending_resolve_count, 1);
  cond_var_broadcast(raw->available); /* wake a destroy waiting on this */
  mutex_unlock(raw->lock);
  /* Note the asymmetry with _chttpcli_resolve's own increment, which
   * correctly remains a bare atomic op with no raw->lock acquisition at
   * all: the increment side can never cause a lost wakeup (it only ever
   * makes the wait predicate MORE true, never flips it from true to false),
   * so it has no need to synchronize with a sleeper. This function is
   * always called standalone, after _chttpcli_resolve has already released
   * chttpcli_slot_table.rwlock, so this raw->lock acquisition is never
   * nested inside the slot table's global lock; an entirely ordinary
   * per-object lock use, identical in shape to every other
   * mutex_lock(cli->lock) call already in this file. */
}

/* Allocates a fresh slot (or reuses a freed one) for cli and returns the
 * resulting handle, or 0 on OOM. Called once, from create_chttpclient_mp,
 * after the object is otherwise fully constructed. */
static chttpcli _chttpcli_handle_slot_acquire(struct chttpclient *cli) {
  call_once(chttpcli_slot_table.once, _chttpcli_slot_table_init_globals);
  rw_lock_wrlock(chttpcli_slot_table.rwlock);
  uint32_t idx;
  chttpcli_slot_t *slot;
  if (cvector_elem_count(chttpcli_slot_table.free_indices) > 0) {
    cvector_pop_back(chttpcli_slot_table.free_indices, &idx);
    slot = (chttpcli_slot_t *)cvector_at(chttpcli_slot_table.slots, idx);
  } else {
    chttpcli_slot_t fresh = {0};
    if (cvector_push_back(chttpcli_slot_table.slots, &fresh) != ccol_success) {
      rw_lock_unlock(chttpcli_slot_table.rwlock);
      return 0; /* ordinary, non-fatal OOM */
    }
    idx = (uint32_t)cvector_elem_count(chttpcli_slot_table.slots) - 1;
    slot = (chttpcli_slot_t *)cvector_at(chttpcli_slot_table.slots, idx);
  }
  slot->generation++;
  if (slot->generation == 0)
    slot->generation++; /* skip the one value that
would collide with CHTTPCLI_INVALID after ~2^32 reuses of this exact
slot index; see this function's own history in the design plan for
why this is closed outright rather than left as residual risk */
  slot->ptr = cli;
  slot->in_use = true;
  chttpcli h = ((chttpcli)idx << 32) | (chttpcli)slot->generation;
  rw_lock_unlock(chttpcli_slot_table.rwlock);
  return h;
}

/* ========================================================================== */
/*                         DEFAULT CLIENT                                     */
/* ========================================================================== */

/* client is _Atomic so __chttpclient_destroy can safely clear it via a
 * compare-and-swap if the handle it's given happens to be this singleton
 * (see __chttpclient_destroy's own comment on this); chttp_default_client
 * reads it with a plain atomic_load, matching this file's existing
 * _Atomic(event_reg) precedent rather than adding a new lock for a single
 * value. once never resets: this module never re-initialises the default
 * client, deliberately (see chttp_default_client's own doc comment). */
static struct {
  _Atomic(chttpcli) client;
  once_flag_t once;
} default_client_bundler = {0};

/* ========================================================================== */
/*                         URL PARSING                                        */
/* ========================================================================== */

static void _url_free(ccol_memmgmt_procs_t *mp, chttp_url_t *u) {
  if (!u) return;
  _mem_free(mp, u->unix_socket_path);
  _mem_free(mp, u->host);
  _mem_free(mp, u->path_and_query);
  _mem_free(mp, u->origin_key);
  _mem_free(mp, u->userinfo_authorization);
  memset(u, 0, sizeof(*u));
}

/* Percent-encodes a unix socket path into a newly allocated NUL-terminated
 * string, for reconstructing a "http+unix://<path>" URL string (origin_key,
 * a redirect target resolved against a unix-socket base). Mirrors the
 * percent-encoding a real client library would apply to any URL authority
 * component: every byte outside an unreserved/sub-delim set is escaped,
 * '/' included (the whole point of this scheme is packing a path that
 * itself contains '/' into a single authority component). */
/* Does the actual work for _percent_encode_unix_path, taking `len` as an
 * explicit parameter (rather than computing it via strlen(path) itself) so
 * the overflow guard below can be exercised directly, with a fake `len`,
 * from a white-box test without needing to actually construct a
 * multi-exabyte path string; see
 * _chttp_percent_encode_unix_path_overflow_guard_for_tests below. */
static char *_percent_encode_unix_path_len(ccol_memmgmt_procs_t *mp,
                                           const char *path, size_t len) {
  static const char *unreserved =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
  /* Overflow guard on the "needed size" computation below, mirroring the
   * SIZE_MAX-relative guard idiom this file's other growable/percent-encoded
   * buffers (chttp_base64_encode_mp, _ob_append) already use for the
   * identical class of computation; without it, a path long enough to make
   * len * 3 + 1 wrap past SIZE_MAX would let this function under-allocate
   * `out` and then write past the end of it in the loop below. Unreachable
   * in practice (it would require an already multi-exabyte path string
   * resident in memory before this function is ever called), but this
   * project treats a reducible/unguarded overflow in a size computation as
   * a real bug regardless of how large an input is needed to trigger it. */
  if (len > (SIZE_MAX - 1) / 3) return NULL;
  char *out = (char *)_mem_alloc(mp, len * 3 + 1);
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

/* Percent-encodes a unix socket path into a newly allocated NUL-terminated
 * string, for reconstructing a "http+unix://<path>" URL string (origin_key,
 * a redirect target resolved against a unix-socket base). Mirrors the
 * percent-encoding a real client library would apply to any URL authority
 * component: every byte outside an unreserved/sub-delim set is escaped,
 * '/' included (the whole point of this scheme is packing a path that
 * itself contains '/' into a single authority component). */
static char *_percent_encode_unix_path(ccol_memmgmt_procs_t *mp,
                                       const char *path) {
  return _percent_encode_unix_path_len(mp, path, strlen(path));
}

#ifdef RUNNING_UNIT_TESTS
/*
 * White-box test helper exposing _percent_encode_unix_path_len's overflow
 * guard directly, mirroring _chttp_ob_append_overflow_guard_for_tests's own
 * established pattern for this exact class of guard: fake_len stands in for
 * strlen(path) without a test ever having to construct a real multi-exabyte
 * path string. ONLY safe to call with a fake_len that actually exceeds the
 * guard's own threshold ((SIZE_MAX - 1) / 3): the guard then rejects before
 * the loop ever runs, so `path` need only be readable for whatever small,
 * real prefix a test happens to pass, regardless of how large fake_len
 * claims to be. A fake_len that does NOT exceed the threshold would make
 * the loop below index into `path` up to fake_len bytes, a real
 * out-of-bounds read for any ordinary small test buffer; this helper exists
 * solely to exercise the rejection path, not the ordinary success path
 * (already covered by every other _chttp_resolve_redirect_url_for_tests-
 * based unix-socket test). Not part of the public API; gated so this symbol
 * does not leak into a production build of libccollections.so, matching
 * every other white-box helper in this file.
 */
bool _chttp_percent_encode_unix_path_overflow_guard_for_tests(
    ccol_memmgmt_procs_t *mp, const char *path, size_t fake_len) {
  char *r = _percent_encode_unix_path_len(mp, path, fake_len);
  bool rejected = (r == NULL);
  _mem_free(mp, r);
  return rejected;
}
#endif /* RUNNING_UNIT_TESTS */

/* Percent-decodes [start, start+len) into a newly allocated NUL-terminated
 * string. Used only for userinfo components; path/query bytes are never
 * decoded, since they must be forwarded to the server exactly as received.
 * Rejects a malformed escape ('%' not followed by 2 hex digits) and a
 * decoded embedded NUL byte: silently truncating a password at a NUL would
 * produce a subtly wrong Authorization header instead of a clear error. */
static ccol_retval_t _percent_decode_component(ccol_memmgmt_procs_t *mp,
                                               const char *start, size_t len,
                                               char **out) {
  char *buf = (char *)_mem_alloc(mp, len + 1);
  if (!buf) return ccol_not_enough_memory;
  size_t w = 0;
  for (size_t i = 0; i < len; i++) {
    char c = start[i];
    if (c == '%') {
      if (i + 2 >= len || !isxdigit((unsigned char)start[i + 1]) ||
          !isxdigit((unsigned char)start[i + 2])) {
        _mem_free(mp, buf);
        return ccol_http_invalid_url;
      }
      char hex[3] = {start[i + 1], start[i + 2], '\0'};
      int v = (int)strtol(hex, NULL, 16);
      if (v == 0) {
        _mem_free(mp, buf);
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

/*
 * Parses the authority component of a "http+unix://" URL: p points just past
 * the "http+unix://" prefix, at a percent-encoded filesystem path (e.g.
 * "%2Fvar%2Frun%2Fapp.sock"), matching Python's requests-unixsocket
 * convention. Only "http+unix://" is recognised; "https+unix://" is not (TLS
 * over a local socket has no real use case) and simply falls through to the
 * caller's own ccol_http_invalid_url return, since it matches no recognised
 * prefix at all. No userinfo ("user:pass@") support for this scheme: there is
 * no established convention combining the two, and the percent-encoded path
 * itself may legitimately contain '@' bytes once decoded.
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
    _mem_free(mp, unix_path);
    return ccol_http_invalid_url;
  }

  const char *q = authority_end;
  const char *pq = (*q && *q != '#') ? q : "/";
  size_t pq_raw_len = strlen(pq);
  const char *frag = memchr(pq, '#', pq_raw_len);
  size_t pq_len = frag ? (size_t)(frag - pq) : pq_raw_len;

  /* Same CRLF-injection concern as _parse_chttp_url's identical check: an
   * unescaped CR or LF byte here would be written verbatim onto the wire as
   * the request line's request-target by _serialize_request, letting a
   * caller-constructed "http+unix://" URL string inject extra header lines
   * or a smuggled second request. */
  if (memchr(pq, '\r', pq_len) || memchr(pq, '\n', pq_len)) {
    _mem_free(mp, unix_path);
    return ccol_http_invalid_url;
  }

  char *path_and_query;
  if (pq_len > 0 && pq[0] == '/') {
    path_and_query = (char *)_mem_alloc(mp, pq_len + 1);
    if (!path_and_query) {
      _mem_free(mp, unix_path);
      return ccol_not_enough_memory;
    }
    memcpy(path_and_query, pq, pq_len);
    path_and_query[pq_len] = '\0';
  } else {
    path_and_query = (char *)_mem_alloc(mp, pq_len + 2);
    if (!path_and_query) {
      _mem_free(mp, unix_path);
      return ccol_not_enough_memory;
    }
    path_and_query[0] = '/';
    memcpy(path_and_query + 1, pq, pq_len);
    path_and_query[pq_len + 1] = '\0';
  }

  int needed = snprintf(NULL, 0, "unix://%s", unix_path);
  if (needed < 0) {
    _mem_free(mp, unix_path);
    _mem_free(mp, path_and_query);
    return ccol_unexpected_failure;
  }
  char *origin_key = (char *)_mem_alloc(mp, (size_t)needed + 1);
  if (!origin_key) {
    _mem_free(mp, unix_path);
    _mem_free(mp, path_and_query);
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

/* Returns an owned "[host]" for an IPv6 literal, or a plain strdup
 * otherwise. Used wherever a host needs to round-trip through a URL string
 * (origin_key, a reconstructed redirect URL); connect()/TLS always use
 * chttp_url_t.host directly, which stays unbracketed. */
static char *_format_bracketed_host(ccol_memmgmt_procs_t *mp, const char *host,
                                    bool is_ipv6) {
  if (!is_ipv6) return ccol_strdup(mp, host);
  size_t hlen = strlen(host);
  /* Overflow guard on hlen + 3, mirroring every other size-computing
   * allocation in this file's URL/redirect-resolution helpers
   * (_percent_encode_unix_path_len, _merge_ref_path, _concat_len), all of
   * which guard this identical class of computation regardless of how
   * unreachable an input large enough to actually trigger it would be in
   * practice (this project's own standing policy: a reducible/unguarded
   * overflow in a size computation is a real bug regardless of how large
   * an input is needed to trigger it). Without it, hlen >= SIZE_MAX - 2
   * would wrap the allocation size, under-allocate `out`, and the memcpy
   * below would write past its end. */
  if (hlen > SIZE_MAX - 3) return NULL;
  char *out = (char *)_mem_alloc(mp, hlen + 3);
  if (!out) return NULL;
  out[0] = '[';
  memcpy(out + 1, host, hlen);
  out[hlen + 1] = ']';
  out[hlen + 2] = '\0';
  return out;
}

/*
 * URL parser: recognises "http://"/"https://", an optional "user:pass@"
 * userinfo prefix (also "user@" or ":pass@"; both syntactically valid RFC
 * 3986 forms) that is turned into a ready-to-send "Basic <base64>"
 * Authorization value, a plain reg-name/IPv4 host or a bracketed IPv6
 * literal ("[::1]"), an optional ":port", and a path/query with any
 * trailing "#fragment" discarded; fragments are never sent to a server
 * (RFC 3986 SS3.5), so silently forwarding one in the request line (the
 * previous behavior) was a real correctness bug, not a documented scope
 * choice.
 *
 * Not attempted: userinfo/hostname are not validated beyond basic syntax;
 * an implausible bracketed literal or reg-name is simply handed to
 * getaddrinfo()/inet_pton() at connect time and surfaces there as
 * ccol_http_host_resolution_failed, exactly like today's unvalidated
 * hostnames.
 *
 * Also recognises "http+unix://<percent-encoded-path>[/path][?query]" (see
 * _parse_chttp_unix_url); "https+unix://" is not recognised and falls
 * through to ccol_http_invalid_url below, same as any other unrecognised
 * scheme.
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

  /* Authority = [ userinfo "@" ] host [ ":" port ], terminated by the first
   * '/', '?', '#', or end of string. */
  const char *authority_end = p;
  while (*authority_end && *authority_end != '/' && *authority_end != '?' &&
         *authority_end != '#')
    authority_end++;

  /* Last unescaped '@' in the authority: tolerates an unescaped '@' inside a
   * lazily-encoded password, matching common real-world parser leniency. */
  const char *last_at = NULL;
  for (const char *s = p; s < authority_end; s++)
    if (*s == '@') last_at = s;

  char *userinfo_authorization = NULL;
  const char *host_scan_start = p;
  if (last_at) {
    /* Delimiters ('@', ':') are located in the RAW (still percent-encoded)
     * string, then each half is decoded independently; decoding first and
     * searching second would incorrectly split on a decoded '@'/':' that
     * was meant to be literal password content. */
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
      _mem_free(mp, user_dec);
      _mem_free(mp, pass_dec);
      return drv;
    }

    userinfo_authorization = chttp_basic_auth_mp(mp, user_dec, pass_dec);
    _mem_free(mp, user_dec);
    _mem_free(mp, pass_dec);
    if (!userinfo_authorization) return ccol_not_enough_memory;

    host_scan_start = last_at + 1;
  }

  bool is_ipv6 = false;
  const char *host_start;
  const char *q;
  if (*host_scan_start == '[') {
    const char *close = host_scan_start + 1;
    while (*close && *close != ']') close++;
    if (*close != ']') {
      _mem_free(mp, userinfo_authorization);
      return ccol_http_invalid_url;
    }
    char after = close[1];
    if (after != '\0' && after != ':' && after != '/' && after != '?' &&
        after != '#') {
      _mem_free(mp, userinfo_authorization);
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
    _mem_free(mp, userinfo_authorization);
    return ccol_http_invalid_url;
  }
  /* Unlike ':'/'/'/'?'/'#' above, a raw CR or LF byte does not terminate the
   * host scan (neither is a valid host character, but this parser tolerates
   * unusual input rather than fully validating reg-name/IPv4 syntax; see
   * this function's own doc comment). Left unchecked, such a byte would be
   * carried verbatim into url->host and then into the synthesized "host: "
   * header line in _serialize_request, letting a caller-constructed URL
   * string with embedded control characters inject extra header lines onto
   * the wire (the same CRLF-injection concern chttp_request_set_header's
   * own check exists for, reached through the URL instead of a header). */
  if (memchr(host_start, '\r', host_len) ||
      memchr(host_start, '\n', host_len)) {
    _mem_free(mp, userinfo_authorization);
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
        _mem_free(mp, userinfo_authorization);
        return ccol_http_invalid_url;
      }
      q++;
      pd++;
    }
    if (pd == 0 || pv == 0) {
      _mem_free(mp, userinfo_authorization);
      return ccol_http_invalid_url;
    }
    /* The digit loop above stops at the first non-digit byte, whatever it
     * is; without this check, trailing garbage right after a syntactically
     * valid port (e.g. "http://host:80abc/get") would silently fall through
     * into the path/query computation below as though "abc/get" were the
     * path, sending the request to a different target than the URL string
     * names instead of being rejected. */
    if (*q != '\0' && *q != '/' && *q != '?' && *q != '#') {
      _mem_free(mp, userinfo_authorization);
      return ccol_http_invalid_url;
    }
    port = (uint16_t)pv;
  }

  /* A fragment is a hard, unconditional delimiter; no escaping semantics
   * apply to '#' itself, since a fragment start is never quoted. */
  const char *pq = (*q && *q != '#') ? q : "/";
  size_t pq_raw_len = strlen(pq);
  const char *frag = memchr(pq, '#', pq_raw_len);
  size_t pq_len = frag ? (size_t)(frag - pq) : pq_raw_len;

  /* Same CRLF-injection concern as the host check above, reached through the
   * path/query component instead: path_and_query is written verbatim onto
   * the wire as the request line's request-target by _serialize_request
   * ("METHOD <path_and_query> HTTP/1.1\r\n"), with no further escaping. An
   * unescaped CR or LF byte here (as opposed to its percent-encoded form,
   * which is preserved as ordinary path/query content and forwarded to the
   * server unchanged, per this function's own doc comment) would let a
   * caller-constructed URL string terminate the request line early and
   * inject arbitrary extra header lines, or a whole smuggled second
   * request, exactly the class of bug the host check above already guards
   * against. */
  if (memchr(pq, '\r', pq_len) || memchr(pq, '\n', pq_len)) {
    _mem_free(mp, userinfo_authorization);
    return ccol_http_invalid_url;
  }

  char *host = (char *)_mem_alloc(mp, host_len + 1);
  if (!host) {
    _mem_free(mp, userinfo_authorization);
    return ccol_not_enough_memory;
  }
  memcpy(host, host_start, host_len);
  host[host_len] = '\0';

  char *path_and_query;
  if (pq_len > 0 && pq[0] == '/') {
    path_and_query = (char *)_mem_alloc(mp, pq_len + 1);
    if (!path_and_query) {
      _mem_free(mp, host);
      _mem_free(mp, userinfo_authorization);
      return ccol_not_enough_memory;
    }
    memcpy(path_and_query, pq, pq_len);
    path_and_query[pq_len] = '\0';
  } else {
    /* pq is empty, or starts with '?' (query with no path component);
     * synthesise a leading '/'. */
    path_and_query = (char *)_mem_alloc(mp, pq_len + 2);
    if (!path_and_query) {
      _mem_free(mp, host);
      _mem_free(mp, userinfo_authorization);
      return ccol_not_enough_memory;
    }
    path_and_query[0] = '/';
    memcpy(path_and_query + 1, pq, pq_len);
    path_and_query[pq_len + 1] = '\0';
  }

  char *bracketed_host = _format_bracketed_host(mp, host, is_ipv6);
  if (!bracketed_host) {
    _mem_free(mp, host);
    _mem_free(mp, path_and_query);
    _mem_free(mp, userinfo_authorization);
    return ccol_not_enough_memory;
  }

  int needed = snprintf(NULL, 0, "%s://%s:%u", https ? "https" : "http",
                        bracketed_host, (unsigned)port);
  if (needed < 0) {
    _mem_free(mp, host);
    _mem_free(mp, path_and_query);
    _mem_free(mp, bracketed_host);
    _mem_free(mp, userinfo_authorization);
    return ccol_unexpected_failure;
  }
  char *origin_key = (char *)_mem_alloc(mp, (size_t)needed + 1);
  if (!origin_key) {
    _mem_free(mp, host);
    _mem_free(mp, path_and_query);
    _mem_free(mp, bracketed_host);
    _mem_free(mp, userinfo_authorization);
    return ccol_not_enough_memory;
  }
  snprintf(origin_key, (size_t)needed + 1, "%s://%s:%u",
           https ? "https" : "http", bracketed_host, (unsigned)port);
  _mem_free(mp, bracketed_host);

  out->is_https = https;
  out->is_ipv6 = is_ipv6;
  out->host = host;
  out->port = port;
  out->path_and_query = path_and_query;
  out->origin_key = origin_key;
  out->userinfo_authorization = userinfo_authorization;
  return ccol_success;
}

/* RFC 3986 SS5.2.4 "Remove Dot Segments". Operates on a path only; never
 * on a query string, since a literal ".."/"." inside query bytes must never
 * be reinterpreted as path navigation; callers strip/re-append any
 * "?query" before/after calling this. */
static char *_remove_dot_segments(ccol_memmgmt_procs_t *mp, const char *path) {
  size_t len = strlen(path);
  char *in = (char *)_mem_alloc(mp, len + 1);
  if (!in) return NULL;
  memcpy(in, path, len + 1);
  char *out = (char *)_mem_alloc(mp, len + 1);
  if (!out) {
    _mem_free(mp, in);
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
  _mem_free(mp, in);
  return out;
}

/* RFC 3986 SS5.3 "merge" (path component only; the caller splices the
 * reference's own query string back on afterward, once dot-segment removal
 * has run; see _remove_dot_segments' own comment for why). */
static char *_merge_ref_path(ccol_memmgmt_procs_t *mp,
                             const char *base_path_and_query,
                             const char *ref_path, size_t ref_path_len) {
  const char *base_query = strchr(base_path_and_query, '?');
  size_t base_path_len = base_query ? (size_t)(base_query - base_path_and_query)
                                    : strlen(base_path_and_query);

  size_t dir_len;
  if (ref_path_len == 0) {
    /* A query-only reference ("?x") reuses the base path verbatim. */
    dir_len = base_path_len;
  } else {
    const char *last_slash = NULL;
    for (size_t i = 0; i < base_path_len; i++)
      if (base_path_and_query[i] == '/') last_slash = &base_path_and_query[i];
    dir_len = last_slash ? (size_t)(last_slash - base_path_and_query) + 1 : 0;
  }

  /* Overflow guard on the "needed size" computation below, mirroring
   * _ob_append's/_sink_buffered's own identical guard for the identical
   * class of computation: dir_len and ref_path_len are two INDEPENDENTLY
   * sized values (a redirect base path and a Location reference's own
   * path), so unlike a single strlen() result, their sum reaching close to
   * SIZE_MAX does not require either one alone to be implausibly large;
   * unchecked, it would let this allocation under-size `out` and then the
   * memcpy calls below write past its end. */
  if (ref_path_len > SIZE_MAX - dir_len ||
      dir_len + ref_path_len > SIZE_MAX - 1)
    return NULL;
  char *out = (char *)_mem_alloc(mp, dir_len + ref_path_len + 1);
  if (!out) return NULL;
  memcpy(out, base_path_and_query, dir_len);
  memcpy(out + dir_len, ref_path, ref_path_len);
  out[dir_len + ref_path_len] = '\0';
  return out;
}

#ifdef RUNNING_UNIT_TESTS
/*
 * White-box test helper exposing _merge_ref_path's overflow guard directly,
 * mirroring _chttp_percent_encode_unix_path_overflow_guard_for_tests's own
 * established pattern: _merge_ref_path already takes ref_path_len as an
 * explicit parameter, so fake_ref_path_len can simply be passed straight
 * through with no further refactoring needed. ONLY safe to call with a
 * fake_ref_path_len that actually exceeds the guard's own threshold, so the
 * guard rejects before either memcpy call ever runs; see that sibling
 * helper's own doc comment for the identical out-of-bounds-read hazard a
 * non-rejecting fake length would otherwise create here. Not part of the
 * public API; gated so this symbol does not leak into a production build of
 * libccollections.so, matching every other white-box helper in this file.
 */
bool _chttp_merge_ref_path_overflow_guard_for_tests(
    ccol_memmgmt_procs_t *mp, const char *base_path_and_query,
    const char *ref_path, size_t fake_ref_path_len) {
  char *r =
      _merge_ref_path(mp, base_path_and_query, ref_path, fake_ref_path_len);
  bool rejected = (r == NULL);
  _mem_free(mp, r);
  return rejected;
}
#endif /* RUNNING_UNIT_TESTS */

/* Concatenates a[0..a_len) and b[0..b_len) into a newly allocated,
 * NUL-terminated string. a_len/b_len are taken as explicit parameters
 * (rather than computed internally via strlen) purely so the overflow
 * guard below can be exercised directly from a white-box test with a fake
 * length, without needing to actually construct a multi-exabyte string;
 * see _chttp_concat_len_overflow_guard_for_tests below. Used by
 * _resolve_redirect_url to splice a resolved path back together with a
 * reference's own query string. */
static char *_concat_len(ccol_memmgmt_procs_t *mp, const char *a, size_t a_len,
                         const char *b, size_t b_len) {
  /* Overflow guard on the "needed size" computation below, mirroring
   * _merge_ref_path's own identical guard just above: a_len and b_len are
   * two independently sized values, so their sum reaching close to
   * SIZE_MAX does not require either one alone to be implausibly large;
   * unchecked, it would let this allocation under-size `out` and then the
   * memcpy calls below write past its end. */
  if (b_len > SIZE_MAX - a_len || a_len + b_len > SIZE_MAX - 1) return NULL;
  char *out = (char *)_mem_alloc(mp, a_len + b_len + 1);
  if (!out) return NULL;
  memcpy(out, a, a_len);
  memcpy(out + a_len, b, b_len);
  out[a_len + b_len] = '\0';
  return out;
}

#ifdef RUNNING_UNIT_TESTS
/*
 * White-box test helper exposing _concat_len's overflow guard directly.
 * ONLY safe to call with a fake_a_len/fake_b_len pair that actually exceeds
 * the guard's own threshold, so the guard rejects before either memcpy call
 * ever runs; see _chttp_merge_ref_path_overflow_guard_for_tests's own doc
 * comment for the identical out-of-bounds-read hazard a non-rejecting fake
 * length would otherwise create here. Not part of the public API; gated so
 * this symbol does not leak into a production build of libccollections.so,
 * matching every other white-box helper in this file.
 */
bool _chttp_concat_len_overflow_guard_for_tests(ccol_memmgmt_procs_t *mp,
                                                const char *a,
                                                size_t fake_a_len,
                                                const char *b,
                                                size_t fake_b_len) {
  char *r = _concat_len(mp, a, fake_a_len, b, fake_b_len);
  bool rejected = (r == NULL);
  _mem_free(mp, r);
  return rejected;
}
#endif /* RUNNING_UNIT_TESTS */

/*
 * True if `s` begins with an RFC 3986 SS3.1 "scheme ':'"
 * (ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":"), scanning only up to the
 * first '/', '?', or '#'; a colon appearing after any of those is
 * ordinary path/query/fragment content, never a scheme delimiter (e.g.
 * "/a:b" and "?a:b" have no scheme). Used by _resolve_redirect_url to
 * detect a Location value that is an absolute-URI reference using a scheme
 * this client does not otherwise special-case (http/https/http+unix, all
 * handled separately via a plain prefix check); per RFC 3986 SS5.2.2, ANY
 * reference with a scheme is absolute (T = R) regardless of whether the
 * scheme is one this client actually knows how to fetch.
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
 * Resolves a Location header against the current hop's URL per RFC 3986
 * SS5.2-5.3: absolute URLs, protocol-relative references ("//host/path"),
 * absolute-path references ("/foo"), and general relative-path references
 * ("foo", "../foo", "./foo", "?query") are all supported. Returns NULL
 * (caller treats as ccol_http_transfer_aborted) only for a NULL/empty
 * location; every other syntactically plausible Location value resolves
 * to *some* absolute URL, exactly like a real browser or curl.
 *
 * The result is always re-parsed by _parse_chttp_url on the next hop, so
 * this function does not need to know anything about userinfo/credential
 * carry-forward; that is handled by each tier's own hop loop. This is also
 * what makes an absolute-URI reference using an unrecognised scheme (see
 * _location_has_scheme below) come out right without this function needing
 * to know anything about which schemes _parse_chttp_url accepts: returning
 * it verbatim lets the next hop's _parse_chttp_url reject it with the exact
 * same ccol_http_invalid_url an unsupported top-level request URL already
 * gets, rather than this function silently mis-resolving it.
 *
 * Dot-segment removal (RFC 3986 SS5.2.4) is applied to the path component
 * only; a query string is always split off first and re-appended verbatim
 * afterward, on every branch below that can produce one, so a query value
 * that happens to contain "/", "..", or "." bytes is never reinterpreted as
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
  /* An absolute-URI reference whose scheme is none of the three above (e.g.
   * "g:h", "mailto:x@y", "ftp://host/path"): RFC 3986 SS5.2.2 says T = R
   * unconditionally once R has a scheme, so this must never fall through to
   * the relative-reference handling below, which would otherwise merge the
   * whole "scheme:opaque" string onto the CURRENT origin's path as though it
   * were a same-origin relative path (e.g. "g:h" against
   * "http://a/b/c/d;p?q" silently became "http://a/b/c/g:h" instead of being
   * recognised as the absolute reference "g:h" it actually is). Returned
   * verbatim, exactly like the three recognised-scheme prefixes just above;
   * see this function's own doc comment for why the next hop's
   * _parse_chttp_url is what actually rejects it. */
  if (_location_has_scheme(location)) return ccol_strdup(mp, location);

  const char *scheme = base->is_https ? "https" : "http";

  if (location[0] == '/' && location[1] == '/') {
    /* Protocol-relative reference: unambiguously names a (possibly
     * different) network host to redirect to, regardless of whether the
     * base was a unix-socket target; borrows the base's scheme (always
     * "http" for a unix base, since is_https is always false there). The
     * rest is already a well-formed authority+path for the next
     * _parse_chttp_url call. Not dot-segment-normalised here, exactly like
     * the fully absolute-URL case above isn't either. */
    int needed = snprintf(NULL, 0, "%s:%s", scheme, location);
    if (needed < 0) return NULL;
    char *out = (char *)_mem_alloc(mp, (size_t)needed + 1);
    if (!out) return NULL;
    snprintf(out, (size_t)needed + 1, "%s:%s", scheme, location);
    return out;
  }

  /* RFC 3986 SS3.5: a fragment is never sent to a server and (per SS5.3's
   * own reference-resolution algorithm) is not part of R.path/R.query
   * either; it must be discarded here, before any merge or dot-segment
   * removal runs, not merely relied upon to be stripped later by the next
   * hop's _parse_chttp_url call the way the absolute-URL and
   * protocol-relative branches above get away with. Left unstripped, a
   * fragment containing its own "/../" bytes (e.g. "g#/../h") would have
   * those bytes walked as real path navigation by _remove_dot_segments
   * below, silently resolving to the wrong target; a fragment-only
   * reference ("#s") would likewise be misrouted into the merge branch
   * with a non-empty ref_path instead of correctly reusing the base path
   * verbatim. No escaping semantics apply to '#' itself (matching
   * _parse_chttp_url's identical treatment of a fragment delimiter), so the
   * first raw '#' unconditionally starts the fragment. */
  const char *frag = strchr(location, '#');
  size_t loc_len_nf = frag ? (size_t)(frag - location) : strlen(location);
  char *location_nf = (char *)_mem_alloc(mp, loc_len_nf + 1);
  if (!location_nf) return NULL;
  memcpy(location_nf, location, loc_len_nf);
  location_nf[loc_len_nf] = '\0';
  location = location_nf;

  /* An absolute-path or relative-path reference against a unix-socket base
   * reconstructs "http+unix://<percent-encoded-path>" + the resolved path,
   * instead of "scheme://host:port" + path; there is no host/port to
   * reconstruct from for a unix target. */
  char *authority = base->is_unix
                        ? _percent_encode_unix_path(mp, base->unix_socket_path)
                        : _format_bracketed_host(mp, base->host, base->is_ipv6);
  if (!authority) {
    _mem_free(mp, location_nf);
    return NULL;
  }

  bool default_port =
      !base->is_unix && ((base->is_https && base->port == 443) ||
                         (!base->is_https && base->port == 80));

  if (loc_len_nf == 0) {
    /* The reference was nothing but a fragment (e.g. "#s"): per RFC 3986
     * SS5.3, both R.path and R.query are undefined in that case, so
     * T.path = Base.path and T.query = Base.query; the whole
     * base->path_and_query is reused completely verbatim, not merely its
     * directory (which is what the general ref_path_len == 0 handling
     * further below, designed for a genuine query-only reference like
     * "?y" where R.query IS defined, would do: reuse the base's PATH but
     * drop its query, since a real "?y" reference supplies its own query
     * to take the base's place). Base is already fully resolved/
     * normalised, so no merge or dot-segment removal is needed here at
     * all. */
    _mem_free(mp, location_nf);
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
      _mem_free(mp, authority);
      return NULL;
    }
    char *out0 = (char *)_mem_alloc(mp, (size_t)needed0 + 1);
    if (!out0) {
      _mem_free(mp, authority);
      return NULL;
    }
    if (base->is_unix || default_port) {
      snprintf(out0, (size_t)needed0 + 1, "%s://%s%s", out_scheme0, authority,
               base->path_and_query);
    } else {
      snprintf(out0, (size_t)needed0 + 1, "%s://%s:%u%s", out_scheme0,
               authority, (unsigned)base->port, base->path_and_query);
    }
    _mem_free(mp, authority);
    return out0;
  }

  /* Split R.query off of R.path ONCE, up front, shared by both branches
   * below: remove_dot_segments (RFC 3986 SS5.2.4) must operate on the path
   * component only, never on query bytes, since a literal ".."/"." inside
   * a query value must never be reinterpreted as path navigation (e.g. a
   * query containing "/../" would otherwise cause dot-segment removal to
   * walk backwards through, and delete, path segments it was never meant to
   * touch). ref_query (if any) is re-appended untouched after dot-segment
   * removal has run, for both branches. */
  const char *ref_query = strchr(location, '?');
  size_t ref_path_len =
      ref_query ? (size_t)(ref_query - location) : strlen(location);

  char *new_path = NULL;
  if (location[0] == '/') {
    /* Absolute-path reference: T.path = remove_dot_segments(R.path)
     * directly, no merge against the base path needed. */
    char *path_only = (char *)_mem_alloc(mp, ref_path_len + 1);
    if (path_only) {
      memcpy(path_only, location, ref_path_len);
      path_only[ref_path_len] = '\0';
      new_path = _remove_dot_segments(mp, path_only);
      _mem_free(mp, path_only);
    }
  } else if (ref_path_len == 0) {
    /* RFC 3986 SS5.3: R.path == "" (a query-only reference, e.g. "?y") means
     * T.path = Base.path VERBATIM (no merge, no dot-segment removal).
     * Base is not assumed to already be normalised here: this client never
     * dot-segment-normalises the caller's own original request URL either
     * (see _parse_chttp_url), so re-deriving a "cleaned" form of it at this
     * one specific reference shape would silently redirect to a different
     * path than the one the original request actually used, with no server
     * instruction to change it (a real, previously-untested divergence from
     * both the RFC and a reference implementation, e.g. Python's
     * urllib.parse.urljoin: both leave a dotted base path like "/a/../b"
     * untouched for a query-only reference, matching the fragment-only
     * branch above, which already gets this right for the identical
     * reason). _merge_ref_path's own ref_path_len==0 handling already
     * produces exactly this verbatim copy (dir_len == the whole base path,
     * nothing appended); the bug was applying _remove_dot_segments to that
     * result afterward, in the branch below, which is why this needs to be
     * its own branch rather than merely skipping that one call inline. */
    new_path = _merge_ref_path(mp, base->path_and_query, location, 0);
  } else {
    char *merged =
        _merge_ref_path(mp, base->path_and_query, location, ref_path_len);
    if (merged) {
      new_path = _remove_dot_segments(mp, merged);
      _mem_free(mp, merged);
    }
  }
  if (new_path && ref_query) {
    /* _concat_len's own overflow guard (see its doc comment) is what makes
     * this safe against path_len + query_len wrapping SIZE_MAX; treated as
     * an ordinary allocation failure here (the existing !with_query
     * branch), exactly like the real OOM case just below it. */
    char *with_query = _concat_len(mp, new_path, strlen(new_path), ref_query,
                                   strlen(ref_query));
    if (!with_query) {
      _mem_free(mp, new_path);
      new_path = NULL;
    } else {
      _mem_free(mp, new_path);
      new_path = with_query;
    }
  }
  if (!new_path) {
    _mem_free(mp, authority);
    _mem_free(mp, location_nf);
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
    _mem_free(mp, authority);
    _mem_free(mp, new_path);
    _mem_free(mp, location_nf);
    return NULL;
  }
  char *out = (char *)_mem_alloc(mp, (size_t)needed + 1);
  if (!out) {
    _mem_free(mp, authority);
    _mem_free(mp, new_path);
    _mem_free(mp, location_nf);
    return NULL;
  }
  if (base->is_unix || default_port) {
    snprintf(out, (size_t)needed + 1, "%s://%s%s", out_scheme, authority,
             new_path);
  } else {
    snprintf(out, (size_t)needed + 1, "%s://%s:%u%s", out_scheme, authority,
             (unsigned)base->port, new_path);
  }
  _mem_free(mp, authority);
  _mem_free(mp, new_path);
  _mem_free(mp, location_nf);
  return out;
}

/* White-box test helpers exposing _parse_chttp_url/_resolve_redirect_url;
 * chttp_url_t is a file-local type, so these flatten the result into out
 * params/a plain string. NULL mp means every result is plain-malloc'd (see
 * _mem_alloc); test code frees them with plain free(). Not part of the
 * public API; gated so these symbols do not leak into a production build
 * of libccollections.so, matching every other white-box helper in this
 * file. */
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
    _mem_free(mp, parsed.unix_socket_path);
  if (host_out)
    *host_out = parsed.host;
  else
    _mem_free(mp, parsed.host);
  if (path_and_query_out)
    *path_and_query_out = parsed.path_and_query;
  else
    _mem_free(mp, parsed.path_and_query);
  if (origin_key_out)
    *origin_key_out = parsed.origin_key;
  else
    _mem_free(mp, parsed.origin_key);
  if (userinfo_authorization_out)
    *userinfo_authorization_out = parsed.userinfo_authorization;
  else
    _mem_free(mp, parsed.userinfo_authorization);
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
  /* A NULL body->data paired with a nonzero body->len is an inconsistent
   * body descriptor (nothing to actually copy body->len bytes from); the
   * body-copy block below only runs when body->data is non-NULL, so this
   * combination used to be silently treated as "no body" instead of being
   * reported to the caller, unlike chttp_base64_encode_mp's identical
   * NULL-data/nonzero-len combination, which is explicitly rejected. A
   * caller with a real bug (a miscomputed length paired with a null
   * buffer) deserves a diagnosable ccol_invalid_args, not a request that
   * silently goes out with no body at all. */
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
    mem_cpy(mp, mprocs, sizeof(ccol_memmgmt_procs_t));
  }

  chttp_request_t *req =
      (chttp_request_t *)_mem_alloc(mp, sizeof(chttp_request_t));
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
    _mem_free(mp, req);
    if (mp) mp->free(mp);
    return NULL;
  }

  if (body && body->data && body->len > 0) {
    void *body_copy = _mem_alloc(mp, body->len);
    if (!body_copy) {
      if (err_str) *err_str = CCOL_ERR_STR("failed to copy body");
      _mem_free(mp, req->url);
      _mem_free(mp, req);
      if (mp) mp->free(mp);
      return NULL;
    }
    memcpy(body_copy, body->data, body->len);
    req->body.data = body_copy;
    req->body.len = body->len;
  }

  /* content_type is copied whenever the caller supplied one, independent of
   * whether body->len > 0: a genuinely zero-length body (e.g. an empty JSON
   * payload via CHTTP_JSON_BODY("", 0)) with an explicit content type is a
   * legitimate request shape, and _serialize_request's Content-Type
   * auto-injection is documented to honor req->body.content_type regardless
   * of body length. Nesting this copy inside the body->len > 0 branch above
   * used to silently discard content_type for exactly that shape. */
  if (body && body->content_type) {
    req->body.content_type = ccol_strdup(mp, body->content_type);
    if (!req->body.content_type) {
      if (err_str) *err_str = CCOL_ERR_STR("failed to copy content_type");
      _mem_free(mp, (void *)req->body.data);
      _mem_free(mp, req->url);
      _mem_free(mp, req);
      if (mp) mp->free(mp);
      return NULL;
    }
  }

  return req;
}

ccol_retval_t chttp_request_set_header(chttp_request_t *req, const char *name,
                                       const char *value) {
  if (!req || !name || !value) return ccol_invalid_args;
  /* A field-name must be a non-empty tchar-only token (RFC 7230 SS3.2.6),
   * mirroring chttpsvr_resp_set_header's identical validation on the
   * server side (chttpserver.c) and this parser's own request-line
   * method/header-name parsing (chttp1_parser.c) for bytes arriving off
   * the wire: an empty name, or one containing a byte outside that set
   * (e.g. a space or a literal ':'), produces a structurally malformed
   * "name: value\r\n" wire line even when it contains no CR/LF of its own. */
  if (!*name) return ccol_invalid_args;
  for (const char *p = name; *p; p++) {
    if (!chttp1_is_tchar((unsigned char)*p)) return ccol_invalid_args;
  }
  /* _serialize_request writes name/value verbatim onto the wire as
   * "name: value\r\n", with no further escaping; an embedded CR or LF byte
   * would let a caller that reflects any untrusted data (a forwarded
   * bearer token, a proxied header) into a request header inject arbitrary
   * extra header lines, or split the request into two, on behalf of
   * whoever controls that data (classic HTTP request splitting / CRLF
   * injection). Rejected outright here, at the one function every
   * documented header-setting path funnels through; _serialize_request
   * itself carries an identical, redundant check as a backstop for the
   * header map any caller with the internal chmap handle is technically
   * free to build and assign directly instead (chttp_run_query, tests). */
  if (strpbrk(name, "\r\n") || strpbrk(value, "\r\n")) return ccol_invalid_args;
  /* chttpclient.c never implements chunked (or any other) request-body
   * transfer-coding: a body-carrying request is always sent Content-Length-
   * framed, with the exact bytes of req->body.data appended verbatim. A
   * caller-set "Transfer-Encoding" header therefore cannot ever be honored;
   * silently accepting it would let _serialize_request's own Content-Length
   * synthesis (gated only on "no explicit content-length header", never on
   * "no explicit transfer-encoding header") add a Content-Length header
   * alongside it, producing a request declaring BOTH framings at once over a
   * body that is not actually chunk-encoded; exactly the RFC 7230 SS3.3.3
   * ambiguous-framing shape this codebase's own chttp1_parser.c rejects
   * outright when parsing an incoming message (see its F_CONTENT_LENGTH/
   * F_CHUNKED conflict check). Rejected here rather than silently dropped or
   * sent as-is, for the same "fail loud, not producing a malformed wire
   * message" reasoning the CRLF check above already uses. */
  if (strcasecmp(name, "transfer-encoding") == 0) return ccol_invalid_args;

  if (!req->headers) {
    char *err = NULL;
    chmap hm = chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                                 ccol_string, req->_m_procs, NULL, &err);
    if (!hm) return ccol_not_enough_memory;
    req->headers = hm;
  }

  size_t nlen = strlen(name);
  char *lower = (char *)_mem_alloc(req->_m_procs, nlen + 1);
  if (!lower) return ccol_not_enough_memory;
  for (size_t i = 0; i <= nlen; i++)
    lower[i] = (char)tolower((unsigned char)name[i]);

  cmap_pair kp = {.ptr = lower, .size = nlen + 1};
  cmap_pair vp = {.ptr = (void *)value, .size = strlen(value) + 1};

  ccol_retval_t rv = chmap_insert_elem((chmap)req->headers, &kp, &vp);
  _mem_free(req->_m_procs, lower);
  if (rv == ccol_key_already_present) rv = ccol_success;
  return rv;
}

const char *chttp_request_get_header(const chttp_request_t *req,
                                     const char *name) {
  if (!req || !name || !req->headers) return NULL;

  size_t nlen = strlen(name);
  char *lower = (char *)_mem_alloc(req->_m_procs, nlen + 1);
  if (!lower) return NULL;
  for (size_t i = 0; i <= nlen; i++)
    lower[i] = (char)tolower((unsigned char)name[i]);

  cmap_pair kp = {.ptr = lower, .size = nlen + 1};
  cmap_pair *vp = NULL;
  ccol_retval_t rv = chmap_get_elem_ref((chmap)req->headers, &kp, &vp);
  _mem_free(req->_m_procs, lower);

  if (rv != ccol_success || !vp) return NULL;
  return (const char *)vp->ptr;
}

void chttp_request_free(chttp_request_t *req) {
  if (!req) return;
  ccol_memmgmt_procs_t *mp = req->_m_procs;
  _mem_free(mp, req->url);
  _mem_free(mp, (void *)req->body.data);
  _mem_free(mp, (void *)req->body.content_type);
  if (req->headers) __chmap_destroy((chmap)req->headers);
  _mem_free(mp, req);
  if (mp) mp->free(mp);
}

/* ========================================================================== */
/*                         DEADLINE HELPERS                                   */
/* ========================================================================== */

static chttp_deadline_t _deadline_make(long timeout_ms) {
  chttp_deadline_t d = {0};
  if (timeout_ms <= 0) return d; /* inactive: no limit */
  d.active = true;
  clock_gettime(CLOCK_MONOTONIC, &d.deadline);
  d.deadline.tv_sec += timeout_ms / 1000;
  d.deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
  if (d.deadline.tv_nsec >= 1000000000L) {
    d.deadline.tv_nsec -= 1000000000L;
    d.deadline.tv_sec += 1;
  }
  return d;
}

/*
 * Returns false if the deadline has already elapsed (caller should treat this
 * as an immediate ccol_timed_out). Otherwise sets *out_ms to the remaining
 * milliseconds, or -1 if there is no active deadline (block indefinitely).
 */
static bool _deadline_remaining_ms(const chttp_deadline_t *d, int *out_ms) {
  if (!d->active) {
    *out_ms = -1;
    return true;
  }
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  long ms = (long)(d->deadline.tv_sec - now.tv_sec) * 1000L +
            (d->deadline.tv_nsec - now.tv_nsec) / 1000000L;
  if (ms <= 0) return false;
  *out_ms = (ms > INT_MAX) ? INT_MAX : (int)ms;
  return true;
}

static int _combine_ms(int a, int b) {
  if (a < 0) return b;
  if (b < 0) return a;
  return (a < b) ? a : b;
}

/* Returns whichever of a/b elapses first ("inactive" == no limit, so it never
 * wins over an active one). Used to bound the Expect: 100-continue interim
 * wait by both its own CHTTP_100_CONTINUE_WAIT_MS budget and whatever is
 * left of the request's overall deadline, without needing a second parallel
 * remaining-ms computation the way _tcp_connect's connect/overall pair does
 * (that shape doesn't fit here since the wait lives inside a single
 * _chttp_read_message call, which only accepts one chttp_deadline_t). */
static chttp_deadline_t _deadline_earlier(chttp_deadline_t a,
                                          chttp_deadline_t b) {
  if (!a.active) return b;
  if (!b.active) return a;
  if (a.deadline.tv_sec != b.deadline.tv_sec)
    return (a.deadline.tv_sec < b.deadline.tv_sec) ? a : b;
  return (a.deadline.tv_nsec <= b.deadline.tv_nsec) ? a : b;
}

/* Initialises *cv against CLOCK_MONOTONIC, matching every deadline this file
 * computes via _deadline_make/clock_gettime(CLOCK_MONOTONIC, ...); falls back
 * to the platform default clock if condattr support is unavailable. Without
 * this, a cond_var_timedwait call against a timespec produced by
 * _deadline_make would be comparing a monotonic-clock-based value against a
 * condvar internally using the wall clock (CLOCK_REALTIME by default),
 * making the wait either return ETIMEDOUT immediately or never honour the
 * deadline at all; see client_deadline_bundle's own identical fix
 * (_client_deadline_init_globals) for the first place this exact mistake was
 * caught in this codebase. */
static int _cond_var_init_monotonic(cond_var_t *cv) {
  cond_var_attr_t cv_attr;
  if (cond_var_attr_init(cv_attr) == 0) {
    cond_var_attr_setclock(cv_attr, CLOCK_MONOTONIC);
    int rv = cond_var_init_ca(*cv, cv_attr);
    cond_var_attr_destroy(cv_attr);
    return rv;
  }
  return cond_var_init(*cv);
}

/* ========================================================================== */
/*                         LOW-LEVEL SOCKET / TLS I/O                         */
/* ========================================================================== */

/*
 * poll() can legitimately return -1/EINTR if a signal is delivered before any
 * fd becomes ready (more frequent under tools like valgrind that use signals
 * internally, but a real possibility in any process). Retry internally,
 * tracking elapsed time against the original timeout budget so repeated
 * interruptions cannot extend the caller's requested wait indefinitely.
 */
static ccol_retval_t _conn_wait(int fd, short events, int timeout_ms) {
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
          long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L +
                            (now.tv_nsec - start.tv_nsec) / 1000000L;
          remaining = (int)(timeout_ms - elapsed_ms);
          if (remaining <= 0) return ccol_timed_out;
        }
        continue;
      }
      return ccol_http_transfer_aborted;
    }
    if (rc == 0) return ccol_timed_out;
    if (pfd.revents & (POLLERR | POLLNVAL)) return ccol_http_connection_failed;
    return ccol_success;
  }
}

static ssize_t _conn_read(chttp_conn_t *c, void *buf, size_t len) {
  if (c->tls) {
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
    ssize_t n = ctls_conn_write(c->tls, buf, len);
    if (n < 0 && (errno == EAGAIN)) errno = EWOULDBLOCK;
    return n;
  }
  ssize_t n;
  do {
    n = send(c->fd, buf, len, MSG_NOSIGNAL);
  } while (n < 0 && errno == EINTR);
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) errno = EWOULDBLOCK;
  return n;
}

static ccol_retval_t _chttp_send_all(chttp_conn_t *conn, const char *data,
                                     size_t len, chttp_deadline_t *overall) {
  size_t sent = 0;
  while (sent < len) {
    int wait_ms;
    if (!_deadline_remaining_ms(overall, &wait_ms)) return ccol_timed_out;
    ccol_retval_t prv = _conn_wait(conn->fd, POLLOUT, wait_ms);
    if (prv != ccol_success) return prv;
    ssize_t n = _conn_write(conn, data + sent, len - sent);
    if (n < 0) {
      if (errno == EWOULDBLOCK) continue;
      return ccol_http_transfer_aborted;
    }
    if (n == 0) return ccol_http_transfer_aborted; /* peer closed mid-write */
    sent += (size_t)n;
  }
  return ccol_success;
}

/* Applies TCP_NODELAY to fd, best-effort (a failure here is never fatal to
 * the connection itself, just a missed latency optimisation). Closes a
 * pre-existing asymmetry: this Tier 1 connect path used to set no socket
 * options at all, while Tier 2's connect path already got TCP_NODELAY
 * internally; both tiers now apply it consistently via this one shared
 * helper. Meaningless for a unix domain socket (no TCP layer), so callers
 * only invoke this for an AF_INET/AF_INET6 connection. */
static void _apply_tcp_nodelay(int fd) {
  int one = 1;
  (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

/* Resolves host:port and connects, trying each address in turn. Combines the
 * connect-specific and overall request deadlines (whichever is tighter). */
static ccol_retval_t _tcp_connect(const char *host, uint16_t port,
                                  chttp_deadline_t *connect_dl,
                                  chttp_deadline_t *overall, int *fd_out) {
  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo *res = NULL;
  if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
    return ccol_http_host_resolution_failed;

  ccol_retval_t result = ccol_http_connection_failed;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    int fd =
        socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK, ai->ai_protocol);
    if (fd < 0) continue;

    int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc == 0) {
      _apply_tcp_nodelay(fd);
      *fd_out = fd;
      result = ccol_success;
      break;
    }
    if (errno != EINPROGRESS) {
      close(fd);
      continue;
    }

    int wait_ms, overall_ms;
    if (!_deadline_remaining_ms(connect_dl, &wait_ms)) {
      close(fd);
      result = ccol_timed_out;
      break;
    }
    if (!_deadline_remaining_ms(overall, &overall_ms)) {
      close(fd);
      result = ccol_timed_out;
      break;
    }

    ccol_retval_t prv =
        _conn_wait(fd, POLLOUT, _combine_ms(wait_ms, overall_ms));
    if (prv == ccol_timed_out) {
      close(fd);
      result = ccol_timed_out;
      break;
    }
    if (prv != ccol_success) {
      close(fd);
      result = ccol_http_connection_failed;
      continue;
    }

    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0 ||
        soerr != 0) {
      close(fd);
      result = ccol_http_connection_failed;
      continue;
    }

    _apply_tcp_nodelay(fd);
    *fd_out = fd;
    result = ccol_success;
    break;
  }

  freeaddrinfo(res);
  return result;
}

/* Connects to a unix domain stream socket at path, honouring the same
 * connect/overall deadlines _tcp_connect does. No TCP_NODELAY/DNS involved;
 * a unix domain connect() essentially never returns EINPROGRESS on Linux
 * (the accept queue is serviced synchronously in-kernel), but the
 * non-blocking + poll() dance is kept anyway for portability and to honour
 * the deadline even in that rare case. */
static ccol_retval_t _unix_connect(const char *path,
                                   chttp_deadline_t *connect_dl,
                                   chttp_deadline_t *overall, int *fd_out) {
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  size_t path_len = strlen(path);
  if (path_len >= sizeof(addr.sun_path)) return ccol_http_invalid_url;
  memcpy(addr.sun_path, path, path_len + 1);

  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
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
  ccol_retval_t prv = _conn_wait(fd, POLLOUT, _combine_ms(wait_ms, overall_ms));
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

/* Drives the client-mode TLS handshake to completion, honouring both the
 * connect and overall deadlines (TLS handshake time is folded into the
 * connect timeout, matching curl's own behaviour). */
static ccol_retval_t _tls_handshake(chttp_conn_t *conn,
                                    chttp_deadline_t *connect_dl,
                                    chttp_deadline_t *overall) {
  for (;;) {
    ctls_handshake_result_t r = ctls_conn_handshake_step(conn->tls);
    if (r == CTLS_HANDSHAKE_DONE) return ccol_success;
    if (r == CTLS_HANDSHAKE_ERROR) {
      /* 0 == X509_V_OK by OpenSSL convention; ctls.h intentionally does not
       * expose OpenSSL headers to callers, so the raw value is compared
       * directly rather than via the X509_V_OK symbol. */
      long vr = ctls_conn_verify_result(conn->tls);
      return (vr != 0) ? ccol_http_tls_cert_verification_failed
                       : ccol_http_tls_handshake_failed;
    }

    short ev = (r == CTLS_HANDSHAKE_WANT_WRITE) ? POLLOUT : POLLIN;
    int wait_ms, overall_ms;
    if (!_deadline_remaining_ms(connect_dl, &wait_ms)) return ccol_timed_out;
    if (!_deadline_remaining_ms(overall, &overall_ms)) return ccol_timed_out;
    ccol_retval_t prv =
        _conn_wait(conn->fd, ev, _combine_ms(wait_ms, overall_ms));
    if (prv != ccol_success) return prv;
  }
}

static ccol_retval_t _conn_open(ccol_memmgmt_procs_t *mp,
                                const chttp_url_t *url, bool want_tls,
                                ctls_ctx_t *tls_ctx, bool verify_host,
                                chttp_deadline_t *connect_dl,
                                chttp_deadline_t *overall, chttp_conn_t *out) {
  memset(out, 0, sizeof(*out));
  out->fd = -1;

  int fd = -1;
  ccol_retval_t rv =
      url->is_unix
          ? _unix_connect(url->unix_socket_path, connect_dl, overall, &fd)
          : _tcp_connect(url->host, url->port, connect_dl, overall, &fd);
  if (rv != ccol_success) return rv;
  out->fd = fd;

  if (want_tls) {
    out->tls =
        ctls_conn_create_client(tls_ctx, fd, url->host, verify_host, NULL);
    if (!out->tls) {
      close(fd);
      out->fd = -1;
      return ccol_not_enough_memory;
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
  _mem_free(mp, c->origin_key);
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
 * Acquires a concurrency-limiter slot, blocking while the pool is at
 * capacity. `deadline` (NULL, or an inactive chttp_deadline_t, both mean "no
 * limit") bounds how long this call is willing to block waiting for a slot;
 * if it elapses first, ccol_timed_out is returned instead of waiting
 * forever. This is what makes chttpclient_set_request_timeout's documented
 * contract ("the maximum time from when chttpclient_do is called...") hold
 * even when the pool (chttpclient_set_pool_size) is saturated: the caller
 * (chttp_do_internal) computes the overall deadline BEFORE calling this,
 * specifically so time spent waiting here counts against it, rather than
 * being invisible to it the way an unconditional cond_var_wait would leave
 * it. cli->available is initialised against CLOCK_MONOTONIC (see
 * _cond_var_init_monotonic) specifically so deadline->deadline, itself a
 * CLOCK_MONOTONIC timespec from _deadline_make, can be handed to
 * cond_var_timedwait directly.
 */
static ccol_retval_t _slot_acquire(struct chttpclient *cli,
                                   const chttp_deadline_t *deadline) {
  mutex_lock(cli->lock);
  if (cli->destroying) {
    mutex_unlock(cli->lock);
    return ccol_not_permitted;
  }
  if (!cli->pool_initialized) {
    cli->pool_cap = _resolve_pool_cap(cli->configured_pool_size);
    cli->pool_initialized = true;
  }
  while (cli->in_flight_count >= cli->pool_cap && !cli->destroying) {
    if (deadline && deadline->active) {
      int wrc =
          cond_var_timedwait(cli->available, cli->lock, deadline->deadline);
      if (wrc == ETIMEDOUT) {
        mutex_unlock(cli->lock);
        return ccol_timed_out;
      }
      /* Any other outcome (a genuine wake, or a spurious one) just falls
       * through to re-checking the loop predicate above, exactly like
       * cond_var_wait's own spurious-wakeup handling already does. */
    } else {
      cond_var_wait(cli->available, cli->lock);
    }
  }
  if (cli->destroying) {
    mutex_unlock(cli->lock);
    return ccol_not_permitted;
  }
  cli->in_flight_count++;
  mutex_unlock(cli->lock);
  return ccol_success;
}

static void _slot_release(struct chttpclient *cli) {
  mutex_lock(cli->lock);
  cli->in_flight_count--;
  cond_var_broadcast(cli->available);
  mutex_unlock(cli->lock);
}

/* ========================================================================== */
/*                         KEEP-ALIVE IDLE POOL                               */
/* ========================================================================== */

/* chmap_entry's SSO storage is naturally aligned, so this memcpy is
 * defense-in-depth rather than a live alignment requirement; kept for
 * consistency with cjson/cyaml/clrucache/cthreadcomm's own chmap-backed
 * pointer storage, which use the same pattern. */
static inline cvec _read_cvec(const void *src) {
  cvec v;
  memcpy(&v, src, sizeof(v));
  return v;
}

/*
 * Attempts to pop a usable idle connection for `origin_key` into *out.
 * Returns false if none is available. May pop and discard several stale/dead
 * candidates before finding a live one or exhausting the list; the age check
 * and liveness probe both happen OUTSIDE the client lock (no I/O while
 * holding it).
 */
static bool _idle_pool_take(struct chttpclient *cli, const char *origin_key,
                            chttp_conn_t *out) {
  for (;;) {
    bool got = false;
    mutex_lock(cli->lock);
    if (cli->idle_pools) {
      cmap_pair kp = {.ptr = (void *)origin_key,
                      .size = strlen(origin_key) + 1};
      cmap_pair *vp = NULL;
      if (chmap_get_elem_ref(cli->idle_pools, &kp, &vp) == ccol_success && vp) {
        cvec list = _read_cvec(vp->ptr);
        if (list && cvector_elem_count(list) > 0 &&
            cvector_pop_back(list, out) == ccol_success) {
          got = true;
          if (cli->idle_total_count > 0) cli->idle_total_count--;
          /* Prune the now-empty per-origin list/map entry rather than
           * leaving it behind indefinitely: a long-running client that
           * contacts many distinct origins over its lifetime (a crawler, an
           * outbound-heavy service) would otherwise accumulate one
           * permanent chmap entry (plus an empty cvec) per origin it has
           * EVER pooled a connection for, unbounded by CHTTP_MAX_IDLE_
           * TOTAL/CHTTP_MAX_IDLE_PER_ORIGIN (which only bound live
           * connections, never distinct origin keys). _idle_pool_offer
           * already handles "no entry for this origin yet" by creating one
           * fresh (see its own logic a few lines below), so removing an
           * empty entry here is always safe: the very next offer for this
           * origin recreates it exactly as if it were a brand-new origin. */
          if (cvector_elem_count(list) == 0) {
            __cvector_destroy(list);
            chmap_delete_elem(cli->idle_pools, &kp);
          }
        }
      }
    }
    mutex_unlock(cli->lock);
    if (!got) return false;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long age_ms = (now.tv_sec - out->last_used.tv_sec) * 1000L +
                  (now.tv_nsec - out->last_used.tv_nsec) / 1000000L;

    bool alive = false;
    if (age_ms <= CHTTP_IDLE_MAX_AGE_MS) {
      char probe;
      ssize_t pn;
      if (out->tls) {
        /* A raw MSG_PEEK on out->fd would peek still-encrypted wire bytes,
         * bypassing OpenSSL entirely: a perfectly healthy TLS connection on
         * which the peer has proactively sent anything at the TLS record
         * layer since the last response was read (most commonly a TLS 1.3
         * NewSessionTicket, which OpenSSL servers routinely send right after
         * the handshake/response) would show up here as "data available",
         * making this probe wrongly declare the connection dead and discard
         * it, silently defeating HTTPS keep-alive reuse. Going through
         * ctls_conn_read instead lets OpenSSL absorb/process any such
         * protocol-only record transparently, exactly like the async
         * engine's own idle-connection liveness check
         * (_async_on_readable_impl's CHTTP_ASYNC_DISPATCH_IDLE branch)
         * already does. This is a real (non-peeking) read rather than a
         * peek, but that is harmless here: if it returns > 0, genuine
         * application data was pending and this connection is about to be
         * discarded anyway (never reused), so consuming that byte has no
         * observable effect; if it returns EWOULDBLOCK, nothing was
         * consumed and the connection is left fully intact for reuse. */
        pn = ctls_conn_read(out->tls, &probe, 1);
      } else {
        /* Retried on EINTR, matching every other blocking-vs-signal-safe
         * syscall wrapper in this file (_conn_read/_conn_write/_conn_wait):
         * a signal interrupting this MSG_DONTWAIT peek reports errno ==
         * EINTR, which matches neither EAGAIN nor EWOULDBLOCK below, so an
         * un-retried call would wrongly declare a perfectly healthy
         * connection dead and discard it instead of reusing it. */
        do {
          pn = recv(out->fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
        } while (pn < 0 && errno == EINTR);
      }
      alive = (pn < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    }
    if (alive) return true;

    _conn_teardown(cli->m_procs, out);
    /* loop: try the next candidate (if any) for this origin */
  }
}

/*
 * Offers a still-good connection back to the idle pool, bounded by per-origin
 * and total caps. If it doesn't fit (caps hit, or the client is being
 * destroyed), the connection is simply closed instead; a lost optimisation
 * opportunity, never a correctness issue.
 */
static void _idle_pool_offer(struct chttpclient *cli, chttp_conn_t *c) {
  bool pooled = false;
  mutex_lock(cli->lock);
  if (!cli->destroying && cli->idle_pools &&
      cli->idle_total_count < CHTTP_MAX_IDLE_TOTAL) {
    cmap_pair kp = {.ptr = c->origin_key, .size = strlen(c->origin_key) + 1};
    cmap_pair *vp = NULL;
    cvec list = NULL;
    if (chmap_get_elem_ref(cli->idle_pools, &kp, &vp) == ccol_success && vp) {
      list = _read_cvec(vp->ptr);
    }
    /* A genuinely new origin only gets a fresh entry while the distinct-
     * origin cap still has room; see CHTTP_MAX_IDLE_ORIGINS's own comment.
     * At capacity, list stays NULL, falling through to the same "doesn't
     * fit" path every other pooling failure below already uses. */
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
  mutex_unlock(cli->lock);
  if (!pooled) _conn_teardown(cli->m_procs, c);
}

/* ========================================================================== */
/*                         REQUEST SERIALIZATION                              */
/* ========================================================================== */

/*
 * Presence of each header _serialize_request auto-injects a default for,
 * scanned case-insensitively in one pass over req->headers (see
 * _scan_header_presence below). A plain chmap_get_elem_ref point lookup
 * (keyed on a lower-cased query string, matching chttp_request_get_header)
 * would miss a borrowed map (e.g. one built directly by a caller and handed
 * to chttp_run_query, never routed through chttp_request_set_header's own
 * lower-casing) whose keys use natural casing like "Content-Type" or
 * "Authorization"; a mismatch there previously meant chttp_run_query calls
 * could hand back a wire request carrying both the caller's own header and
 * an auto-injected duplicate (or, for Host specifically, silently drop the
 * caller's own value, since the header-emission loop below always skips
 * any case-insensitive "host" match unconditionally). This was only ever
 * fixed for Content-Type (the case-insensitive scan this struct/function
 * generalizes); every other field below needs the identical treatment.
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
 * Case-insensitive header-NAME dedup tracker for _serialize_request's header-
 * emission loop below. req->headers is keyed by exact byte content, so a
 * caller-constructed (borrowed) map can legally hold two literally-different
 * keys that are the SAME header name under HTTP's case-insensitive semantics
 * (e.g. "Host" and "host") as two distinct entries; chttp_request_set_header's
 * own lower-casing prevents this for the documented header-setting API, but a
 * borrowed map built directly (chttp_run_query's headers parameter, or a
 * caller that pokes chttp_request_t.headers itself) has no such guarantee.
 * Left undeduplicated, both entries reached the wire as two separate header
 * lines; for an arbitrary custom header that is merely unusual (RFC 7230
 * SS3.2.2 permits combining repeated fields with the same name), but for
 * "Host" specifically it produces a request RFC 7230 SS5.4 requires a server
 * to reject outright ("more than one Host header field" is explicitly listed
 * as a 400 Bad Request condition); a real, remotely-observable protocol
 * violation, not merely a style nit, and the same duplicate-Host shape real
 * request-smuggling/cache-poisoning techniques rely on.
 *
 * `names` holds borrowed pointers into req->headers' own key storage (valid
 * for the emission loop's duration; chmap iteration never mutates or
 * relocates existing entries), populated in iteration order (which, for
 * this codebase's separate-chaining chmap, is newest-insertion-first; see
 * chashmap.c's own "Reverse-insertion-order iteration" documentation), so
 * checking "already seen" before adding and skipping on a hit keeps exactly
 * the most-recently-inserted occurrence of each case-insensitive name and
 * drops any older duplicate, matching the "last set wins" behavior a single
 * exact-key chmap_insert_elem update already exhibits.
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

/* Records name as emitted. Returns false only on allocation failure (the
 * growable backing array itself, never a copy of name's bytes, since name
 * is borrowed and outlives this whole call). */
static bool _seen_names_add(ccol_memmgmt_procs_t *mp, chttp_seen_names_t *seen,
                            const char *name) {
  if (seen->count == seen->cap) {
    /* Overflow guard on the doubling-growth computation below, mirroring
     * this project's own established guard idiom (cvector/csort/cmempool,
     * and this file's own _ob_append/_sink_buffered) for the identical
     * "curr_size *= 2" class of computation: without it, an already-
     * astronomical seen->cap doubling past SIZE_MAX, or the subsequent
     * *sizeof(*nn) multiplication overflowing on its own, could settle on a
     * small or zero nc; the header-emission loop above would then treat
     * that as a validly-grown (but actually undersized) array and write
     * seen->names[seen->count++] past its real bounds. Unreachable in
     * practice (would need on the order of 2^61 distinct header names on a
     * single request), but this project treats a reducible/unguarded
     * overflow in a size computation as a real bug regardless of how large
     * an input is needed to trigger it. */
    if (seen->cap > SIZE_MAX / 2) return false;
    size_t nc = seen->cap ? seen->cap * 2 : 8;
    if (nc > SIZE_MAX / sizeof(*seen->names)) return false;
    const char **nn =
        (const char **)_mem_realloc(mp, (void *)seen->names, nc * sizeof(*nn));
    if (!nn) return false;
    seen->names = nn;
    seen->cap = nc;
  }
  seen->names[seen->count++] = name;
  return true;
}

#ifdef RUNNING_UNIT_TESTS
/*
 * White-box test helper exposing _seen_names_add's overflow guard directly:
 * constructs a chttp_seen_names_t whose own count/cap already sit at
 * fake_cap (names stays NULL; no real backing array of that size is ever
 * allocated or touched, since the guard must reject before the growth
 * branch's own realloc call, let alone the seen->names[seen->count++]
 * write below it, is ever reached) and calls _seen_names_add once. Mirrors
 * this project's own established "assert the guard rejects before any real
 * work happens" pattern for this exact class of overflow guard elsewhere
 * in this file (_chttp_ob_append_overflow_guard_for_tests,
 * _chttp_percent_encode_unix_path_overflow_guard_for_tests). Not part of
 * the public API; gated so this symbol does not leak into a production
 * build of libccollections.so, matching every other white-box helper in
 * this file.
 */
bool _chttp_seen_names_add_overflow_guard_for_tests(ccol_memmgmt_procs_t *mp,
                                                    size_t fake_cap) {
  chttp_seen_names_t seen = {.names = NULL, .count = fake_cap, .cap = fake_cap};
  bool added = _seen_names_add(mp, &seen, "x");
  /* A non-rejecting (ordinary, guard-not-triggered) call genuinely grows
   * seen.names via realloc; free it here rather than leaking it, since this
   * helper's own struct is a throwaway local, not something a caller of the
   * real _seen_names_add (which always frees seen.names itself, in
   * _serialize_request's own header-emission loop) would ever be
   * responsible for cleaning up. */
  _mem_free(mp, (void *)seen.names);
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
  /* Overflow guard on the "needed size" computation below, mirroring the
   * SIZE_MAX-relative guard idiom this project's other growable buffers
   * (cvector, csort, cmempool) already use for the identical class of
   * computation. Without this, a caller-supplied body large enough to make
   * b->len + n + 1 wrap past SIZE_MAX would let the doubling loop below
   * settle on an `nc` far smaller than actually needed (or, if `nc` itself
   * wraps to 0 mid-loop, spin forever, since 0 < any nonzero target is
   * always true), turning the subsequent memcpy into a real heap-buffer
   * overflow instead of a clean, reported allocation failure. Unreachable
   * in practice (it would require an already multi-exabyte request body
   * resident in memory before this function is ever called), but this
   * project treats a reducible/unguarded overflow in a size computation as
   * a real bug regardless of how large an input is needed to trigger it. */
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
    char *nb = (char *)_mem_realloc(b->mp, b->buf, nc);
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
 * White-box test helper exposing _ob_append's overflow guard directly: sets
 * up a chttp_outbuf_t whose synthetic `len` already sits at fake_len (no
 * real buffer of that size is ever allocated; chttp_outbuf_t starts with
 * buf == NULL/cap == 0 regardless, so the guard must reject the append
 * before ever touching mp's own malloc/realloc) and appends up to 64 bytes
 * (n is capped at compile time so a real, small backing buffer always
 * suffices) to it, returning whether _ob_append set b->oom. The caller is
 * expected to pass a counting allocator and check its own call count
 * separately; this mirrors this project's own established "assert the
 * allocator was never invoked" pattern for this exact class of overflow
 * guard elsewhere (see e.g. csort's/cmempool's own default comparators).
 * Not part of the public API; gated so this symbol does not leak into a
 * production build of libccollections.so, matching every other white-box
 * helper in this file.
 */
bool _chttp_ob_append_overflow_guard_for_tests(ccol_memmgmt_procs_t *mp,
                                               size_t fake_len, size_t n) {
  chttp_outbuf_t ob = {.mp = mp, .len = fake_len};
  char probe[64] = {0};
  if (n > sizeof(probe)) n = sizeof(probe);
  _ob_append(&ob, probe, n);
  bool oom = ob.oom;
  _mem_free(mp, ob.buf);
  return oom;
}
#endif /* RUNNING_UNIT_TESTS */

/*
 * Serialises method line + headers + body into a single wire buffer.
 * Redirect-hop method/body substitution is the caller's responsibility (via
 * the method/body fields of `req`, which is a shallow per-hop view, not the
 * caller's original request object).
 *
 * out_presence is optional (NULL is fine): when non-NULL, it receives the
 * same chttp_header_presence_t this function already computes internally
 * (from req->headers) to decide its own auto-injection/dedup behaviour.
 * Exists purely so a caller that also needs one of these flags (Tier 1's
 * hop loop needs has_expect, to decide whether this hop should route through
 * the Expect: 100-continue wait) doesn't have to pay for a second full
 * O(header count) scan via its own separate _scan_header_presence call;
 * chttp_do_internal used to do exactly that, once here and once again right
 * after this call returned, over the identical header map.
 */
static ccol_retval_t _serialize_request(ccol_memmgmt_procs_t *mp,
                                        const chttp_request_t *req,
                                        const chttp_url_t *url,
                                        const char *auto_authorization,
                                        bool suppress_explicit_authorization,
                                        char **out_buf, size_t *out_len,
                                        chttp_header_presence_t *out_presence) {
  chttp_outbuf_t ob = {.mp = mp};

  _ob_append_cstr(&ob, chttp_method_str(req->method));
  _ob_append(&ob, " ", 1);
  _ob_append_cstr(&ob, url->path_and_query);
  _ob_append_cstr(&ob, " HTTP/1.1\r\n");

  chttp_header_presence_t hp = _scan_header_presence((chmap)req->headers);
  if (hp.has_transfer_encoding) {
    /* Redundant with chttp_request_set_header's own identical rejection (see
     * that function's doc comment); this is the real backstop, since
     * req->headers is an internal chmap handle a caller can still build and
     * assign directly (chttp_run_query's borrowed map, or a caller that
     * pokes chttp_request_t.headers itself), entirely bypassing
     * chttp_request_set_header. Caught here, before the Content-Length
     * synthesis further down would otherwise pair it with a conflicting
     * Content-Length header over a body that was never actually
     * chunk-encoded. */
    _mem_free(mp, ob.buf);
    return ccol_invalid_args;
  }
  bool has_host = hp.has_host;
  bool has_accept = hp.has_accept;
  bool has_ua = hp.has_user_agent;
  bool has_cl = hp.has_content_length;
  bool has_ct = hp.has_content_type;
  /* A caller-set "authorization" header that this hop's caller has decided
   * to suppress (see suppress_explicit_authorization's own call-site
   * comments: a redirect that crossed to a different origin than the one
   * this header was originally set against) is treated as though it were
   * never present at all for the purpose of deciding whether to inject
   * auto_authorization below (the header loop further down independently
   * skips actually emitting it from req->headers). Without this, a caller
   * who set an explicit Authorization for the original origin AND whose
   * redirect target's OWN URL happens to embed "user:pass@" userinfo of its
   * own would wrongly end up with NO Authorization header on this hop at
   * all, instead of the new origin's own userinfo-derived one. */
  bool has_auth = hp.has_authorization && !suppress_explicit_authorization;

  if (!has_host) {
    if (url->is_unix) {
      /* A unix-domain-socket target has no real hostname/port to convey;
       * "localhost" matches curl's own --unix-socket default and is simple
       * and predictable for any server on the other end to expect (RFC 7230
       * SS5.4 still requires every HTTP/1.1 request to carry a Host header,
       * even though its value is meaningless here). */
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
    /* No "host" special-case here (unlike an earlier version of this loop):
     * the synthesis block above only ever runs when !has_host, so a "host"
     * entry only ever reaches this loop when the caller supplied one
     * explicitly (has_host true, synthesis skipped); skipping it
     * unconditionally here, on top of that, meant a caller-supplied Host
     * header (via chttp_request_set_header, or a borrowed map) was silently
     * dropped from the wire entirely: no synthesized line (correctly
     * suppressed) and no user-supplied line either (incorrectly
     * suppressed), violating RFC 7230 SS5.4's "every HTTP/1.1 request MUST
     * carry a Host header" requirement this same file's own unix-socket
     * comment already cites elsewhere. Letting it fall through here like any
     * other header is what actually honors has_host's own gating intent. */
    chttp_seen_names_t seen = {0};
    cmap_iterator *it = chashmap_begin_iter((chmap)req->headers, NULL);
    for (; it; it = it->_next_fn(it)) {
      const char *name = (const char *)it->key_pair->ptr;
      const char *val = (const char *)it->val_pair->ptr;
      /* Redundant with chttp_request_set_header's own identical checks (see
       * that function's doc comment); this is the real backstop, since
       * req->headers is an internal chmap handle any caller can still build
       * and assign directly (chttp_run_query's borrowed map, or a caller
       * that pokes chttp_request_t.headers itself), entirely bypassing
       * chttp_request_set_header. This loop is the one place every header,
       * from either path, is actually written onto the wire. */
      if (strpbrk(name, "\r\n") || strpbrk(val, "\r\n") || !*name) {
        ccol_iter_destroy(it);
        _mem_free(mp, (void *)seen.names);
        _mem_free(mp, ob.buf);
        return ccol_invalid_args;
      }
      for (const char *p = name; *p; p++) {
        if (!chttp1_is_tchar((unsigned char)*p)) {
          ccol_iter_destroy(it);
          _mem_free(mp, (void *)seen.names);
          _mem_free(mp, ob.buf);
          return ccol_invalid_args;
        }
      }
      /* A non-body-carrying method on THIS hop never puts req->body on the
       * wire at all (see the body-append check further down), regardless of
       * what req->headers still contains: either the caller set one of
       * these three on a request they always intended to be bodyless, or
       * (the more consequential case) a 301/302/303
       * redirect downgraded a POST/PUT/PATCH to GET: chttp_do_internal/
       * _async_submit_hop rewrite cur_method/cur_body for the new hop but
       * re-send req->headers/chain->req_headers completely unchanged, so a
       * Content-Length/Content-Type/Expect the caller set to describe the
       * ORIGINAL body survives onto a hop that will never actually send
       * one. A stale Content-Length is not merely cosmetically wrong: a
       * receiving server that trusts the declared length up front (this
       * codebase's own chttpserver.c included, via
       * chttp1_declared_content_length) blocks reading a body that will
       * never arrive, until its own read timeout fires. */
      if (!body_carrying_method && (strcasecmp(name, "content-length") == 0 ||
                                    strcasecmp(name, "content-type") == 0 ||
                                    strcasecmp(name, "expect") == 0))
        continue;
      /* See suppress_explicit_authorization's own call-site comments: a
       * caller-set Authorization header is dropped, not forwarded, once a
       * redirect chain has crossed to a different origin than the one it
       * was set against, matching curl's own CVE-2018-1000007-hardened
       * default for exactly this scenario. */
      if (suppress_explicit_authorization &&
          strcasecmp(name, "authorization") == 0)
        continue;
      /* See chttp_seen_names_t's own comment: a borrowed map may hold two
       * case-variant keys for the same header name; only the first one
       * encountered here (the most-recently-inserted, per chmap's own
       * iteration order) is ever written to the wire. */
      if (_seen_names_contains(&seen, name)) continue;
      /* A caller-supplied Content-Length must match the body bytes actually
       * about to be appended below (see the body-append check further
       * down), or the framing this hop declares to the server desyncs from
       * what is actually sent onto a connection this client will also pool
       * for reuse; the identical "declared framing disagrees with the
       * wire" hazard the Transfer-Encoding rejection above exists to
       * prevent (see chttp_request_set_header's own doc comment), just via
       * a wrong length instead of a wrong transfer-coding. Only checked for
       * a body_carrying_method: for any other method this exact header is
       * already dropped from the wire entirely a few lines above (line
       * ~2088), so there is no framing for a mismatched value to desync
       * from. */
      if (body_carrying_method && strcasecmp(name, "content-length") == 0) {
        size_t expected = req->body.data ? req->body.len : (size_t)0;
        /* val is written to the wire byte-for-byte if accepted, so its
         * grammar must be validated as strictly as this codebase's own
         * request-side parser (chttp1_parser.c's parse_uint64_decimal, used
         * by chttpserver.c) validates an incoming Content-Length: a bare
         * "1*DIGIT" with no sign and no embedded whitespace. strtoull alone
         * is not sufficient here, since (unlike parse_uint64_decimal) it
         * tolerates a leading '+'/'-' and leading whitespace before the
         * digits; a value like "+42" or " 42" numerically equal to the real
         * body length used to pass this check (strtoull("+42", &endp, 10)
         * returns 42 with *endp == '\0') and be written verbatim onto the
         * wire, producing a request this library's own chttpserver rejects
         * outright as "Invalid Content-Length" despite chttp_request_
         * set_header/_serialize_request having accepted it as valid. */
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
          _mem_free(mp, (void *)seen.names);
          _mem_free(mp, ob.buf);
          return ccol_invalid_args;
        }
      }
      if (!_seen_names_add(mp, &seen, name)) {
        ccol_iter_destroy(it);
        _mem_free(mp, (void *)seen.names);
        _mem_free(mp, ob.buf);
        return ccol_not_enough_memory;
      }
      _ob_append_cstr(&ob, name);
      _ob_append(&ob, ": ", 2);
      _ob_append_cstr(&ob, val);
      _ob_append(&ob, "\r\n", 2);
    }
    _mem_free(mp, (void *)seen.names);
  }

  if (!has_ct && body_carrying_method && req->body.content_type) {
    /* Gated on body_carrying_method alone (not also on req->body.data/len),
     * matching the Content-Length synthesis a few lines below, which
     * likewise fires for any body_carrying_method with !has_cl regardless
     * of whether body.data is set. A non-body-carrying method (GET, DELETE,
     * HEAD, OPTIONS) never puts req->body on the wire at all (see the
     * body-append check further down), so advertising a content-type for
     * one of those was a real, previously-untested inconsistency; a caller
     * attaching a body to e.g. a DELETE request (as this file's own
     * delete_body_not_transmitted test does) got a spurious
     * "content-type: ..." header with no body bytes and no Content-Length
     * to match it - body_carrying_method alone is what rules that out.
     * A genuinely empty body on a body-carrying method (body.data == NULL
     * because body.len == 0, e.g. CHTTP_JSON_BODY("", 0)) is a different,
     * legitimate case: it still emits "content-length: 0" below, so a
     * matching "content-type: ..." describing that empty representation is
     * correct HTTP, not spurious, and must not be additionally gated on
     * body.data/len the way the non-body-carrying-method case is.
     *
     * Same CRLF-injection concern as the header loop above; content_type
     * has no dedicated setter to validate it at (it travels in via
     * chttp_request_body_t, copied verbatim by chttp_request_new_mp), so
     * this is its only checkpoint before reaching the wire. */
    if (strpbrk(req->body.content_type, "\r\n")) {
      _mem_free(mp, ob.buf);
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
    _mem_free(mp, ob.buf);
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
  /* Overflow guard on the size computations below, mirroring _ob_append's
   * own identical guard for the sibling (request-serialization) growable
   * buffer: bb->len is a running total across many _on_body callbacks (each
   * individually bounded to a single ~8KB socket read), so unlike a single
   * strlen()-sized allocation elsewhere in this file, no single caller-
   * supplied value needs to be implausibly large on its own for bb->len +
   * len to approach SIZE_MAX; only the CUMULATIVE total across many calls
   * does. Practically unreachable regardless (the real allocator behind
   * _mem_realloc below would already fail well before bb->buf could ever
   * grow anywhere near that size, and that failure is already handled by
   * the existing `if (!nb)` check further down), but this project treats a
   * reducible/unguarded overflow in a size computation as a real bug
   * regardless of how large an input is needed to trigger it. Left
   * unguarded, a wrapped bb->len + len would let the max_size comparison
   * below wrongly conclude an oversized response is still under the cap,
   * and would let the growth loop settle on an nc far smaller than actually
   * needed (or, if nc itself wraps to 0 mid-loop, spin forever), turning
   * the memcpy below into a real heap-buffer overflow instead of a clean,
   * reported allocation failure. */
  if (len > SIZE_MAX - bb->len || bb->len + len > SIZE_MAX - 1) {
    bb->oom = true;
    return 0;
  }
  /* Reactive cap enforcement: covers every body-framing mode (Content-Length,
   * chunked, EOF-delimited), unlike the up-front declared-Content-Length
   * check in _on_headers_complete, which only catches a Content-Length that
   * is honest about being oversized before a single body byte is even read.
   * A short return here (like the OOM case just below) is what makes _on_
   * body abort the parse via CHTTP1_USER instead of silently continuing to
   * grow bb->buf without bound. */
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
    char *nb = (char *)_mem_realloc(bb->mp, bb->buf, nc);
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
 * White-box test helper exposing _sink_buffered's overflow guard directly:
 * sets up a chttp_bodybuf_t whose own accumulated `len` already sits at
 * fake_len (bb.buf stays NULL, no real buffer of that size is ever
 * allocated; the guard must reject before ever touching bb->mp's own
 * malloc/realloc or bb->buf itself) and feeds it a small, real probe chunk.
 * Mirrors _chttp_ob_append_overflow_guard_for_tests's own established
 * pattern for this exact class of guard on the sibling growable buffer.
 * Not part of the public API; gated so this symbol does not leak into a
 * production build of libccollections.so, matching every other white-box
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
  _mem_free(mp, bb.buf);
  return rejected;
}
#endif /* RUNNING_UNIT_TESTS */

/* chttp1_parser hands a header/trailer line to this callback whole (name and
 * value already split and OWS-trimmed), so there is no accumulator state to
 * maintain here at all. name/value point into the parser's own internal
 * line buffer and are only valid for this call, so BOTH need a local,
 * NUL-terminated copy before use: name because it must be lower-cased, and
 * value because this codebase's cmap_pair convention for string values is
 * "size = strlen + 1" (chmap_insert_elem copies exactly that many bytes);
 * reading value_len+1 raw bytes directly out of the parser's own line
 * buffer would read one uncontrolled byte past the value itself, which is
 * not guaranteed to be '\0'. Both buffers are sized to CHTTP1_MAX_LINE_LEN+1,
 * more than enough for any substring of one line. */
static int _on_header(chttp1_parser_t *p, const char *name, size_t name_len,
                      const char *value, size_t value_len) {
  chttp_parse_ctx_t *ctx = (chttp_parse_ctx_t *)p->data;

  char lower_name[CHTTP1_MAX_LINE_LEN + 1];
  for (size_t i = 0; i < name_len; i++)
    lower_name[i] = (char)tolower((unsigned char)name[i]);
  lower_name[name_len] = '\0';

  char value_copy[CHTTP1_MAX_LINE_LEN + 1];
  memcpy(value_copy, value, value_len);
  value_copy[value_len] = '\0';

  cmap_pair kp = {.ptr = lower_name, .size = name_len + 1};
  cmap_pair vp = {.ptr = value_copy, .size = value_len + 1};
  ccol_retval_t rv = chmap_insert_elem(ctx->headers, &kp, &vp);
  if (rv != ccol_success && rv != ccol_key_already_present) {
    ctx->error = true;
    return 1;
  }
  return 0;
}

static int _on_headers_complete(chttp1_parser_t *p) {
  chttp_parse_ctx_t *ctx = (chttp_parse_ctx_t *)p->data;
  ctx->status_code = p->status_code;

  /* Whether this response IS a redirect is always detected here, regardless
   * of how many hops the chain has already used: the caller (chttp_do_
   * internal's hop loop for Tier 1; _async_handle_redirect for Tier 2/3) is
   * the one that knows the current hop count and decides whether to follow
   * it or report ccol_http_too_many_redirects instead; this callback has no
   * hop-count context of its own to gate on. */
  bool is_redirect_status = ctx->status_code == 301 ||
                            ctx->status_code == 302 ||
                            ctx->status_code == 303 ||
                            ctx->status_code == 307 || ctx->status_code == 308;
  if (is_redirect_status) {
    cmap_pair kp = {.ptr = (void *)"location", .size = sizeof("location")};
    cmap_pair *vp = NULL;
    if (chmap_get_elem_ref(ctx->headers, &kp, &vp) == ccol_success && vp) {
      ctx->location = ccol_strdup(ctx->mp, (const char *)vp->ptr);
      if (!ctx->location) {
        ctx->error = true;
        return -1; /* anything outside {0, 1} aborts with CHTTP1_USER */
      }
      ctx->will_redirect = true;
    }
  }

  /* Up-front rejection of an honestly-declared, oversized Content-Length,
   * before a single body byte is read off the wire: only meaningful for a
   * buffered (non-streaming) request that is NOT itself a redirect (a
   * redirect's own body is always discarded via _sink_discard regardless of
   * its declared length, and a streaming caller manages its own memory via
   * chttpcli_write_fn's return value, never chttp_bodybuf_t at all), and only
   * for a message that will actually carry a body onto the wire. Several
   * message classes can legitimately declare a Content-Length that has
   * nothing to do with what will actually be read: a 1xx informational
   * response (RFC 7230 SS3.3.2 says a server MUST NOT send Content-Length on
   * one at all, but a misbehaving or malicious server might anyway; every
   * such response is always discarded/skipped by the caller's own
   * interim-response handling (_chttp_read_message_loop's is_skippable_1xx
   * check, or the parser's own no_body forcing for 1xx a few lines below this
   * callback), never delivered as a final result), a 204/304 response (RFC
   * 7230 SS3.3 treats these identically to 1xx for body-framing purposes: a
   * 304 commonly carries the original resource's own Content-Length per RFC
   * 7232 SS4.1, e.g. from a conditional GET against a CDN, and chttp1_parser
   * itself already unconditionally forces no_body for both regardless of any
   * declared length - see chttp1_parser.c's own identical status-code set),
   * and a HEAD response (whose Content-Length describes what a GET would have
   * returned, per RFC 7231 SS4.3.2, but is never followed by actual body
   * bytes; see this function's own is_head_request check at the very end).
   * Applying this cap to any of these wrongly failed the WHOLE request with
   * ccol_msg_too_large even when the real, eventually-delivered response
   * (always zero bytes for 1xx/204/304, unconditionally) was well within the
   * configured limit: a 103 Early Hints interim response (or any other 1xx)
   * carrying an oversized declared length ahead of a small, well-within-cap
   * final response, a plain HEAD request against a large resource, or an
   * ordinary conditional GET receiving 304 Not Modified with the original
   * resource's oversized Content-Length, all used to fail this way before
   * this check excluded them. A chunked or EOF-delimited body (no
   * Content-Length declared) has no up-front signal to check here regardless;
   * it is still bounded by the reactive per-append check in _sink_buffered
   * below. */
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
      return -1; /* anything outside {0, 1} aborts with CHTTP1_USER */
    }
  }

  ctx->sink_fn = ctx->will_redirect ? _sink_discard : ctx->requested_sink_fn;
  ctx->sink_ctx = ctx->will_redirect ? NULL : ctx->requested_sink_ctx;

  /* A HEAD response's Content-Length (if any) describes a body that was
   * never sent; this is the one case the parser cannot infer from the wire
   * on its own. */
  return ctx->is_head_request ? 1 : 0;
}

static int _on_body(chttp1_parser_t *p, const char *at, size_t len) {
  chttp_parse_ctx_t *ctx = (chttp_parse_ctx_t *)p->data;
  size_t n = ctx->sink_fn ? ctx->sink_fn(at, len, ctx->sink_ctx) : len;
  if (n != len) {
    /* A short return from _sink_buffered means either its realloc failed (an
     * allocation failure, reported via ctx->error so the caller sees
     * ccol_not_enough_memory) or the configured response-size cap was
     * exceeded (reported via ctx->too_large so the caller sees
     * ccol_msg_too_large); neither is a caller-level streaming abort
     * (reported via ctx->aborted -> ccol_http_transfer_aborted). All three
     * surface as the same CHTTP1_USER return from chttp1_parser_execute, so
     * this is the only place that can still tell them apart. */
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
  once_flag_t once;
} client_http1_settings_bundler = {0};

static void _init_chttp1_settings(void) {
  chttp1_settings_init(&client_http1_settings_bundler.settings);
  client_http1_settings_bundler.settings.on_header = _on_header;
  client_http1_settings_bundler.settings.on_headers_complete =
      _on_headers_complete;
  client_http1_settings_bundler.settings.on_body = _on_body;
  client_http1_settings_bundler.settings.on_message_complete =
      _on_message_complete;
}

static void _parse_ctx_free_fields(chttp_parse_ctx_t *ctx) {
  _mem_free(ctx->mp, ctx->location);
  if (ctx->headers) __chmap_destroy(ctx->headers);
  ctx->location = NULL;
  ctx->headers = NULL;
}

/* Resets ctx to parse a SECOND, logically distinct message on the same
 * connection: specifically, the real final response following a "100
 * Continue" interim response the same ctx was just used to parse. Mirrors
 * chttp_do_internal's own "fresh state per hop" convention (a fresh
 * chttp_parse_ctx_t per redirect hop) at the sub-hop granularity this one
 * connection's two-message exchange needs; the interim response's own
 * (rare, but legal) headers must never leak into the final response's
 * header map. is_head_request is left untouched (a property of the
 * request, not of any one parsed message);
 * requested_sink_fn/requested_sink_ctx are also left untouched (the
 * caller's real sink config); sink_fn/sink_ctx are cleared since
 * _on_headers_complete resolves them fresh for the message it's parsing. */
static ccol_retval_t _parse_ctx_reset_for_continue(chttp_parse_ctx_t *ctx) {
  _mem_free(ctx->mp, ctx->location);
  ctx->location = NULL;
  if (ctx->headers) __chmap_destroy(ctx->headers);
  char *herr = NULL;
  ctx->headers =
      chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                        ccol_string, ctx->mp, NULL, &herr);
  ctx->will_redirect = false;
  ctx->message_complete = false;
  ctx->trailing_garbage = false;
  ctx->error = false;
  ctx->aborted = false;
  ctx->too_large = false; /* not currently reachable (every path that sets
                           * this already fails the read via CHTTP1_USER
                           * before this reset ever runs), but reset
                           * anyway so a stale "too large" verdict from a
                           * discarded interim message can never carry
                           * forward into the next one this function
                           * prepares ctx for. */
  ctx->status_code = 0;
  ctx->sink_fn = NULL;
  ctx->sink_ctx = NULL;
  return ctx->headers ? ccol_success : ccol_not_enough_memory;
}

/*
 * Reads and parses exactly one HTTP/1.1 message from `conn`, starting with
 * whatever bytes are already available in `carry_in` (if any; fed to the
 * parser before ever touching the socket) and falling back to ordinary
 * deadline-bounded socket reads once carry_in is exhausted. On
 * ccol_success, *keep_alive_out reflects chttp1_parser's own keep-alive
 * bookkeeping (NOT yet downgraded for trailing garbage; see
 * _chttp_read_response_carry, the only caller that should be treating
 * leftover bytes as garbage in the first place).
 *
 * Unlike treating any bytes past the message boundary as trailing garbage
 * outright, this function reports them via leftover_out/leftover_len_out
 * (heap-allocated with pctx->mp; NULL/0 when there is none) instead;
 * needed so chttp_do_internal's Expect: 100-continue handling can carry a
 * fast server's real final-response bytes forward into a second parse, if
 * they happened to arrive in the same read as the "100 Continue" interim
 * status line. Every other caller has no second message to carry them into
 * and should treat a non-empty leftover exactly like the trailing garbage
 * it actually is; see _chttp_read_response_carry.
 *
 * *any_bytes_read_out is set to true the moment the first byte of THIS
 * message is fed to the parser, whether that byte arrives via a live read
 * off the wire in this call or via carry_in (bytes the caller already read
 * off the wire in an earlier call, e.g. chttp_do_internal's Expect:
 * 100-continue handling threading a fast server's leftover bytes forward
 * into the real final-response read). Both sources are treated identically
 * here, deliberately: a non-empty carry_in is fed straight into
 * chttp1_parser_execute() below, which can invoke on_header/on_body/
 * on_headers_complete against pctx and the caller's sink exactly as a live
 * read would, so a failure partway through this call can leave real,
 * caller-visible state behind even if not one byte was read from the fd
 * during this specific call. Callers use *any_bytes_read_out to decide
 * whether a failure is safe to silently retry against a fresh connection
 * (nothing has been parsed or handed to the caller yet) versus one that
 * must be surfaced (partial response already in flight, possibly already
 * streamed out to a user callback); treating carry_in as exempt used to let
 * chttp_do_internal's reused-connection retry-once safety net reissue a
 * request onto a fresh connection while reusing a chttp_parse_ctx_t/body
 * buffer that a dead carry_in parse had already partially populated,
 * silently mixing a discarded response's headers/body-prefix into the one
 * actually delivered to the caller.
 */
static ccol_retval_t _chttp_read_message(
    chttp_conn_t *conn, chttp_parse_ctx_t *pctx, chttp_deadline_t *overall,
    const char *carry_in, size_t carry_in_len, bool *keep_alive_out,
    bool *any_bytes_read_out, char **leftover_out, size_t *leftover_len_out) {
  call_once(client_http1_settings_bundler.once, _init_chttp1_settings);

  chttp1_parser_t parser;
  chttp1_parser_init(&parser, &client_http1_settings_bundler.settings);
  parser.data = pctx;

  *leftover_out = NULL;
  *leftover_len_out = 0;

  if (carry_in_len > 0) {
    *any_bytes_read_out = true;
    chttp1_errno_t err = chttp1_parser_execute(&parser, carry_in, carry_in_len);
    if (err == CHTTP1_PAUSED) {
      size_t consumed = chttp1_parser_consumed(&parser);
      *keep_alive_out = chttp1_should_keep_alive(&parser);
      if (consumed < carry_in_len) {
        size_t rem = carry_in_len - consumed;
        char *lb = (char *)_mem_alloc(pctx->mp, rem);
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
    /* else: message not yet complete, fall through to reading more */
  }

  char buf[8192];
  for (;;) {
    int wait_ms;
    if (!_deadline_remaining_ms(overall, &wait_ms)) return ccol_timed_out;
    ccol_retval_t prv = _conn_wait(conn->fd, POLLIN, wait_ms);
    if (prv != ccol_success) return prv;

    ssize_t n = _conn_read(conn, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EWOULDBLOCK) continue;
      return ccol_http_transfer_aborted;
    }
    if (n > 0) *any_bytes_read_out = true;
    if (n == 0) {
      /* CHTTP1_PAUSED here (not just CHTTP1_OK) is the expected outcome for
       * a valid EOF-delimited body (HTTP/1.0-style, or an explicit
       * Connection: close with no Content-Length/chunked framing): see
       * chttp1_parser_finish's own doc comment. Only CHTTP1_ERROR (a
       * genuinely truncated message) falls through to the aborted case. */
      chttp1_errno_t fe = chttp1_parser_finish(&parser);
      if ((fe != CHTTP1_OK && fe != CHTTP1_PAUSED) || !pctx->message_complete)
        return ccol_http_transfer_aborted;
      *keep_alive_out = false; /* peer closed; nothing left to reuse */
      return ccol_success;
    }

    chttp1_errno_t err = chttp1_parser_execute(&parser, buf, (size_t)n);
    if (err == CHTTP1_PAUSED) {
      size_t consumed = chttp1_parser_consumed(&parser);
      *keep_alive_out = chttp1_should_keep_alive(&parser);
      if (consumed < (size_t)n) {
        size_t rem = (size_t)n - consumed;
        char *lb = (char *)_mem_alloc(pctx->mp, rem);
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
    /* else: message not yet complete, need more data */
  }
}

/*
 * Like _chttp_read_message, but transparently discards and reads past any
 * interim informational (1xx) response whose status is not stop_at_status
 * (pass 0 to never treat any status as the expected stop code, i.e. always
 * skip every 1xx) until a message worth returning to the caller arrives:
 * either stop_at_status itself, any final (>=200) response, or a hard
 * failure/timeout.
 *
 * This closes a real correctness gap: every caller used to read exactly one
 * message and unconditionally treat it as final, so a server that
 * proactively sends an interim 1xx status other than the one being watched
 * for (most notably "103 Early Hints", RFC 8297, which servers may send
 * ahead of an ORDINARY response, not just ahead of "100 Continue") would be
 * misread as though it WERE the final response: an empty, wrong-status
 * result delivered to the caller while the server's real response sits
 * unread on the wire; corrupting whatever unrelated later request
 * happens to reuse this same connection from the keep-alive pool.
 *
 * Every discarded interim message's own pctx state (headers, Location,
 * etc.) is cleared via _parse_ctx_reset_for_continue before the next read,
 * the same treatment "100 Continue" itself already got, so a discarded
 * message's headers can never leak into the one the caller actually
 * receives. Leftover bytes past a DISCARDED message's own boundary (a fast
 * server that already started writing its next message in the same read)
 * are threaded into the very next read as its carry_in, the same mechanism
 * chttp_do_internal's "100 Continue" handling already relies on; only
 * leftover bytes past the message this function actually RETURNS are
 * reported to the caller via leftover_out/leftover_len_out, exactly
 * matching _chttp_read_message's own contract; what to do with them is
 * left to the caller (_chttp_read_response_carry always treats them as
 * trailing garbage; _chttp_send_and_read's "100 Continue" wait instead
 * carries them forward into the real final read).
 *
 * *any_bytes_read_out, once set true by any discarded interim message
 * actually receiving bytes off the wire, is deliberately never reset back
 * to false for a later message in this same loop: a complete (not merely
 * partial/abandoned) interim response proves the server received and
 * started responding to this request, so it is no longer safe for a
 * caller-level "retry the whole hop against a fresh connection" recovery
 * (which resends the request from scratch) to treat this as if nothing had
 * happened yet; the same reasoning that already gates every other use of
 * any_bytes_read_out in this file.
 */
static ccol_retval_t _chttp_read_message_loop(
    chttp_conn_t *conn, chttp_parse_ctx_t *pctx, chttp_deadline_t *overall,
    const char *carry_in, size_t carry_in_len, int stop_at_status,
    bool *keep_alive_out, bool *any_bytes_read_out, char **leftover_out,
    size_t *leftover_len_out) {
  char *cur_carry = NULL;
  size_t cur_carry_len = 0;
  if (carry_in_len > 0) {
    cur_carry = (char *)_mem_alloc(pctx->mp, carry_in_len);
    if (!cur_carry) return ccol_not_enough_memory;
    memcpy(cur_carry, carry_in, carry_in_len);
    cur_carry_len = carry_in_len;
  }

  /* n_discarded is checked against the cap AFTER a message has been read and
   * found to be an interim (skippable) one, never before attempting a read;
   * see CHTTP_MAX_INTERIM_RESPONSES's own comment ("how many...it will
   * discard before giving up") for why this ordering, not the reverse, is
   * the documented contract. An earlier version of this loop checked the
   * cap up front, gating every read attempt (interim or not) uniformly:
   * once 64 interim responses had been discarded, that version refused to
   * even attempt reading whatever message came next, so a real, final
   * response arriving as the very next (65th) message was wrongly rejected
   * even though only 64 (not 65) interim responses had actually needed
   * discarding. That silently violated this function's own documented
   * "after 64 consecutive discarded interim responses" contract (also
   * published in README.md and chttpclient_do.3) by giving up one message
   * early, and disagreed with the async engine's own equivalent loop
   * (_async_on_readable_impl), which has never gated the final response
   * this way, only the interim ones. Checking the cap here, after
   * classifying the message, makes both tiers agree exactly: a final
   * response is always deliverable no matter its position, and only the
   * interim-discard count itself is bounded. */
  for (size_t n_discarded = 0;;) {
    char *leftover = NULL;
    size_t leftover_len = 0;
    ccol_retval_t rv = _chttp_read_message(
        conn, pctx, overall, cur_carry, cur_carry_len, keep_alive_out,
        any_bytes_read_out, &leftover, &leftover_len);
    _mem_free(pctx->mp, cur_carry);
    cur_carry = NULL;
    cur_carry_len = 0;
    if (rv != ccol_success) {
      _mem_free(pctx->mp, leftover);
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
      _mem_free(pctx->mp, leftover);
      return ccol_http_transfer_aborted;
    }
    n_discarded++;

    ccol_retval_t rrv = _parse_ctx_reset_for_continue(pctx);
    if (rrv != ccol_success) {
      _mem_free(pctx->mp, leftover);
      return rrv;
    }
    cur_carry = leftover;
    cur_carry_len = leftover_len;
  }
}

/* _chttp_read_message_loop (skipping every 1xx status; stop_at_status = 0),
 * with any leftover bytes past the returned message's own boundary
 * collapsed into the ordinary trailing-garbage handling every caller except
 * chttp_do_internal's Expect: 100-continue path actually wants (downgrades
 * keep_alive_out to false, exactly matching this function's previous,
 * inlined behavior before _chttp_read_message was split out). carry_in/
 * carry_in_len are forwarded unchanged. */
static ccol_retval_t _chttp_read_response_carry(
    chttp_conn_t *conn, chttp_parse_ctx_t *pctx, chttp_deadline_t *overall,
    const char *carry_in, size_t carry_in_len, bool *keep_alive_out,
    bool *any_bytes_read_out) {
  char *leftover = NULL;
  size_t leftover_len = 0;
  ccol_retval_t rv = _chttp_read_message_loop(
      conn, pctx, overall, carry_in, carry_in_len, 0, keep_alive_out,
      any_bytes_read_out, &leftover, &leftover_len);
  if (rv == ccol_success && leftover_len > 0) {
    pctx->trailing_garbage = true;
    *keep_alive_out = false;
  }
  _mem_free(pctx->mp, leftover);
  return rv;
}

/* Reads and parses exactly one HTTP/1.1 response from `conn`. See
 * _chttp_read_message's own doc comment for *keep_alive_out/
 * *any_bytes_read_out semantics (trailing-garbage-adjusted here, unlike that
 * lower-level function).
 */
static ccol_retval_t _chttp_read_response(chttp_conn_t *conn,
                                          chttp_parse_ctx_t *pctx,
                                          chttp_deadline_t *overall,
                                          bool *keep_alive_out,
                                          bool *any_bytes_read_out) {
  return _chttp_read_response_carry(conn, pctx, overall, NULL, 0,
                                    keep_alive_out, any_bytes_read_out);
}

/* ========================================================================== */
/*                         TLS CONTEXT MANAGEMENT                             */
/* ========================================================================== */

static bool _file_readable(const char *path) {
  return path && access(path, R_OK) == 0;
}

/*
 * Builds (or rebuilds) cli->tls_ctx from cli->tls. Called both at client
 * construction (default config) and from chttpclient_set_tls.
 *
 * ctls_ctx_cert_add/_trust report failure via an ordinary ccol_retval_t and
 * never touch the process's own lifetime (a missing/unreadable file is
 * always a recoverable error here, never a process abort). chttpclient_set_tls
 * must still be able to accept a syntactically valid but currently-nonexistent
 * path without crashing (callers may configure TLS well before ever making an
 * HTTPS request, or never make one at all); so the configured paths are
 * still validated with access() BEFORE ever calling into ctls. If they don't
 * check out, no context is built here and the failure is deferred to actual
 * connection time (see chttp_do_internal), where it surfaces as a normal
 * ccol_retval_t exactly as before. This pre-check is kept (rather than
 * relying solely on ctls_ctx_cert_add's own graceful failure) to preserve
 * this function's existing "always returns ccol_success, failure is always
 * deferred" contract unchanged.
 */
static ccol_retval_t _rebuild_tls_ctx_locked(struct chttpclient *cli) {
  if (cli->tls_ctx) {
    ctls_ctx_release(cli->tls_ctx);
    cli->tls_ctx = NULL;
  }
  cli->tls_ctx_usable = false;

  bool have_cert_pair = cli->tls.cert_path && cli->tls.key_path;
  if (have_cert_pair && (!_file_readable(cli->tls.cert_path) ||
                         !_file_readable(cli->tls.key_path))) {
    return ccol_success; /* deferred failure, see comment above */
  }
  if (cli->tls.ca_bundle_path && !_file_readable(cli->tls.ca_bundle_path)) {
    return ccol_success; /* deferred failure, see comment above */
  }

  ctls_ctx_t *ctx = ctls_ctx_new_mp(cli->m_procs, NULL);
  if (!ctx) return ccol_not_enough_memory;

  if (have_cert_pair) {
    if (ctls_ctx_cert_add(ctx, NULL, cli->tls.cert_path, cli->tls.key_path,
                          NULL, NULL) != ccol_success) {
      ctls_ctx_release(ctx);
      return ccol_success; /* deferred failure, see comment above */
    }
  }

  if (cli->tls.ca_bundle_path) {
    if (ctls_ctx_trust(ctx, cli->tls.ca_bundle_path, NULL) != ccol_success) {
      ctls_ctx_release(ctx);
      return ccol_success; /* deferred failure, see comment above */
    }
  } else if (cli->tls.verify_peer || cli->tls.verify_host) {
    /* verify_host implies verify_peer: hostname matching against a
     * certificate whose chain was never validated (SSL_VERIFY_NONE, no
     * trust store configured) gives no real security guarantee: the
     * certificate itself could be entirely attacker-forged. Without this,
     * a caller setting verify_peer=false, verify_host=true (plausible if
     * the two are read as independent toggles, which chttp_tls_config_t's
     * field comments now warn against) got a false sense of security: the
     * hostname check would run and "pass" against literally any
     * self-signed certificate for that hostname. */
    if (ctls_ctx_trust_system(ctx) != ccol_success) {
      /* Unlike a plain missing/unreadable CA-bundle file (checked with
       * access() before ctls is ever touched, above), this failure is not
       * pre-checkable: it means the platform's own default CA store
       * couldn't be loaded inside ctls. Discarding ctx entirely here
       * (rather than falling through to install it) matters: ctx's
       * verify_peer flag was already committed to SSL_VERIFY_PEER before
       * this rebuild failed, but the rebuild that would have loaded any
       * trust anchors never committed, so the PREVIOUS, pre-verify_peer
       * ctx_default from ctls_ctx_new_mp's own initial build would remain
       * installed with no certificate verification at all if this ctx were
       * kept; every other failure in this function already discards ctx
       * for the identical reason. */
      ctls_ctx_release(ctx);
      return ccol_success; /* deferred failure, see comment above */
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
 * One static, process-wide event_loop reactor shared by every chttpcli
 * instance in the process (including the lazily-created default client);
 * One of two separate, independent reactors (the other belongs to chttpserver;
 * see chttpserver.c's own identical lifecycle wrapper). chttpclient does not
 * share a process-wide singleton with chttpserver, so this lifecycle wrapper
 * needs no cross-module ordering at all, only ref-counting across Tier 2/3
 * callers; it mirrors chttpserver.c's own g_reactor
 * acquire/release/reaper-thread shape exactly, with chttpclient's own
 * additional resources (cli_engine_bundler.dns_pool, the deadline sweep)
 * layered on top and torn down in lockstep with it.
 */
static struct {
  event_loop reactor;
  size_t reactor_refs;
  mutex_t mutex;
  cond_var_t stopped_cv;
  once_flag_t once;
  bool stopping;
  thread_id_t reaper_thread;
  bool reaper_joinable;
  ccol_memmgmt_procs_t mprocs_storage;
  ccol_memmgmt_procs_t *mprocs;

  /* 0 = auto-detect via sysconf(_SC_NPROCESSORS_ONLN), this module's
   * original, still-default behavior. A positive value pins the reactor to
   * exactly that many OS threads instead; see
   * chttpcli_set_engine_num_reactor_threads's own doc comment. Baked into
   * the reactor at construction time, same "before first start, or after a
   * full stop" restriction as mprocs above. */
  size_t num_reactor_threads;
  /* The value actually passed to event_loop_create_with_mprocs the last time
   * the reactor was created (auto-detected or explicit); for test
   * instrumentation only, see
   * _chttpclient_engine_num_reactor_threads_for_tests below. */
  size_t last_resolved_num_reactor_threads;

  /* Diagnostics logger for chttpclient's own reactor-thread events (TLS
   * handshake failures, connect errors). A caller-installed logger (via
   * chttpcli_set_engine_logger) is used if set before the engine first
   * starts; otherwise _client_engine_acquire installs a CLOG_FATAL-level
   * fallback logger (fd 2) the first time the engine starts, so this field
   * is never actually NULL for the engine's whole running lifetime; not
   * merely an internal detail: _client_engine_reaper_fn's own teardown
   * log_info call, and the deadline sweep's, both rely on having SOME
   * live logger to call through (chttp1_parser has no equivalent of its
   * own; _clog_write itself has no NULL-handle guard, so calling through a
   * genuinely NULL logger would crash, not silently skip). Only the rare
   * logger-allocation-itself-fails-with-OOM path ever reaps the engine with
   * this left NULL (see that path's own comment in _client_engine_acquire).
   * Guarded by cli_engine_bundler.mutex purely against a torn pointer
   * read/write racing a concurrent chttpcli_set_engine_logger() call (clog
   * itself is already thread-safe for concurrent logging calls through one
   * handle). */
  clog logger;

  /* Offloads the (potentially blocking) DNS-resolve-and-connect step off of
   * caller threads. Lives and dies with the reactor itself (created/destroyed
   * alongside it, guarded by cli_engine_bundler.mutex) rather than being a
   * separate lazy singleton, since nothing needs it once the reactor is down.
   */
  ctpool dns_pool;
} cli_engine_bundler = {0};

static ccol_retval_t _client_deadline_sweep_start(void);
static void _client_deadline_sweep_stop_and_join(void);

static void _client_engine_globals_init(void) {
  if (mutex_init(cli_engine_bundler.mutex) != 0)
    fatal_err("chttpclient engine: failed to initialize mutex");
  if (cond_var_init(cli_engine_bundler.stopped_cv) != 0)
    fatal_err("chttpclient engine: failed to initialize condition variable");
  /* Belt-and-suspenders: Tier 1 already passes MSG_NOSIGNAL to every send(),
   * and Tier 2/3's own raw writes do the same (see _async_on_writable), so
   * this is not load-bearing the way chttpserver.c's identical call is for
   * its own worker-thread writes; but it costs nothing and protects any
   * future raw write path in this file from an unexpected process-killing
   * SIGPIPE regardless. */
  signal(SIGPIPE, SIG_IGN);
}

/*
 * Logs through cli_engine_bundler.logger with the read of that pointer and
 * the actual log_info call combined into ONE critical section, rather than
 * copying the pointer out under the lock and using it afterward with no
 * lock held at all (the shape this replaced). That earlier shape was a
 * real use-after-free: chttpcli_set_engine_logger() swaps in a new logger
 * under cli_engine_bundler.mutex but calls clog_close() on the OLD one only
 * after releasing it (clog_close() unconditionally frees the handle itself,
 * regardless of the shared backing store's own separate refcount; see
 * clogger.c), so a caller that had already copied the old pointer out
 * before the swap, and only used it afterward, could call log_info() on
 * memory that had just been freed out from under it. Kept as a macro
 * (rather than a wrapping function) specifically so log_info's own
 * __FILE__/__LINE__/__func__ capture still reflects the real call site, not
 * this helper's. */
#define _CLIENT_ENGINE_LOG_INFO(fmt, ...)                            \
  do {                                                               \
    call_once(cli_engine_bundler.once, _client_engine_globals_init); \
    mutex_lock(cli_engine_bundler.mutex);                            \
    if (cli_engine_bundler.logger)                                   \
      log_info(cli_engine_bundler.logger, fmt, ##__VA_ARGS__);       \
    mutex_unlock(cli_engine_bundler.mutex);                          \
  } while (0)

static void _client_engine_join_reaper_if_needed_locked(void) {
  if (cli_engine_bundler.reaper_joinable) {
    thread_join(cli_engine_bundler.reaper_thread);
    cli_engine_bundler.reaper_joinable = false;
  }
}

/*
 * Runs on a freshly spawned thread (never inline on the calling thread that
 * dropped the last reference; that thread is routinely a reactor callback
 * thread itself, e.g. _async_on_error tearing down the last live ctx, and
 * must never block on joining the deadline sweep or draining
 * cli_engine_bundler.dns_pool). Tears down the deadline sweep and
 * cli_engine_bundler.dns_pool, destroys the reactor, then clears
 * cli_engine_bundler.stopping so a waiting acquirer can proceed.
 */
static void *_client_engine_reaper_fn(void *arg) {
  (void)arg;
  event_loop loop_to_destroy;
  ctpool dns_pool_to_destroy;
  mutex_lock(cli_engine_bundler.mutex);
  loop_to_destroy = cli_engine_bundler.reactor;
  dns_pool_to_destroy = cli_engine_bundler.dns_pool;
  mutex_unlock(cli_engine_bundler.mutex);

  /* Stop the deadline sweep before tearing down the reactor and DNS pool it
   * may still be referencing (a sweep tick closes a fd/removes a
   * registration directly; see _client_deadline_sweep_once). */
  _client_deadline_sweep_stop_and_join();
  if (loop_to_destroy) event_loop_destroy(loop_to_destroy);
  /* ctpool_destroy (an implicit drain shutdown, joining every dns_pool
   * worker thread - potentially slow if one is mid-getaddrinfo()/connect())
   * is called here, outside the lock, mirroring event_loop_destroy just
   * above it rather than being held under cli_engine_bundler.mutex the way
   * an earlier version of this function did. That earlier version let any
   * concurrent, otherwise-cheap call to chttpcli_set_engine_logger/_set_
   * engine_mem_mgmt_procs/_set_engine_num_reactor_threads (all of which
   * take this same mutex unconditionally, with no cond-var wait to yield
   * it) block for the full duration of the drain, with no real ordering
   * requirement forcing that - reactor/dns_pool are only ever cleared
   * together, under the single critical section below, regardless of
   * where the actual destroy call happens relative to it. */
  if (dns_pool_to_destroy) ctpool_destroy(dns_pool_to_destroy);

  mutex_lock(cli_engine_bundler.mutex);
  cli_engine_bundler.dns_pool = CTPOOL_INVALID;
  cli_engine_bundler.reactor = EVENT_LOOP_INVALID;
  cli_engine_bundler.stopping = false;
  /* logger can legitimately still be NULL here: _client_engine_acquire's own
   * fallback-logger allocation can itself fail, in which case it reaps the
   * engine via this exact same reaper path (see that function's own
   * comment) with no logger ever having been installed; _clog_write has no
   * NULL-handle guard of its own, so logging unconditionally here would
   * crash instead of simply skipping diagnostics, exactly like every other
   * log_info call site in this file already does via its own `if (el)`/
   * `if (logger)` guard. */
  if (cli_engine_bundler.logger)
    log_info(cli_engine_bundler.logger,
             "The http client reactor engine has been destroyed");
  clog old_logger = cli_engine_bundler.logger;
  cli_engine_bundler.logger = CLOG_INVALID;
  cond_var_broadcast(cli_engine_bundler.stopped_cv);
  mutex_unlock(cli_engine_bundler.mutex);
  if (old_logger) clog_close(old_logger);
  return NULL;
}

static void _client_engine_spawn_reaper(void) {
  thread_id_t reaper;
  if (thread_create(reaper, _client_engine_reaper_fn, NULL) != 0) {
    /* No safer fallback than running it inline (OOM-class failure); nothing
     * to join afterward since it already ran to completion synchronously. */
    _client_engine_reaper_fn(NULL);
    return;
  }
  mutex_lock(cli_engine_bundler.mutex);
  cli_engine_bundler.reaper_thread = reaper;
  cli_engine_bundler.reaper_joinable = true;
  mutex_unlock(cli_engine_bundler.mutex);
}

/*
 * Acquires a reference to chttpclient's own reactor, lazily creating it (and
 * cli_engine_bundler.dns_pool, and starting the deadline sweep) on the first
 * call. Subsequent calls just bump cli_engine_bundler.reactor_refs. Must be
 * paired with exactly one _client_engine_release() call.
 */
static ccol_retval_t _client_engine_acquire(void) {
  call_once(cli_engine_bundler.once, _client_engine_globals_init);
  mutex_lock(cli_engine_bundler.mutex);
  while (cli_engine_bundler.stopping)
    cond_var_wait(cli_engine_bundler.stopped_cv, cli_engine_bundler.mutex);
  _client_engine_join_reaper_if_needed_locked();

  if (!cli_engine_bundler.reactor) {
    size_t nthreads = cli_engine_bundler.num_reactor_threads;
    if (nthreads == 0) {
      long cpus = sysconf(_SC_NPROCESSORS_ONLN);
      nthreads = (cpus > 0) ? (size_t)cpus : 1;
    }
    cli_engine_bundler.last_resolved_num_reactor_threads = nthreads;
    char *err = NULL;
    cli_engine_bundler.reactor = event_loop_create_with_mprocs(
        256, 4, nthreads, cli_engine_bundler.mprocs, &err);
    if (!cli_engine_bundler.reactor) {
      mutex_unlock(cli_engine_bundler.mutex);
      return ccol_not_enough_memory;
    }

    char *dns_err = NULL;
    cli_engine_bundler.dns_pool = create_cthread_pool(nthreads, 0, &dns_err);
    if (!cli_engine_bundler.dns_pool) {
      /* Capture the reactor handle and clear the struct field BEFORE
       * releasing the mutex, so a concurrent _client_engine_acquire sees a
       * clean "nothing built yet" state and retries construction itself,
       * rather than observing a reactor field that still looks live while
       * this thread blocks joining every reactor thread. The blocking
       * destroy itself runs AFTER the mutex is released: nothing else can
       * be relying on this mutex to make progress at this exact point (this
       * freshly created reactor has zero registrations, and the deadline
       * sweep thread has not been started yet either), so holding the
       * mutex across it serves no correctness purpose, only needlessly
       * blocks any other thread trying to acquire/release/reconfigure the
       * engine for the duration of the join. */
      event_loop stale_reactor = cli_engine_bundler.reactor;
      cli_engine_bundler.reactor = EVENT_LOOP_INVALID;
      mutex_unlock(cli_engine_bundler.mutex);
      __event_loop_destroy(stale_reactor);
      return ccol_not_enough_memory;
    }

    if (_client_deadline_sweep_start() != ccol_success) {
      /* Same reasoning as the dns_pool-creation-failure branch just above:
       * clear both fields under the mutex, then release it before the two
       * blocking destroys. The deadline sweep thread failed to even start
       * here, so (unlike the logger-allocation-failure branch below, which
       * has an already-running sweep thread to contend with) there is
       * nothing else that could be depending on either handle at this
       * point. */
      ctpool stale_dns_pool = cli_engine_bundler.dns_pool;
      event_loop stale_reactor = cli_engine_bundler.reactor;
      cli_engine_bundler.dns_pool = CTPOOL_INVALID;
      cli_engine_bundler.reactor = EVENT_LOOP_INVALID;
      mutex_unlock(cli_engine_bundler.mutex);
      __ctpool_destroy(stale_dns_pool);
      __event_loop_destroy(stale_reactor);
      return ccol_unexpected_failure;
    }

    if (!cli_engine_bundler.logger) {
      cli_engine_bundler.logger =
          clog_open_fd_mp(2, CLOG_FATAL, NULL, cli_engine_bundler.mprocs);
      if (!cli_engine_bundler.logger) {
        /* Unlike the three rollback branches above (reactor/dns_pool/sweep
         * creation failing), the sweep thread is ALREADY running at this
         * point, so this cannot simply destroy everything inline the same
         * way: the sweep thread itself uses _CLIENT_ENGINE_LOG_INFO, which
         * takes cli_engine_bundler.mutex, so synchronously joining it while
         * still holding that same mutex here would deadlock (the sweep
         * thread's own in-flight tick could be blocked waiting for exactly
         * this lock). Unlocking first and joining afterward isn't
         * safe either: a concurrent _client_engine_acquire call could see
         * cli_engine_bundler.reactor already non-NULL in that window, skip
         * this whole "create everything" branch entirely, and hand out a
         * live reference to a reactor this thread is about to destroy out
         * from under it. The correct fix is the same one
         * _client_engine_release() already uses for "last reference just
         * dropped, tear down without blocking the calling thread": mark
         * the bundle stopping (so any concurrent acquirer's own top-of-
         * function wait loop blocks instead of proceeding) and hand the
         * actual teardown off to a freshly spawned reaper thread, exactly
         * as if this acquire's not-yet-granted reference had been acquired
         * and immediately released. Without this, a logger allocation
         * failure here permanently leaked the reactor, dns_pool, and
         * (already-running) sweep thread for the remaining life of the
         * process, since reactor_refs never left 0 and nothing would ever
         * call _client_engine_release() to reap them. */
        cli_engine_bundler.stopping = true;
        mutex_unlock(cli_engine_bundler.mutex);
        _client_engine_spawn_reaper();
        return ccol_not_enough_memory;
      }
      clog_set_field(cli_engine_bundler.logger, "component",
                     "http-client-engine");
    }

    log_info(cli_engine_bundler.logger,
             "New http client reactor engine has been created");
  }
  cli_engine_bundler.reactor_refs++;
  mutex_unlock(cli_engine_bundler.mutex);
  return ccol_success;
}

/*
 * Releases a reference acquired via _client_engine_acquire(). Once
 * cli_engine_bundler.reactor_refs returns to zero, hands the actual teardown
 * off to a freshly spawned reaper thread rather than performing it inline;
 * this is essential, not just a style choice, since this is routinely called
 * from inside a reactor callback thread itself (_async_on_error tearing down
 * the last live ctx), which must never block joining the deadline sweep or
 * draining cli_engine_bundler.dns_pool (a real hang, caught in testing, in the
 * single-owner predecessor of this exact design).
 */
static void _client_engine_release(void) {
  bool should_reap = false;
  mutex_lock(cli_engine_bundler.mutex);
  if (cli_engine_bundler.reactor_refs > 0) cli_engine_bundler.reactor_refs--;
  if (cli_engine_bundler.reactor_refs == 0 && cli_engine_bundler.reactor) {
    cli_engine_bundler.stopping = true;
    should_reap = true;
  }
  mutex_unlock(cli_engine_bundler.mutex);
  if (should_reap) _client_engine_spawn_reaper();
}

/*
 * Blocks until any in-flight reaper thread (see _client_engine_release) has
 * fully finished tearing this module's resources down. A no-op if not
 * currently stopping (including if not running at all, or running and
 * staying up because other Tier 2/3 references remain); this is
 * deliberately NOT "block until the engine eventually stops on its own"
 * (that would hang forever against a healthy, still-referenced engine);
 * callers use this only to wait for a teardown they know they just
 * triggered (releasing their own last reference) to actually finish.
 *
 * Gated behind RUNNING_UNIT_TESTS: unlike chttpsvr_engine_wait() on the
 * server side, this module exposes no public equivalent (the engine starts
 * and stops on its own as Tier 2/3 usage comes and goes, with no atexit
 * safety net needing to wait on it either; see the "SHARED STATIC
 * REACTOR" section above), so this primitive's only caller in a production
 * build would otherwise be none at all; it exists purely for
 * _chttpclient_engine_wait_for_quiescence_for_tests below.
 */
#ifdef RUNNING_UNIT_TESTS
static void _client_engine_wait_for_quiescence(void) {
  call_once(cli_engine_bundler.once, _client_engine_globals_init);
  mutex_lock(cli_engine_bundler.mutex);
  while (cli_engine_bundler.stopping)
    cond_var_wait(cli_engine_bundler.stopped_cv, cli_engine_bundler.mutex);
  _client_engine_join_reaper_if_needed_locked();
  mutex_unlock(cli_engine_bundler.mutex);
}
#endif /* RUNNING_UNIT_TESTS */

ccol_retval_t chttpcli_set_engine_logger(clog cl) {
  if (!cl) return ccol_invalid_args;
  clog derived = clog_derive(cl);
  if (!derived) return ccol_not_enough_memory;
  clog_set_field(derived, "component", "http-client-engine");
  call_once(cli_engine_bundler.once, _client_engine_globals_init);
  mutex_lock(cli_engine_bundler.mutex);
  clog old = cli_engine_bundler.logger;
  cli_engine_bundler.logger = derived;
  mutex_unlock(cli_engine_bundler.mutex);
  if (old) clog_close(old);
  return ccol_success;
}

ccol_retval_t chttpcli_set_engine_mem_mgmt_procs(ccol_memmgmt_procs_t *mp) {
  if (mp && (!mp->malloc || !mp->free || !mp->calloc || !mp->realloc))
    return ccol_invalid_args;
  call_once(cli_engine_bundler.once, _client_engine_globals_init);
  mutex_lock(cli_engine_bundler.mutex);
  if (cli_engine_bundler.reactor) {
    mutex_unlock(cli_engine_bundler.mutex);
    return ccol_not_permitted;
  }
  if (mp) {
    cli_engine_bundler.mprocs_storage = *mp;
    cli_engine_bundler.mprocs = &cli_engine_bundler.mprocs_storage;
  } else {
    cli_engine_bundler.mprocs = NULL;
  }
  mutex_unlock(cli_engine_bundler.mutex);
  return ccol_success;
}

ccol_retval_t chttpcli_set_engine_num_reactor_threads(size_t num_threads) {
  call_once(cli_engine_bundler.once, _client_engine_globals_init);
  mutex_lock(cli_engine_bundler.mutex);
  if (cli_engine_bundler.reactor) {
    mutex_unlock(cli_engine_bundler.mutex);
    return ccol_not_permitted;
  }
  cli_engine_bundler.num_reactor_threads = num_threads;
  mutex_unlock(cli_engine_bundler.mutex);
  return ccol_success;
}

/* ========================================================================== */
/*                    ASYNC CONNECTION STATE MACHINE (STEP A + B)             */
/* ========================================================================== */

/*
 * Tier 2/3 engine: a non-blocking HTTP or HTTPS request/response cycle per
 * connection, with redirect-following layered on top of a single-hop
 * pipeline, driven by chttpclient's own event_loop reactor (see the "SHARED
 * STATIC REACTOR" section above).
 *
 * TLS: uses the same reactor-agnostic, client-mode ctls API Tier 1 uses
 * (ctls_conn_create_client / ctls_conn_handshake_step / ctls_conn_read /
 * ctls_conn_write), driven from on_readable/on_writable instead of a
 * blocking poll() loop.
 *
 * No tls_lock is needed here: event_loop's own per-event_entry dispatch_lock
 * (see cthreadcomm's own documentation) guarantees that a registration's
 * callback is never invoked concurrently with itself, and, more strongly,
 * that both directions of one fd (on_readable/on_writable) are never
 * dispatched concurrently with each other either. That guarantee is exactly
 * what lets this module drive OpenSSL on one shared connection from either
 * callback with no additional locking of its own.
 *
 * Write-readiness re-arming needs no special-casing: event_loop is
 * level-triggered and keeps a registration live until explicitly removed
 * (event_loop_modify is used instead, purely to flip which direction(s) are
 * of interest), so there is no "forgot to re-arm a one-shot write interest"
 * window to guard against for raw (queue-bypassing) TLS writes.
 *
 * Connection identity is its raw fd plus the event_reg handle returned by
 * event_loop_add for its current registration (EVENT_REG_INVALID while none
 * is active, e.g. mid-connect via the DNS/connect pool, or while idle-pooled).
 * event_loop's own generation counter (event_loop_reg_generation) is
 * available for defensive bookkeeping but is not needed for correctness
 * here: event_loop's dispatch path already re-validates a registration's
 * liveness under its own lock at dispatch time, so a stale, already-fetched
 * epoll_wait batch entry for a since-removed registration is always a safe
 * no-op regardless.
 *
 * event_loop_remove only protects event_loop's OWN internal structures via
 * deferred/epoch-based freeing; it does NOT protect this module's OWN ctx
 * payload from a dispatch that was already in flight when the removal
 * happened. Every place that ends a connection therefore goes through one
 * of two small, explicit teardown helpers (_async_ctx_finish/
 * _async_idle_ctx_finish, defined after the idle pool section below) rather
 * than relying on any implicit "on_close eventually runs" guarantee: there
 * is no unconditional terminal callback under event_loop, so this module
 * must be explicit about ending a connection instead.
 *
 * Redirect-chain lifetime: a redirect chain spans multiple connections (one
 * chttp_async_ctx_t per hop, exactly mirroring Tier 1's "a fresh
 * chttp_parse_ctx_t per hop" comment), but must still fulfil the caller's
 * future exactly once and release exactly one engine reference for the whole
 * chain. chttp_async_chain_t is the small heap-allocated struct that
 * outlives any single hop's ctx to carry that shared state: the future, the
 * fulfilled-once guard, an owned deep copy of the original request's headers
 * and body, the pinned TLS context (constant across hops, exactly like
 * Tier 1's tls_ctx local), and a refcount tracking how many hops' ctx are
 * currently live. Every ctx teardown path releases one chain reference;
 * when the count reaches zero, _async_chain_release both frees the chain
 * and, via a fulfilled-guarded backstop, fulfils the future with a generic
 * error if nothing else already did.
 */

/* The ctpool_future's result is a chttpcli_async_result_t (public, declared
 * in chttpclient.h); built by _async_fulfill_chain below. */

typedef enum {
  CHTTP_ASYNC_CONNECTING,
  CHTTP_ASYNC_TLS_HANDSHAKING,
  CHTTP_ASYNC_WRITING,
  CHTTP_ASYNC_AWAITING_CONTINUE, /* Expect: 100-continue only: the header
                                  * block has been fully written and this
                                  * ctx is registered for READ, waiting up
                                  * to ctx->continue_deadline for either an
                                  * interim "100 Continue" (resume writing
                                  * the body), a direct final response (the
                                  * body is never sent), or the deadline
                                  * sweep to flip this registration to
                                  * WRITE as a timeout signal (send the
                                  * body anyway); see ctx->want_100_continue
                                  * and ctx->continue_decided's own field
                                  * comments for the full design. */
  CHTTP_ASYNC_READING,
  CHTTP_ASYNC_IDLE, /* sitting in cli->idle_pools_async, not owned by any
                     * chain (ctx->chain == NULL); see the "ASYNC IDLE
                     * POOL" section below */
} chttp_async_state_t;

/*
 * Shared, per-request (not per-hop) state for a redirect chain. See the
 * file-level comment above for the ownership/lifetime rationale.
 */
typedef struct {
  ccol_memmgmt_procs_t *mp;
  struct chttpclient *cli; /* owning client; needed by _async_submit_hop to
                            * reach the async idle pool (cli->idle_pools_async)
                            * for both taking a reusable connection and
                            * offering one back */
  ctpool_future *future;   /* the caller's own reference is separate; see
                            * _chttp_do_async_internal's return value; this
                            * module only ever calls ctpool_future_fulfill on
                            * it (the producer-side reference) */

  chttpcli_write_fn write_fn; /* NULL for a buffered (chttpclient_do_async)
                               * request; non-NULL for a streaming
                               * (chttpclient_do_async_streaming) one. Wired
                               * into every hop's ctx->pctx.requested_sink_fn
                               * in _async_submit_hop/_async_retry_hop;
                               * constant across the whole chain, exactly
                               * like Tier 1's identical streaming/write_fn
                               * locals in chttp_do_internal. Called on
                               * whichever reactor thread is driving this
                               * hop's on_data callback; see this field's
                               * public documentation on
                               * chttpclient_do_async_streaming for the
                               * must-not-block contract that implies. */
  void *write_ctx;            /* passed verbatim to write_fn */

  chmap req_headers; /* owned deep copy of the original request's headers
                      * (chmap(char* -> char*)); re-sent unchanged on every
                      * hop, exactly like Tier 1's hop_req.headers. NULL if
                      * the original request had none. */
  void *body_data;   /* owned deep copy of the original request body bytes;
                      * NULL if the original request had none. Re-sent
                      * verbatim on 307/308 hops; a non-preserving redirect
                      * (301/302/303 with a non-HEAD method) drops it. */
  size_t body_len;
  char *body_content_type; /* owned copy; NULL if none */
  bool body_dropped;       /* Latches true, permanently, the first time a
                            * non-preserving redirect drops the body; mirrors Tier
                            * 1's chttp_do_internal, which persists this by
                            * overwriting its cur_body loop-local with an empty
                            * body once dropped, so a LATER 307/308 on the same
                            * chain preserves "whatever the current, possibly
                            * already-dropped, body is", not the original one.
                            * _async_handle_redirect has no per-ctx equivalent of
                            * Tier 1's cur_body (a fresh chttp_async_ctx_t exists
                            * only for the hop that is currently in flight), so
                            * this chain-level flag is what makes that same
                            * "once dropped, stays dropped" persistence hold
                            * across hops here too; without it, a 307/308 hop
                            * following an earlier non-preserving downgrade would
                            * resurrect body_data/body_len/body_content_type from
                            * this struct's own ORIGINAL, hop-0 values instead of
                            * the empty body the chain had already moved to.
                            * Mutated only from _async_handle_redirect, which
                            * always runs synchronously on whichever thread just
                            * finished parsing a hop's response and calls
                            * _async_submit_hop for the next hop from the same
                            * call stack; safe without additional locking for the
                            * same reason carried_auth/explicit_auth_suppressed
                            * above already are (see this struct's own comments on
                            * those fields). */

  char *carried_auth;        /* auto-injected-from-userinfo Authorization
                              * value carried forward across hops, mirroring
                              * Tier 1's identical carried_auth local in
                              * chttp_do_internal; see that function's own
                              * comment for the full same-origin-carry /
                              * cross-origin-drop-permanently contract. NULL
                              * if no userinfo has been seen on this chain
                              * (yet, or ever). Mutated only from
                              * _async_submit_hop, at the same point the
                              * method/body downgrade decision already
                              * mutates this chain unguarded; see that
                              * function's own comment on why no additional
                              * locking is needed. */
  char *carried_auth_origin; /* origin_key the above was derived for */

  char *initial_origin_key; /* origin_key of hop 0 (the request as originally
                             * submitted), captured once in
                             * _chttp_do_async_internal before the chain's
                             * first hop is ever queued. Used purely to
                             * detect a cross-origin redirect for the check
                             * below; unlike carried_auth_origin, this never
                             * changes for the chain's whole lifetime. */
  bool explicit_auth_suppressed; /* Latches true, permanently, the first time
                                  * a hop's origin differs from
                                  * initial_origin_key; mirrors carried_auth's
                                  * own "dropped permanently, never
                                  * re-acquired" cross-origin contract, but
                                  * for a caller-set (via
                                  * chttp_request_set_header, not URL
                                  * userinfo) "authorization" header in
                                  * req_headers: unlike the auto-injected
                                  * userinfo case (already correctly
                                  * origin-scoped via carried_auth), an
                                  * explicit header sitting in req_headers has
                                  * no origin of its own and would otherwise
                                  * be re-sent verbatim to every hop
                                  * regardless of origin, leaking credentials
                                  * to a redirect target on a different
                                  * origin (matching curl's own
                                  * CVE-2018-1000007-hardened default: a
                                  * user-set Authorization header is dropped,
                                  * not forwarded, once a redirect crosses to
                                  * a different host). Mutated only from
                                  * _async_submit_hop, at the same point
                                  * carried_auth/carried_auth_origin already
                                  * mutate this chain unguarded; see that
                                  * field's own comment for why no additional
                                  * locking is needed. */

  ctls_ctx_t *tls_ctx; /* pinned once (ctls_ctx_retain'd from cli->tls_ctx)
                        * for the whole chain, exactly like Tier 1's tls_ctx
                        * local; a redirect can hop between http and
                        * https, so this must survive every hop regardless
                        * of which scheme the chain started with. Released
                        * once via ctls_ctx_release when the chain is freed. */
  bool tls_ctx_usable;
  bool verify_host;

  long connect_timeout_ms; /* re-read fresh into ctx->connect_deadline at the
                            * start of every hop that actually connects (the
                            * reused-connection path never consults it, since
                            * it skips CONNECTING/TLS_HANDSHAKING entirely);
                            * see the "ASYNC DEADLINE SWEEP" section below. */
  chttp_deadline_t overall_deadline; /* computed once, here, for the whole
                                      * chain's lifetime; mirrors Tier 1's
                                      * overall_dl, computed once before its
                                      * hop loop rather than reset per hop. */
  size_t max_response_body_size;     /* snapshot of cli->max_response_body_size,
                                      * taken once in _chttp_do_async_internal
                                      * exactly like connect_timeout_ms/overall_
                                      * deadline above; copied into every hop's
                                      * ctx->bb.max_size in _async_submit_hop/
                                      * _async_retry_hop. 0 = unlimited. */
  bool expect_continue; /* snapshot of the original chttp_request_t.
                         * expect_continue, taken once in
                         * _chttp_do_async_internal, mirroring connect_
                         * timeout_ms/max_response_body_size above. Whether
                         * a given HOP actually routes through the
                         * Expect: 100-continue wait is still recomputed
                         * per hop in _async_submit_hop (mirroring Tier 1's
                         * identical per-hop use_100_continue local),
                         * exactly like Tier 1's own request-level flag vs.
                         * per-hop decision split, since a redirect can
                         * downgrade the method/drop the body, making the
                         * wait moot for a later hop even though this flag
                         * itself never changes for the chain's lifetime. */

  mutex_t lock; /* guards fulfilled and refcount, both mutated from
                 * potentially concurrent hops' reactor callbacks (see the
                 * file-level comment: an old hop's on_close can fire
                 * concurrently with a new hop's on_data/on_ready once the
                 * redirect handoff has queued the next connection) */
  bool fulfilled;
  int refcount; /* number of currently-live per-hop ctx referencing this
                 * chain; reaches zero exactly once, when the last hop's
                 * teardown runs with no further hop having been queued to
                 * take over */
} chttp_async_chain_t;

/* Tagged (not anonymous) specifically so deadline_prev/deadline_next below
 * can self-reference the struct; an anonymous struct has no name to spell
 * a pointer to itself with. */
typedef struct chttp_async_ctx_s {
  ccol_memmgmt_procs_t *mp;
  struct chttpclient *cli; /* owning client; needed while idle
                            * (chain == NULL then) to reach
                            * cli->idle_pools_async, and while active to
                            * offer this connection back to the pool on a
                            * reusable completion */
  chttp_async_chain_t
      *_Atomic chain;        /* shared, whole-redirect-chain state;
                              * not owned by ctx; see _async_ctx_teardown.
                              * NULL while ctx is idle-pooled. _Atomic (like
                              * state/fd below) so the deadline sweep can
                              * read it without idle_lock; see that lock's
                              * own comment and the "ASYNC DEADLINE SWEEP"
                              * section for why individual atomic loads are
                              * sufficient there even though idle_lock's
                              * stronger *compound* (state+chain together)
                              * consistency guarantee remains necessary, and
                              * is left completely undisturbed, for every
                              * pre-existing idle_lock call site. */
  int hop;                   /* 0-based hop index of THIS connection */
  chttp_method_t cur_method; /* method used to build THIS hop's wire; the
                              * basis for deciding the next hop's method on
                              * redirect, exactly like Tier 1's cur_method
                              * loop variable */
  _Atomic chttp_async_state_t state;
  _Atomic int fd; /* -1 until the connect task obtains a real fd */
  _Atomic(event_reg)
      reg; /* current event_loop registration, or EVENT_REG_INVALID
            * while none is active (mid-connect via the DNS/connect
            * pool, or while idle-pooled with no direction of
            * interest yet needed). Set exactly once, by
            * _async_connect_task, right after event_loop_add
            * returns; and _Atomic specifically because that
            * assignment is NOT safe to treat as "invisible until a
            * dispatch could care": event_loop_add's own internal
            * registration goes live (dispatchable by another
            * reactor thread) as part of the call itself, which can
            * complete and hand a callback to a DIFFERENT thread
            * before this thread's own `ctx->reg = event_loop_add(
            * ...)` assignment has finished executing; especially
            * likely for a loopback connect, which is often already
            * writable the instant it's registered. A plain
            * (non-atomic) pointer here was a real, TSan-caught data
            * race (an earlier version of this comment's own claim
            * that this field is "never read lock-free" was simply
            * wrong, found only by running ThreadSanitizer against a
            * suite that kept intermittently failing
            * async_idle_pool.dead_connection_detected_and_retried,
            * not by re-reading this code and noticing it). Every
            * dispatch callback (_async_on_writable/_on_readable/
            * _on_error) treats a NULL read here as "registration
            * not yet published" and returns immediately without
            * touching anything else; this is self-healing rather
            * than a bug, since the underlying condition (a
            * writable/readable/errored fd) is level-triggered and
            * gets re-reported on the very next epoll_wait, by which
            * point the assignment has long since completed. */

  bool is_unix;
  char *unix_socket_path; /* owned copy; NULL unless is_unix */
  char *host;             /* owned copy; NULL if is_unix. port is passed
                           * separately */
  uint16_t port;
  bool is_ipv6;         /* host is a raw (unbracketed) IPv6 literal; mirrors
                         * chttp_url_t.is_ipv6. Needed by _async_handle_redirect to
                         * correctly re-bracket the host when reconstructing a
                         * redirect target; omitting this (an earlier version of this
                         * struct had no such field at all) silently produced a
                         * malformed "scheme://<unbracketed-ipv6>:port/path" redirect
                         * target that failed to re-parse on the next hop. */
  char *path_and_query; /* owned copy of THIS HOP's request path+query, e.g.
                         * "/a/b?x=1"; mirrors chttp_url_t.path_and_query.
                         * Captured fresh in _async_submit_hop on EVERY hop,
                         * even a reused connection, since the path can differ
                         * request to request even when host/port/origin_key
                         * (and therefore the idle-pool origin) stay the same;
                         * carried across a retry by _async_retry_hop, exactly
                         * like host/unix_socket_path/origin_key. Needed by
                         * _async_handle_redirect to resolve a relative-path
                         * Location header against this hop's own URL, the
                         * same way Tier 1's per-hop chttp_url_t url local
                         * already does; without it (an earlier version of
                         * this struct had no such field), _async_handle_
                         * redirect's own hand-built chttp_url_t base always
                         * left this NULL, and any genuinely relative (not
                         * absolute-path, not a full URL, not protocol-
                         * relative) Location header crashed the process via
                         * a NULL-pointer strchr() inside _merge_ref_path. */
  char *origin_key;     /* owned copy, "scheme://host:port" or "unix://<path>";
                         * matches Tier 1's chttp_conn_t.origin_key; used to
                         * place/remove this ctx in cli->idle_pools_async */
  bool reused;          /* true if this hop's connection came from the idle
                         * pool rather than a fresh connect, for THIS
                         * attempt (a retry always resets this to false;
                         * see _async_retry_hop) */
  bool any_bytes_read;  /* true once >=1 response byte has been read for the
                         * CURRENT attempt; gates the reused-connection
                         * dead-connection retry, exactly like Tier 1's
                         * identically-named any_bytes_read output param */
  size_t interim_responses_seen; /* how many CONSECUTIVE interim (1xx)
                                  * responses have been discarded since the
                                  * most recent non-discarded message
                                  * boundary; must persist across dispatch
                                  * callback invocations (the discard loop in
                                  * _async_on_readable_impl/_async_awaiting_
                                  * continue_on_data can return early,
                                  * waiting for more bytes via a later
                                  * on_readable dispatch, mid-way through
                                  * skipping one), unlike Tier 1's identical
                                  * cap, which lives as a plain stack-local
                                  * loop counter in _chttp_read_message_loop
                                  * since that function blocks synchronously
                                  * for the whole loop instead. See
                                  * CHTTP_MAX_INTERIM_RESPONSES's own
                                  * comment for why this is capped at all:
                                  * without it, a server that never stops
                                  * sending e.g. "103 Early Hints" could keep
                                  * this ctx (and the chain/future waiting on
                                  * it) alive indefinitely. Reset to 0 on
                                  * every fresh attempt (a new connect, or a
                                  * retry via _async_retry_hop); a reused
                                  * connection popped from the idle pool also
                                  * starts at 0, since _async_idle_pool_
                                  * offer's reset block zeroes every
                                  * per-hop-attempt field before pooling.
                                  * ALSO reset to 0 the moment a genuine "100
                                  * Continue" is claimed in _async_awaiting_
                                  * continue_on_data, right before handing
                                  * off to the final-response read phase (see
                                  * that call site's own comment): a
                                  * confirmed "100 Continue" is itself a
                                  * real, non-discarded message, so the
                                  * "consecutive discarded" run this field
                                  * tracks genuinely restarts there, exactly
                                  * mirroring how Tier 1's _chttp_send_and_
                                  * read hands the continue-wait and the
                                  * final-response read to two SEPARATE
                                  * _chttp_read_message_loop calls, each with
                                  * its own independent counter. Without this
                                  * reset, a hop that discarded N interim
                                  * responses while waiting for "100
                                  * Continue" only had 64-N (not a fresh 64)
                                  * left over for the final response's own
                                  * interim responses, silently violating
                                  * this cap's own documented "64 CONSECUTIVE
                                  * discarded interim responses" contract
                                  * (README.md/chttpclient_do_async.3): a
                                  * confirmed "100 Continue" breaks the
                                  * consecutive run, so it must not still
                                  * count against whatever comes after it. */
  struct timespec last_used;     /* set when offered to the idle pool; used by
                                  * _async_idle_pool_take's staleness check */
  char *wire; /* owned serialized request bytes; this module always retains
               * ownership and frees it once fully written (tracked via
               * wire_sent below), for both the plain and TLS path alike
               * (there is no queue to hand ownership off to instead).
               * Freeing it in _async_ctx_free is always safe (a no-op once
               * NULL). */
  size_t wire_len;
  size_t wire_sent; /* bytes of wire already written; ctls_conn_write/raw
                     * write(2) both have ordinary short-write semantics. */

  bool is_https;
  bool verify_host;
  _Atomic bool
      hop_completed; /* set once this hop's response has been
                      * fully parsed and either fulfilled or handed off to
                      * a redirect; guards the dispatch callbacks against
                      * re-running that (for _async_handle_redirect,
                      * non-idempotent) logic on a spurious extra
                      * readable/writable dispatch; see that check's own
                      * comment for why this can happen. Also used,
                      * together with reused/any_bytes_read/timed_out, by
                      * _async_ctx_finish (see the "ASYNC CONNECTION STATE
                      * MACHINE" section's own file-level comment) to
                      * decide, at every single connection-ending point,
                      * whether that ending must still fulfil the chain's
                      * future, retry, or neither (already handled by
                      * whichever code path called _async_ctx_finish in
                      * the first place). _Atomic (unlike a first version
                      * of this field, which assumed event_loop's own
                      * dispatch_lock was sufficient protection) because
                      * it is also written directly by _async_submit_hop's
                      * reused-connection path (ordinary application
                      * code running on whatever thread called
                      * chttpclient_do_async, not a dispatch callback, and
                      * therefore NOT covered by event_loop's dispatch_lock
                      * at all) while a concurrent dispatch for this
                      * same, already-registered ctx can legitimately be
                      * in flight at the same time. A real, TSan-caught
                      * data race, found chasing down an intermittent
                      * async_idle_pool.dead_connection_detected_and_
                      * retried failure. See pending_app_teardown's own
                      * field comment for why setting this field is never,
                      * by itself, allowed to be paired with an application
                      * thread also calling _async_ctx_teardown directly. */
  _Atomic bool
      pending_app_teardown; /* Set (always together with hop_completed,
                             * and always before shutdown(ctx->fd, ...) is
                             * called) by the two application-thread
                             * "reused connection could not be
                             * (re)activated" failure paths in
                             * _async_submit_hop/_async_submit_hop_fail,
                             * which must NEVER call _async_ctx_teardown
                             * themselves: ctx's event_loop registration is
                             * still fully live at that point (it was just
                             * popped from the idle pool for reuse), so a
                             * reactor dispatch callback can legitimately
                             * be in flight, or about to be invoked, on a
                             * different thread at the same moment; and
                             * that thread can be preempted by the OS for
                             * an unbounded duration between passing
                             * event_loop's own liveness check and ever
                             * touching ctx. An application thread
                             * physically freeing ctx while that could
                             * still happen is a real, ASan-reproduced
                             * use-after-free; a per-ctx atomic refcount
                             * pinned by every dispatch callback (an
                             * earlier version of this fix) does not
                             * close it either, since the pin's own very
                             * first read of the refcount can itself race
                             * a concurrent free the same way. The only
                             * fix that actually closes this by
                             * construction: no application thread ever
                             * calls the real, destructive teardown for a
                             * registered ctx at all. Instead, these two
                             * failure sites mark ctx terminal, shut its
                             * fd down (forcing a genuine EPOLLIN/EPOLLERR
                             * dispatch on this still-read-registered fd),
                             * and return without touching ctx again;
                             * whichever dispatch callback discovers
                             * hop_completed already true (see
                             * _async_ctx_handle_if_abandoned, called from
                             * all five such checks across
                             * _async_on_readable_impl/_on_writable_impl/
                             * _on_error_impl) checks this flag to tell
                             * "an earlier dispatch invocation already
                             * fully handled this" (false; nothing further
                             * to do) apart from "an application thread
                             * deferred the real teardown to whichever
                             * dispatch notices" (true; run
                             * _async_ctx_teardown for real, now safely
                             * from dispatch context, which is inherently
                             * serialised against every other dispatch for
                             * this exact registration by event_loop's own
                             * entry->dispatch_lock and
                             * entry->refcount-gated "one job in flight
                             * per entry" invariant). Write-once per ctx,
                             * unlike hop_completed: a ctx this flag is
                             * ever set true for is always destined for
                             * termination and never successfully re-
                             * pooled/reused afterward, so there is no
                             * later point at which it would need
                             * resetting back to false the way
                             * hop_completed is reset in
                             * _async_idle_pool_offer. */
  ctls_conn_t *tls;         /* NULL until the connect succeeds and the handshake
                             * begins; NULL for plain HTTP. No separate lock guards
                             * this: see the file-level comment on why event_loop's
                             * own per-registration dispatch_lock already makes one
                             * unnecessary. */

  mutex_t idle_lock; /* Guards the state/chain pair specifically across the
                      * idle<->active transition. A connection popped out of
                      * cli->idle_pools_async by _async_idle_pool_take stays
                      * fully attached to the reactor the whole time (there
                      * is no way to "pause" polling for a single fd);
                      * removing it from the POOL's bookkeeping does nothing
                      * to stop the reactor from independently dispatching
                      * on_readable/on_writable/on_error for it on another
                      * thread the moment the peer sends something (or the
                      * connection dies) while _async_submit_hop is still in
                      * the middle of reconfiguring ctx->state away from
                      * CHTTP_ASYNC_IDLE and ctx->chain away from NULL for
                      * its new owner. Without this lock, a dispatch's
                      * IDLE-state guard could read a stale (already-popped
                      * but not-yet-reconfigured) ctx->state, fall through
                      * past the guard, and dereference ctx->chain while it
                      * is still NULL. Only ever held very briefly, around
                      * the handful of statements that flip state+chain in
                      * either direction, never across I/O. Deliberately NOT
                      * used by the deadline sweep (see the "ASYNC DEADLINE
                      * SWEEP" section for the lock-ordering hazard this
                      * would otherwise create, and the shutdown(fd)-based
                      * design that avoids it entirely): state/chain/fd/
                      * timed_out are instead _Atomic specifically so the
                      * sweep can read them lock-free; see those fields' own
                      * comments for why individual atomic reads (rather
                      * than this lock's stronger compound guarantee) are
                      * sufficient for the sweep's specific use, even though
                      * every OTHER consumer of state/chain (the dispatch
                      * callbacks, via _async_dispatch_kind) still needs (and
                      * keeps getting) the full compound protection this
                      * lock alone provides. */

  chttp_deadline_t connect_deadline; /* Only meaningful while state is
                                      * CHTTP_ASYNC_CONNECTING or
                                      * CHTTP_ASYNC_TLS_HANDSHAKING; set
                                      * fresh (by the thread about to submit
                                      * this hop attempt) at the start of
                                      * every hop that actually connects
                                      * (never for a reused connection, which
                                      * skips both those states entirely).
                                      * Deliberately NOT _Atomic: it is
                                      * written exactly once, strictly
                                      * before _client_deadline_register is
                                      * called for this attempt; that
                                      * call's own mutex lock/unlock is
                                      * itself a release/acquire pair, so it
                                      * already guarantees the sweep (which
                                      * only ever observes a ctx AFTER
                                      * finding it via the registry) sees a
                                      * fully-initialised value with no
                                      * separate synchronisation needed. See
                                      * the "ASYNC DEADLINE SWEEP" section. */
  mutex_t deadline_lock; /* Guards overall_deadline below ONLY; a small,
                          * dedicated leaf lock, never held while trying to
                          * acquire idle_lock or a deadline stripe's mutex
                          * (so it introduces no new lock-ordering cycle with
                          * either), taken briefly by both the writer
                          * (_async_idle_pool_take/_async_submit_hop/
                          * _async_retry_hop, all of which may already be
                          * holding idle_lock at the point they need to
                          * write overall_deadline) and the reader (the
                          * deadline sweep, which already holds this ctx's
                          * own stripe's mutex for that stripe's walk). A
                          * first version of overall_deadline
                          * relied on ctx->chain's own _Atomic-ness as a
                          * publication barrier (matching connect_deadline's
                          * established pattern below) instead of a real
                          * lock; that reasoning turned out to be
                          * insufficient in practice for THIS field
                          * specifically (unlike connect_deadline, which is
                          * published exactly once via _client_deadline_
                          * register's own mutex, overall_deadline can be
                          * REWRITTEN on every idle-pool reuse cycle without
                          * a fresh _client_deadline_register call at all),
                          * caught as a real, reproducible TSan data race
                          * even after the ordering fix. */
  chttp_deadline_t overall_deadline; /* A ctx-local COPY of chain->
                                      * overall_deadline (immutable for the
                                      * chain's whole lifetime, computed once
                                      * in _async_chain_create), updated
                                      * (under deadline_lock) everywhere
                                      * ctx->chain is assigned to a live
                                      * chain (_async_submit_hop's fresh and
                                      * reused paths, _async_idle_pool_take,
                                      * _async_retry_hop). This copy exists
                                      * so the deadline sweep never needs to
                                      * dereference ctx->chain at all: an
                                      * earlier version read chain->
                                      * overall_deadline directly through
                                      * node->chain, a genuine TSan-caught
                                      * use-after-free, since nothing
                                      * prevented the chain's last reference
                                      * from being released (and the chain
                                      * freed) between the sweep's atomic
                                      * read of the pointer and its
                                      * subsequent dereference of it. The
                                      * sweep now only ever compares
                                      * node->chain against NULL directly
                                      * (a safe, non-dereferencing pointer
                                      * read, needing no lock) to decide
                                      * whether this field is meaningful
                                      * right now, and reads this field
                                      * itself under deadline_lock. */
  _Atomic bool timed_out;   /* Set by the deadline sweep, lock-free, right
                             * before it shuts this connection's fd down
                             * (see the "ASYNC DEADLINE SWEEP" section); lets
                             * the normal dispatch-driven teardown
                             * (_async_ctx_finish) report ccol_timed_out and
                             * skip the ordinary dead-connection retry
                             * (retrying past an already-blown deadline
                             * would only extend the overrun for no
                             * benefit). _Atomic for the same reason
                             * state/chain/fd are, above. */
  bool deadline_registered; /* True once this ctx has been linked into its
                             * stripe's list (client_deadline_bundle.
                             * stripes[_deadline_stripe_index_for_ctx(ctx)])
                             * at least once. Registration is idempotent
                             * and, once made, persists for ctx's whole
                             * lifetime (including every idle-pool cycle);
                             * see _client_deadline_register's own comment. */
  struct chttp_async_ctx_s *deadline_prev,
      *deadline_next; /* Intrusive
                       * doubly-linked membership in this ctx's own stripe of
                       * the process-wide, lock-sharded deadline registry,
                       * guarded by that one stripe's mutex (a different lock
                       * than idle_lock above; see client_deadline_bundle's
                       * own comment for the lock-ordering contract between
                       * the two, which applies identically to every
                       * individual stripe's mutex). */

  chttp1_parser_t parser;
  chttp_parse_ctx_t pctx;
  chttp_bodybuf_t bb;

  /* Expect: 100-continue (Tier 2/3). Mirrors Tier 1's own chttp_do_
   * internal/_chttp_send_and_read design (see that function's own doc
   * comment for the three-outcome contract: interim 100 seen -> send body;
   * server answers directly -> that IS the final response, body never
   * sent; wait times out -> send body anyway), adapted to this module's
   * event-driven dispatch instead of a blocking wait loop. */
  bool want_100_continue; /* Computed once per hop attempt in
                           * _async_submit_hop/_async_retry_hop, mirroring
                           * Tier 1's identical use_100_continue local
                           * exactly (same condition: req->expect_continue
                           * && !has_explicit_expect && body_carrying_method
                           * && body.data && body.len > 0). Only ctx->reg's
                           * own dispatch_lock-serialised callbacks and the
                           * hop-setup code that runs strictly before this
                           * ctx is ever registered touch this field, so it
                           * needs no atomic/lock protection of its own. */
  size_t header_len;      /* Only meaningful when want_100_continue: the
                           * boundary within `wire` between the header
                           * block and the body (wire_len - body_len, i.e.
                           * exactly Tier 1's own header_len local). The
                           * first write phase stops here instead of at
                           * wire_len; see _async_plain_try_write/_tls_
                           * try_write's own want_100_continue branch. */
  _Atomic bool
      continue_decided; /* Guards the read-sees-100-vs-write-sees-timeout
                         * race the exact same way hop_completed guards
                         * the analogous "which dispatch gets to finish
                         * this hop" race elsewhere in this file: at most
                         * one of _async_on_readable_impl's "100 Continue
                         * seen" branch and _async_on_writable_impl's
                         * "continue_deadline expired" branch may actually
                         * decide to resume writing the body; whichever
                         * dispatch's callback runs first (event_loop's own
                         * per-entry dispatch_lock guarantees the two can
                         * never run concurrently with each other, since
                         * both directions of one fd share one entry) sets
                         * this via atomic_exchange and only proceeds if it
                         * observed the transition from false; the other,
                         * if it also fires, sees it already true and
                         * no-ops. Reset to false at the start of every
                         * fresh want_100_continue hop attempt, exactly
                         * like interim_responses_seen. */
  chttp_deadline_t
      continue_deadline; /* Bounded by both CHTTP_100_CONTINUE_WAIT_MS and
                          * whatever remains of ctx->overall_deadline (via
                          * _deadline_earlier), computed once when this hop
                          * attempt transitions into CHTTP_ASYNC_AWAITING_
                          * CONTINUE. Consulted only by the deadline sweep,
                          * only while ctx->state == CHTTP_ASYNC_AWAITING_
                          * CONTINUE; needs no lock of its own for the
                          * identical reason connect_deadline needs none
                          * (see that field's own comment): set once per
                          * hop attempt, never rewritten while a concurrent
                          * reader could be consulting it. */
  bool retry_unsafe;     /* Mirrors Tier 1's identical *retry_unsafe_out
                          * parameter to _chttp_send_and_read: set true the
                          * moment this hop sends the body onto the wire AFTER
                          * having received an explicit "100 Continue" from the
                          * server on THIS connection, at which point a
                          * subsequent read failure with zero final-response
                          * bytes is no longer safe to interpret as "nothing
                          * was ever sent, retry is free" (see that function's
                          * own doc comment for the full non-idempotent-
                          * double-submission rationale). Checked by
                          * _async_ctx_finish alongside reused/any_bytes_read;
                          * reset to false at the start of every fresh hop
                          * attempt. */
  char *continue_carry;  /* Non-NULL only when a "100 Continue" line
                          * arrived in the same read() as the start of
                          * the real final response (a fast/optimistic
                          * server writing both before it has even
                          * received the body; RFC 7231 SS5.1.1
                          * license this - see Tier 1's identical
                          * carry-forward in _chttp_send_and_read for
                          * the blocking-design equivalent). Owned copy
                          * of those trailing bytes, taken because the
                          * body write that must happen first may not
                          * complete synchronously (EWOULDBLOCK
                          * partway through a large body), so the
                          * bytes cannot simply be re-parsed inline:
                          * they are stashed here and fed through
                          * _async_process_reading_data the moment the
                          * body write actually finishes (in
                          * _async_plain_try_write/_tls_try_write's
                          * shared completion path), whichever
                          * dispatch that turns out to be on. Freed
                          * and NULLed there; also freed at ctx
                          * teardown for the abandoned-mid-flight
                          * case. */
  size_t continue_carry_len;
} chttp_async_ctx_t;

/* ========================================================================== */
/*                    ASYNC DEADLINE SWEEP (TIER 2)                           */
/* ========================================================================== */

/*
 * Enforces connect_timeout_ms/request_timeout_ms for Tier 2/3 as true
 * absolute wall-clock deadlines, mirroring Tier 1's _deadline_make/
 * _deadline_remaining_ms semantics exactly; including catching a
 * connection that never goes fully idle (e.g. a slow trickle of bytes just
 * before an activity-reset timeout would fire) but has still blown its
 * deadline. event_loop has no built-in timer/deadline mechanism at all, so
 * this is a small periodic-sweep thread, entirely separate from the
 * reactor's own threads and the DNS/connect ctpool; started/stopped
 * alongside them (see _client_deadline_sweep_start/_stop_and_join, called
 * from _client_engine_acquire/_client_engine_reaper_fn above).
 *
 * The registry of live async ctx nodes is sharded across
 * CHTTP_DEADLINE_SWEEP_STRIPES independent stripes (client_deadline_bundle.
 * stripes[]), each with its own mutex and its own intrusive doubly-linked
 * list head, rather than one global mutex guarding one global list. A
 * registration/unregistration only ever needs to lock the one stripe its
 * ctx hashes into (_deadline_stripe_index_for_ctx), so concurrent
 * registrations for different ctx nodes across many in-flight requests no
 * longer serialise against each other, and the sweep's own per-stripe walk
 * (below) only ever blocks a registration/unregistration landing in that
 * one stripe, not every registration/unregistration in the process. A
 * single request's own register/unregister pair is still, by construction,
 * always on the very same stripe (the hash is a pure, stateless function
 * of the ctx pointer, recomputed identically on both calls, so no extra
 * per-ctx bookkeeping field is needed to remember which stripe a given ctx
 * landed in). Mirrors event_loop's own lock-striped fd registry in
 * cthreadcomm.c (_stripe_index_for_fd) for exactly the same reason: a real
 * ctx-hash key needs the same stripe on every lookup, unlike event_loop's
 * queue-selectable stripe assignment (round-robin), which is safe only
 * because a queue selectable is never looked up a second time.
 *
 * client_deadline_bundle.mutex/cond_var, separately, guard only the sweep
 * thread's own start/stop/sleep-wake lifecycle (client_deadline_bundle.
 * stop/thread); they play no part in registry mutual exclusion at all any
 * more. Each stripe's own mutex now serialises, for that one stripe: (1)
 * its intrusive doubly-linked list itself (ctx->deadline_prev/
 * deadline_next, for every ctx hashing into it), and (2) the reason a ctx
 * is never explicitly unregistered except from within _async_ctx_free, the
 * single reliable point ctx memory is actually released: mutual exclusion
 * between the sweep reading a registered ctx's fields and any other thread
 * freeing that same ctx concurrently. _client_deadline_unregister cannot
 * complete (and therefore _async_ctx_free cannot proceed to actually free
 * ctx) while the sweep is still walking that ctx's own stripe with that
 * stripe's mutex held; a sweep tick's walk of a DIFFERENT stripe is
 * entirely unaffected and proceeds concurrently.
 *
 * Registration (_client_deadline_register) is idempotent and, once made,
 * persists for a ctx's entire lifetime, including every idle-pool cycle it
 * goes through; a pooled/idle ctx just sits in its stripe inertly (the
 * sweep's own state==CONNECTING/TLS_HANDSHAKING and chain!=NULL checks
 * naturally skip it while idle), which is simpler and just as correct as
 * explicitly unregistering on every idle-pool-offer and re-registering on
 * every reuse.
 *
 * Lock ordering: _client_deadline_register CAN legitimately be called while
 * some ctx's idle_lock is held (e.g. _async_submit_hop's reused-connection
 * event_loop_modify-failure branch holds the abandoned ctx's own idle_lock
 * across its call to _async_retry_hop, which registers the brand-new retry
 * ctx before releasing it), so a stripe's mutex is NOT always taken outside
 * of every ctx->idle_lock. This is still deadlock-free only because the
 * reverse acquisition never happens anywhere: neither
 * _client_deadline_register nor _client_deadline_unregister ever touches
 * any ctx's idle_lock while holding a stripe's mutex (both only ever
 * manipulate that one stripe's own intrusive list), and the sweep
 * (_client_deadline_sweep_once, every stripe mutex's only other consumer)
 * never acquires idle_lock at all, reading every field it needs via plain
 * _Atomic loads instead (see that function's own comment). Preserving THAT
 * one-directional invariant - no stripe's mutex may ever be held while
 * acquiring any ctx->idle_lock - is what actually matters here; do not
 * "fix" the sweep or the register/unregister functions to also take
 * idle_lock without re-deriving this reasoning first, or the
 * idle_lock-then-stripe-mutex ordering already in real use above becomes a
 * genuine AB-BA lock-order inversion. No two stripe mutexes are ever held
 * at once anywhere in this file either (each operation touches exactly one
 * ctx, hence exactly one stripe, and the sweep locks/walks/unlocks one
 * stripe fully before moving to the next), so sharding introduces no new
 * inter-stripe lock-ordering surface to reason about beyond the
 * stripe-vs-idle_lock rule above.
 *
 * Neutering an expired connection: a raw fd carries no protection of its own
 * against being closed and reused by an unrelated connection between the
 * moment this sweep captures it and the moment it acts on it. Calling any
 * fd-based operation on a captured int after releasing a lock therefore
 * risks acting on a completely unrelated connection if that fd number was
 * closed and reused by a fresh connect() in the meantime. This sweep avoids
 * that hazard by construction rather than needing a generation check of its
 * own: every teardown path (_async_ctx_finish/_async_idle_ctx_finish,
 * defined after the idle pool section below) calls
 * _client_deadline_unregister (which requires that ctx's own stripe mutex)
 * strictly BEFORE closing ctx->fd, so any ctx still linked into its stripe
 * is guaranteed to not have had its fd closed yet. This sweep therefore
 * calls shutdown(fd, SHUT_RDWR) directly, still holding that ctx's own
 * stripe mutex, with no separate collect-then-act-outside-the-lock phase
 * needed at all: shutdown() is a plain kernel-level operation with no
 * synchronous application callback, so there is no reentrancy risk in
 * calling it here. shutdown() neuters both directions of the connection
 * (readable-with-error and writable-with-error on the next epoll_wait)
 * without this sweep ever touching ctx's own memory, idle_lock, or its
 * event_loop registration; the normal dispatch path
 * (_async_on_readable/_on_writable/_on_error) takes it from there once the
 * reactor observes it, exactly like any other organic connection failure.
 */
#define CHTTP_DEADLINE_SWEEP_STRIPES 16

typedef struct {
  mutex_t mutex;
  chttp_async_ctx_t *head;
} chttp_deadline_stripe_t;

struct {
  mutex_t mutex;
  cond_var_t cond_var;
  /* client_deadline_bundle.mutex/_cv guard only the sweep thread's own
   * start/stop/sleep-wake lifecycle (stop, thread), never the ctx registry
   * itself; the registry's own locking is entirely per-stripe (see
   * stripes[] below). client_deadline_bundle.mutex/_cv used to carry static
   * PTHREAD_MUTEX_INITIALIZER/PTHREAD_COND_INITIALIZER initializers; converted
   * to lazy, call_once-guarded runtime init (see
   * _client_deadline_init_globals), independent of the async engine pair's own
   * once-guard above, for the same reason: a non-pthread backend's equivalent
   * primitive may need real setup work a compile-time constant can't provide.
   * Every function below that touches any of these globals calls
   * call_once(client_deadline_bundle.once, ...) as its first statement. */
  once_flag_t once;
  chttp_deadline_stripe_t stripes[CHTTP_DEADLINE_SWEEP_STRIPES];
  thread_id_t thread;
  bool stop;
} client_deadline_bundle = {0};

static size_t _deadline_stripe_index_for_ctx(const chttp_async_ctx_t *ctx) {
  uintptr_t p = (uintptr_t)ctx;
  /* Fold the full pointer width down to 32 bits (XOR the high and low
   * halves) before applying the multiplicative hash, rather than just
   * truncating to the low 32 bits: a heap pointer's low bits are often
   * alignment-constrained (e.g. always a multiple of 16), so using only
   * them would waste entropy the high bits actually carry. Same Knuth
   * multiplicative constant _stripe_index_for_fd uses in cthreadcomm.c.
   *
   * The `p >> 32` fold only makes sense (and only compiles) on a platform
   * where uintptr_t is wider than 32 bits: on a 32-bit target (e.g. i386),
   * uintptr_t IS the 32-bit value already, so there is no "high half" left
   * to XOR in, and `p >> 32` is a shift by the full width of the type,
   * undefined behavior and a hard -Werror=shift-count-overflow error under
   * this project's build flags. Guarded with a preprocessor #if (not a
   * runtime if), mirroring chashmap.c's own `#if SIZE_MAX == UINT64_MAX`
   * convention for the identical reason: a runtime check does not stop the
   * compiler from still fully type-checking (and rejecting) the
   * dead-on-this-platform branch. */
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
  if (mutex_init(client_deadline_bundle.mutex) != 0)
    fatal_err("chttpclient deadline sweep: failed to initialize mutex");
  for (size_t i = 0; i < CHTTP_DEADLINE_SWEEP_STRIPES; i++) {
    if (mutex_init(client_deadline_bundle.stripes[i].mutex) != 0)
      fatal_err(
          "chttpclient deadline sweep: failed to initialize stripe mutex");
  }
  /* CLOCK_MONOTONIC to match _client_deadline_sweep_fn's own
   * clock_gettime(CLOCK_MONOTONIC, ...)-based wake deadline;
   * cond_var_init's default clock (CLOCK_REALTIME) would make that
   * deadline comparison meaningless (a monotonic-clock timespec, always a
   * small value relative to boot time, compared against a condvar
   * internally using the wall clock), so cond_var_timedwait would report
   * ETIMEDOUT immediately on every call instead of actually sleeping ~100ms
   * between sweeps, busy-spinning the sweep thread at 100% CPU for the
   * entire life of the engine. See chttpserver.c's srv->requests_done_cv
   * for the exact same fix, applied there first; see _cond_var_init_monotonic
   * (this file's shared helper for this exact pattern) for the full
   * rationale. */
  if (_cond_var_init_monotonic(&client_deadline_bundle.cond_var) != 0)
    fatal_err(
        "chttpclient deadline sweep: failed to initialize condition "
        "variable");
}

#define CHTTP_DEADLINE_SWEEP_INTERVAL_MS 100
/* Soft cap on how many expired connections one sweep tick shuts down.
 * Self-healing, not a hard limit: anything beyond this on one tick simply
 * stays registered (not yet marked timed_out, so it is picked up again) and
 * gets its shutdown() call from a later tick instead, at worst another
 * CHTTP_DEADLINE_SWEEP_INTERVAL_MS later per extra batch; a practically
 * irrelevant delay for the number of simultaneously-expiring connections it
 * would take to ever exceed this. */
#define CHTTP_DEADLINE_SWEEP_BATCH 256

/*
 * Adds ctx to the deadline registry if not already present. Must be called
 * with ctx->idle_lock NOT held (see the lock-ordering note above). Only
 * ever locks the one stripe ctx hashes into, never any other stripe and
 * never client_deadline_bundle.mutex itself (that mutex guards the sweep
 * thread's own lifecycle only, not the registry).
 */
static void _client_deadline_register(chttp_async_ctx_t *ctx) {
  call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  chttp_deadline_stripe_t *stripe =
      &client_deadline_bundle.stripes[_deadline_stripe_index_for_ctx(ctx)];
  mutex_lock(stripe->mutex);
  if (!ctx->deadline_registered) {
    ctx->deadline_prev = NULL;
    ctx->deadline_next = stripe->head;
    if (stripe->head) stripe->head->deadline_prev = ctx;
    stripe->head = ctx;
    ctx->deadline_registered = true;
  }
  mutex_unlock(stripe->mutex);
}

/* Removes ctx from the deadline registry if present. Called exactly once,
 * as the first thing _async_ctx_free does; see that function's own
 * comment for why that is the single correct place for this.
 * _deadline_stripe_index_for_ctx is a pure, stateless function of ctx's own
 * address, so this always recomputes and locks the identical stripe
 * _client_deadline_register used for this same ctx - no separate
 * "which stripe was this in" bookkeeping is needed. */
static void _client_deadline_unregister(chttp_async_ctx_t *ctx) {
  call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  chttp_deadline_stripe_t *stripe =
      &client_deadline_bundle.stripes[_deadline_stripe_index_for_ctx(ctx)];
  mutex_lock(stripe->mutex);
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
  mutex_unlock(stripe->mutex);
}

/*
 * One sweep pass: walks each of the CHTTP_DEADLINE_SWEEP_STRIPES stripes in
 * turn, locking only that one stripe's own mutex for the duration of its
 * own walk (never more than one stripe lock held at a time, and never
 * client_deadline_bundle.mutex itself, which this function never touches),
 * checking each ctx's connect_deadline (only while it is actually in a
 * connecting phase) and its chain's overall_deadline (whenever it has a
 * live chain at all), and shuts down the fd of any newly-expired connection
 * directly, inline, still holding that stripe's lock (see the section's own
 * file-level comment for why this is safe: no reentrancy risk, and no
 * fd-reuse race). n_shutdown accumulates across the whole tick (every
 * stripe), so CHTTP_DEADLINE_SWEEP_BATCH still bounds one sweep tick's total
 * work the same way it did under the single-list design, not per-stripe.
 *
 * state/chain/fd/timed_out are read as plain (but _Atomic-qualified, so
 * individually race-free) loads here; deliberately NOT under the ctx's own
 * idle_lock (see the lock-ordering note above for why: some call paths
 * legitimately hold a ctx's idle_lock while also needing to lock a deadline
 * stripe, e.g. _async_submit_hop's reused-connection-write-failure branch
 * calling _async_retry_hop -> _client_deadline_register; this sweep must
 * never acquire idle_lock itself while holding a stripe's mutex, or that
 * becomes a classic AB-BA lock-order inversion, for that stripe exactly as
 * it would have for the single global mutex before sharding).
 *
 * This is safe without idle_lock's stronger *compound* (state-and-chain-
 * together) consistency guarantee specifically because of what this
 * function does with a possibly-torn snapshot: at both points idle_lock
 * actually protects (_async_idle_pool_take's idle->active transition and
 * _async_idle_pool_offer's active->idle transition), every reachable torn
 * combination of (state, chain) either skips both deadline checks (chain
 * NULL, or state not a connecting state) or evaluates a chain that is,
 * worst case, a moment away from being released but still guaranteed alive
 * (the chain reference is only actually dropped strictly after the
 * state/chain pair is updated); so the worst possible outcome of a torn
 * read here is shutting down a connection a few instructions before or
 * after it would otherwise have been cleanly pooled or hopped, i.e. a lost
 * optimisation opportunity, never an incorrect abort of a still-in-flight,
 * unrelated request. Every OTHER consumer of state/chain (the dispatch
 * callbacks, via _async_dispatch_kind) has a stronger need for idle_lock's
 * full guarantee and keeps using it entirely unchanged.
 *
 * A ctx is marked ctx->timed_out before its fd is shut down, both to tell
 * the normal teardown path "this is a timeout, not an ordinary connection
 * failure" (report ccol_timed_out and skip the usual dead-connection retry;
 * retrying past an already-blown deadline would just extend the overrun)
 * and to make repeated sweep ticks idempotent against a ctx still mid-
 * teardown from an earlier tick's shutdown() call.
 */
static void _client_deadline_sweep_once(void) {
  call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  size_t n_shutdown = 0;

  for (size_t s = 0; s < CHTTP_DEADLINE_SWEEP_STRIPES &&
                     n_shutdown < CHTTP_DEADLINE_SWEEP_BATCH;
       s++) {
    chttp_deadline_stripe_t *stripe = &client_deadline_bundle.stripes[s];
    mutex_lock(stripe->mutex);
    chttp_async_ctx_t *node = stripe->head;
    while (node && n_shutdown < CHTTP_DEADLINE_SWEEP_BATCH) {
      chttp_async_ctx_t *next = node->deadline_next;

      chttp_async_state_t state = node->state;
      /* Only ever compared against NULL (never dereferenced): see
       * node->overall_deadline's own field comment for why dereferencing a
       * bare chain pointer read here was a real, TSan-caught
       * use-after-free. */
      bool has_chain = (node->chain != NULL);
      int fd = node->fd;
      bool expired = false;
      if (!node->timed_out) {
        int ms;
        if ((state == CHTTP_ASYNC_CONNECTING ||
             state == CHTTP_ASYNC_TLS_HANDSHAKING) &&
            !_deadline_remaining_ms(&node->connect_deadline, &ms)) {
          expired = true;
        }
        if (!expired && has_chain) {
          /* See node->deadline_lock's own field comment: this dedicated
           * leaf lock is what makes reading overall_deadline here safe,
           * since it can be rewritten (not just published once) on every
           * idle-pool reuse cycle, unlike connect_deadline. */
          mutex_lock(node->deadline_lock);
          bool still_ok = _deadline_remaining_ms(&node->overall_deadline, &ms);
          mutex_unlock(node->deadline_lock);
          if (!still_ok) expired = true;
        }
        if (expired) node->timed_out = true;
      }

      if (expired) {
        if (fd >= 0) shutdown(fd, SHUT_RDWR);
        n_shutdown++;
      } else if (state == CHTTP_ASYNC_AWAITING_CONTINUE) {
        /* A soft, per-hop-phase deadline distinct from connect_deadline/
         * overall_deadline above: CHTTP_100_CONTINUE_WAIT_MS expiring does
         * NOT mean the request itself timed out (overall_deadline, checked
         * unconditionally above since has_chain is true here too, remains
         * the real backstop for that and is left completely untouched by
         * this branch); it means the wait for a "100 Continue" is over and
         * the body should be sent anyway (see CHTTP_ASYNC_AWAITING_CONTINUE's
         * own doc comment for the full three-outcome contract). Delivered by
         * flipping this registration to write direction, which dispatches
         * to _async_on_writable_impl's own CHTTP_ASYNC_AWAITING_CONTINUE
         * branch to actually act on it under that dispatch's own
         * dispatch_lock, rather than mutating ctx's fields directly from
         * this sweep thread.
         *
         * continue_decided is only peeked (not claimed) here: the real
         * claim happens inside the dispatch this triggers, exactly like
         * every other exit from _async_awaiting_continue_on_data. Already
         * true here means a genuine "100 Continue"/final response was
         * concurrently claimed by an in-flight on_readable dispatch for
         * this same ctx; skipping the modify call in that case avoids a
         * harmless but wasted spurious write-direction flip racing that
         * dispatch's own, already correct, transition. A residual,
         * extremely narrow window remains where this peek and the modify
         * call below could still race a same-instant on_readable claim;
         * the worst outcome is a transiently wrong epoll direction for
         * this one connection, which self-heals via the very next
         * event_loop_modify call for the same registration (always
         * recomputed from live state, never a stale snapshot) or, at
         * worst, via overall_deadline's own unconditional backstop above -
         * never a permanent hang, a crash, or any effect on any OTHER
         * request. */
        int cms;
        if (!_deadline_remaining_ms(&node->continue_deadline, &cms) &&
            !atomic_load(&node->continue_decided)) {
          event_reg reg = atomic_load(&node->reg);
          if (reg) {
            event_loop_modify(cli_engine_bundler.reactor, reg,
                              ccol_select_write);
          }
        }
      }
      node = next;
    }
    mutex_unlock(stripe->mutex);
  }

  if (n_shutdown > 0) {
    _CLIENT_ENGINE_LOG_INFO("deadline sweep shut down %zu connection(s)",
                            n_shutdown);
  }
}

static void *_client_deadline_sweep_fn(void *arg) {
  (void)arg;
  call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  mutex_lock(client_deadline_bundle.mutex);
  while (!client_deadline_bundle.stop) {
    struct timespec wake;
    clock_gettime(CLOCK_MONOTONIC, &wake);
    wake.tv_nsec += CHTTP_DEADLINE_SWEEP_INTERVAL_MS * 1000000L;
    if (wake.tv_nsec >= 1000000000L) {
      wake.tv_nsec -= 1000000000L;
      wake.tv_sec += 1;
    }
    cond_var_timedwait(client_deadline_bundle.cond_var,
                       client_deadline_bundle.mutex, wake);
    if (client_deadline_bundle.stop) break;
    mutex_unlock(client_deadline_bundle.mutex);
    _client_deadline_sweep_once();
    mutex_lock(client_deadline_bundle.mutex);
  }
  mutex_unlock(client_deadline_bundle.mutex);
  return NULL;
}

/* Starts the sweep thread. Called from _client_engine_acquire, alongside
 * spawning the engine's own reactor thread; see that function for
 * rollback-on-failure handling. */
static ccol_retval_t _client_deadline_sweep_start(void) {
  client_deadline_bundle.stop = false;
  int rc = thread_create(client_deadline_bundle.thread,
                         _client_deadline_sweep_fn, NULL);
  return (rc == 0) ? ccol_success : ccol_unexpected_failure;
}

/* Signals and joins the sweep thread. Called from _client_engine_reaper_fn,
 * alongside joining the engine's own reactor thread; see that function's
 * own comment for why teardown always happens on a dedicated reaper thread,
 * never inline from a reactor callback. */
static void _client_deadline_sweep_stop_and_join(void) {
  call_once(client_deadline_bundle.once, _client_deadline_init_globals);
  mutex_lock(client_deadline_bundle.mutex);
  client_deadline_bundle.stop = true;
  cond_var_broadcast(client_deadline_bundle.cond_var);
  mutex_unlock(client_deadline_bundle.mutex);
  thread_join(client_deadline_bundle.thread);
}

/* Deep-copies a chmap(char* -> char*) header map; used to give a redirect
 * chain its own copy of the original request's headers, since the caller's
 * chttp_request_t may be freed the moment chttpclient_do_async returns, long
 * before a later hop needs to re-serialise them. Returns NULL on OOM (input
 * NULL is not an error; it just means "no headers", and returns NULL too,
 * which is indistinguishable from an OOM failure taken alone; callers that
 * need to tell the two apart check the source map first, exactly like every
 * other call site in this file that treats "no headers" as normal). */
static chmap _clone_headers_map(ccol_memmgmt_procs_t *mp, chmap src) {
  char *err = NULL;
  chmap dst = chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                                ccol_string, mp, NULL, &err);
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
 * Allocates the shared, whole-redirect-chain state (see the file-level
 * comment above chttp_async_chain_t). Takes ownership of tls_ctx on success
 * (released once via ctls_ctx_release when the chain is freed); on failure,
 * the caller still owns tls_ctx and must release it itself. req_headers/
 * body_data/body_content_type are NOT taken by reference: this function
 * deep-copies whatever it needs from them and never retains the originals.
 */
static chttp_async_chain_t *_async_chain_create(
    ccol_memmgmt_procs_t *mp, struct chttpclient *cli, ctpool_future *future,
    chmap req_headers, const void *body_data, size_t body_len,
    const char *body_content_type, const char *initial_origin_key,
    ctls_ctx_t *tls_ctx, bool tls_ctx_usable, bool verify_host,
    long connect_timeout_ms, long request_timeout_ms,
    size_t max_response_body_size, chttpcli_write_fn write_fn, void *write_ctx,
    bool expect_continue) {
  chttp_async_chain_t *chain =
      (chttp_async_chain_t *)_mem_calloc(mp, 1, sizeof(*chain));
  if (!chain) return NULL;
  if (mutex_init(chain->lock) != 0) {
    _mem_free(mp, chain);
    return NULL;
  }
  chain->mp = mp;
  chain->cli = cli;
  chain->future = future;
  chain->tls_ctx = tls_ctx;
  chain->tls_ctx_usable = tls_ctx_usable;
  chain->verify_host = verify_host;
  chain->connect_timeout_ms = connect_timeout_ms;
  chain->overall_deadline = _deadline_make(request_timeout_ms);
  chain->max_response_body_size = max_response_body_size;
  chain->write_fn = write_fn;
  chain->write_ctx = write_ctx;
  chain->expect_continue = expect_continue;

  if (req_headers) {
    chain->req_headers = _clone_headers_map(mp, req_headers);
    if (!chain->req_headers) goto fail;
  }
  if (body_data && body_len > 0) {
    chain->body_data = _mem_alloc(mp, body_len);
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

  /* This chain is now live and about to be handed to _async_submit_hop;
   * count it against cli's own async in-flight total (see that field's own
   * comment on struct chttpclient) so __chttpclient_destroy can wait for it.
   * Matched by exactly one decrement in _async_chain_release, once this
   * chain's refcount reaches zero. */
  mutex_lock(cli->async_count_lock);
  cli->async_in_flight_count++;
  mutex_unlock(cli->async_count_lock);

  return chain;

fail:
  if (chain->req_headers) __chmap_destroy(chain->req_headers);
  _mem_free(mp, chain->body_data);
  _mem_free(mp, chain->body_content_type);
  _mem_free(mp, chain->initial_origin_key);
  mutex_destroy(chain->lock);
  _mem_free(mp, chain);
  return NULL;
}

/* Forward declaration: needed by _async_chain_release's backstop, defined
 * further below once chttp_async_result_t's construction logic is in scope. */
static void _async_fulfill_chain(chttp_async_chain_t *chain, ccol_retval_t rv,
                                 chttpcli_response *resp);

/* Adds one live-hop reference to chain. Must be paired with exactly one
 * _async_chain_release call. Called with the chain already known to be
 * live (either freshly created with refcount 0 -> 1, or retained again by a
 * redirect handoff while the old hop's ctx is still alive). */
static void _async_chain_retain(chttp_async_chain_t *chain) {
  mutex_lock(chain->lock);
  chain->refcount++;
  mutex_unlock(chain->lock);
}

/*
 * Releases one live-hop reference. When the count reaches zero; meaning no
 * further hop was ever queued to take over from the last one torn down;
 * this is the single reliable point to both fulfil the future as a backstop
 * (a no-op if some hop already fulfilled it, via the same fulfilled guard
 * _async_fulfill_chain checks) and free the chain itself, including
 * releasing the one engine reference held for the whole chain's lifetime.
 */
static void _async_chain_release(chttp_async_chain_t *chain) {
  if (!chain) return;
  mutex_lock(chain->lock);
  int remaining = --chain->refcount;
  mutex_unlock(chain->lock);
  if (remaining > 0) return;

  struct chttpclient *cli = chain->cli;

  _async_fulfill_chain(chain, ccol_http_transfer_aborted, NULL);
  if (chain->tls_ctx) ctls_ctx_release(chain->tls_ctx);
  if (chain->req_headers) __chmap_destroy(chain->req_headers);
  _mem_free(chain->mp, chain->body_data);
  _mem_free(chain->mp, chain->body_content_type);
  _mem_free(chain->mp, chain->carried_auth);
  _mem_free(chain->mp, chain->carried_auth_origin);
  _mem_free(chain->mp, chain->initial_origin_key);
  mutex_destroy(chain->lock);
  _mem_free(chain->mp, chain);
  _client_engine_release();

  /* Matches the increment in _async_chain_create; see cli's own
   * async_in_flight_count field comment for why this uses a dedicated leaf
   * lock rather than cli->lock (this function runs from inside event_loop
   * dispatch callbacks as often as from a safe synchronous context). */
  mutex_lock(cli->async_count_lock);
  if (cli->async_in_flight_count > 0) cli->async_in_flight_count--;
  if (cli->async_in_flight_count == 0)
    cond_var_broadcast(cli->async_count_drained);
  mutex_unlock(cli->async_count_lock);
}

static chttp_async_ctx_t *_async_ctx_create(ccol_memmgmt_procs_t *mp) {
  chttp_async_ctx_t *ctx =
      (chttp_async_ctx_t *)_mem_calloc(mp, 1, sizeof(*ctx));
  if (!ctx) return NULL;
  ctx->mp = mp;
  ctx->fd = -1;
  /* pending_app_teardown/hop_completed both correctly default to false via
   * _mem_calloc above; no explicit init needed (unlike the old ctx->refs
   * field this replaced, whose correct default was 1, not calloc's 0). */
  if (mutex_init(ctx->idle_lock) != 0) {
    _mem_free(mp, ctx);
    return NULL;
  }
  if (mutex_init(ctx->deadline_lock) != 0) {
    mutex_destroy(ctx->idle_lock);
    _mem_free(mp, ctx);
    return NULL;
  }
  return ctx;
}

/*
 * The single reliable place ctx (a single hop's connection state) is freed;
 * called from _async_ctx_teardown (via _async_ctx_finish/
 * _async_idle_ctx_finish, the two explicit terminal-teardown helpers below
 * that give this module its own guaranteed-exactly-once teardown point; see
 * the "ASYNC CONNECTION STATE MACHINE" section's file-level comment for why
 * event_loop needs this to be explicit rather than implicit) or, for
 * failures that occur before a connection is ever registered with the
 * reactor, directly by the failing code path. Every field is safe to
 * free/destroy unconditionally: fields whose ownership was transferred
 * elsewhere (headers/bb.buf -> a successfully built response) are set
 * NULL/cleared at the transfer site, and _mem_free/_parse_ctx_free_fields/a
 * NULL tls/reg/fd are all no-ops. Does NOT touch ctx->chain; that is
 * shared, whole-chain state; see _async_ctx_teardown, which pairs this with
 * the matching chain release.
 *
 * event_loop_remove and close() are both called here, unconditionally and
 * idempotently safe, rather than requiring every caller to have already
 * done so: this mirrors ctx->tls/ctx->wire's own "always safe to free here
 * regardless of what the caller already did" treatment. _client_deadline_
 * unregister is called FIRST, strictly before close(ctx->fd): this ordering
 * is load-bearing, not incidental; see the "ASYNC DEADLINE SWEEP"
 * section's own comment for why the deadline sweep's fd-based shutdown()
 * call is only safe from an fd-reuse race because every teardown path
 * unregisters from that registry before its fd can be closed and
 * potentially reused by an unrelated connection.
 */
static void _async_ctx_destroy_now(chttp_async_ctx_t *ctx) {
  /* Brief acquire/release, not held across anything below: guarantees
   * _async_connect_task's own post-event_loop_add window (see its own
   * comment, right after this same idle_lock is taken there) has fully
   * completed before this function proceeds to free ctx out from under it.
   * Every other _async_ctx_free/_async_ctx_teardown call site already
   * releases idle_lock (if it was even held) before calling in here (see
   * the call-site audit in this function's own doc comment above), so this
   * cannot self-deadlock against any existing caller. */
  mutex_lock(ctx->idle_lock);
  mutex_unlock(ctx->idle_lock);
  _client_deadline_unregister(ctx); /* no-op if never registered; MUST run
                                     * before the close() below */
  if (ctx->reg) event_loop_remove(cli_engine_bundler.reactor, ctx->reg);
  if (ctx->tls) ctls_conn_destroy(ctx->tls);
  if (ctx->fd >= 0) close(ctx->fd);
  mutex_destroy(ctx->idle_lock);
  mutex_destroy(ctx->deadline_lock);
  _mem_free(ctx->mp, ctx->wire);
  _mem_free(ctx->mp, ctx->continue_carry);
  _mem_free(ctx->mp, ctx->unix_socket_path);
  _mem_free(ctx->mp, ctx->host);
  _mem_free(ctx->mp, ctx->path_and_query);
  _mem_free(ctx->mp, ctx->origin_key);
  _parse_ctx_free_fields(&ctx->pctx);
  _mem_free(ctx->mp, ctx->bb.buf);
  _mem_free(ctx->mp, ctx);
}

/*
 * The single reliable place ctx is actually, physically freed. Safe to call
 * unconditionally because this module's own invariant (see
 * pending_app_teardown's field comment, and the "no application thread ever
 * frees a registered ctx" rule it exists to uphold) guarantees this function
 * is only ever reached in one of two situations: (1) ctx was never
 * registered with the reactor at all (a pre-connect failure; nothing else
 * could possibly reference it), or (2) this call is running from within a
 * reactor dispatch callback, which event_loop's own entry->dispatch_lock and
 * entry->refcount-gated "at most one job in flight per entry" invariant
 * already guarantee cannot be running concurrently with any other dispatch
 * for this exact registration. Neither case needs, or has, any reference
 * count of its own on ctx.
 */
static void _async_ctx_free(chttp_async_ctx_t *ctx) {
  if (!ctx) return;
  _async_ctx_destroy_now(ctx);
}

/* Frees a single hop's per-connection state and releases its chain
 * reference; the standard way every terminal code path for a ctx (on_close,
 * or an early failure before fio_attach ever ran) ends. Captures chain into
 * a local first since _async_ctx_free frees ctx itself. */
static void _async_ctx_teardown(chttp_async_ctx_t *ctx) {
  chttp_async_chain_t *chain = ctx->chain;
  _async_ctx_free(ctx);
  _async_chain_release(chain);
}

/*
 * Delivers a terminal result to the caller's future exactly once (guarded by
 * chain->fulfilled, under chain->lock; see that field's comment for why
 * this must be lock-protected rather than a plain bool now that a redirect
 * chain can have two hops' callbacks running concurrently); safe to call
 * from multiple exit paths (e.g. an error path followed by
 * _async_chain_release's backstop call) since only the first call has any
 * effect. On the (rare) allocation failure building the result struct
 * itself, still fulfills with NULL rather than leaving the caller's
 * ctpool_future_get blocked forever.
 */
static void _async_fulfill_chain(chttp_async_chain_t *chain, ccol_retval_t rv,
                                 chttpcli_response *resp) {
  mutex_lock(chain->lock);
  bool already = chain->fulfilled;
  chain->fulfilled = true;
  mutex_unlock(chain->lock);
  if (already) {
    if (resp) {
      if (resp->headers) __chmap_destroy(resp->headers);
      _mem_free(chain->mp, resp->body);
      _mem_free(chain->mp, resp);
    }
    return;
  }
  chttpcli_async_result_t *result =
      (chttpcli_async_result_t *)_mem_calloc(chain->mp, 1, sizeof(*result));
  if (!result) {
    ctpool_future_fulfill(chain->future, NULL);
    if (resp) {
      if (resp->headers) __chmap_destroy(resp->headers);
      _mem_free(chain->mp, resp->body);
      _mem_free(chain->mp, resp);
    }
    return;
  }
  result->rv = rv;
  result->resp = resp;
  result->_m_procs = chain->mp;
  ctpool_future_fulfill(chain->future, result);
}

static void _async_fulfill(chttp_async_ctx_t *ctx, ccol_retval_t rv,
                           chttpcli_response *resp) {
  /* Marks this ctx terminal so _async_on_readable/_async_on_writable's own
   * top-of-function guard skips any further work for it; see
   * ctx->hop_completed's field comment for why a stray extra callback
   * invocation after this point must be a no-op rather than re-running
   * (non-idempotent) logic like _async_handle_redirect or a second
   * ctls_conn_handshake_step call on an already-failed handshake. */
  ctx->hop_completed = true;
  _async_fulfill_chain(ctx->chain, rv, resp);
}

/* Builds the chttpcli_response from the completed parse (mirrors Tier 1's
 * own response-building code at the tail of chttp_do_internal). Transfers
 * ownership of ctx->pctx.headers/ctx->bb.buf out of ctx (nulling them there)
 *; called BEFORE _async_finish_connection's idle-pool-offer path, which
 * would otherwise free those exact same fields while resetting ctx for its
 * idle life; see _async_on_data's CHTTP1_PAUSED handling for why the ordering
 * (build response, then finish the connection, then actually fulfil) matters
 * on its own terms too.
 *
 * For a streaming request (ctx->chain->write_fn set), body bytes were
 * already delivered to the caller's callback as they arrived off the wire
 * (see _async_submit_hop/_async_retry_hop's sink wiring); ctx->bb was
 * never used as the sink at all, so there is nothing to transfer into
 * resp->body, which stays NULL/0 exactly like Tier 1's
 * chttpclient_do_streaming leaves chttpcli_response.body. Headers ARE still
 * parsed internally (redirect detection needs them regardless of sink), but
 * are freed here rather than exposed, matching chttpclient_do_streaming's
 * documented "response headers are not accessible via this path". */
static chttpcli_response *_async_build_response(chttp_async_ctx_t *ctx) {
  chttpcli_response *resp =
      (chttpcli_response *)_mem_calloc(ctx->mp, 1, sizeof(*resp));
  if (!resp) return NULL;
  resp->_m_procs = ctx->mp;
  resp->status_code = ctx->pctx.status_code;
  if (ctx->chain->write_fn) {
    _parse_ctx_free_fields(&ctx->pctx);
  } else {
    resp->body = ctx->bb.buf;
    resp->body_len = ctx->bb.len;
    resp->headers = ctx->pctx.headers;
    ctx->pctx.headers = NULL; /* ownership transferred to resp */
    ctx->bb.buf = NULL;       /* ownership transferred to resp */
  }
  return resp;
}

/* Builds the response and fulfills with it in one step; used by the eof-
 * driven completion path, which never pools its connection (a peer that
 * closes the connection to signal end-of-body is, by definition, not
 * offering keep-alive), so there is no ordering hazard with
 * _async_finish_connection to worry about here. */
static void _async_fulfill_success(chttp_async_ctx_t *ctx) {
  chttpcli_response *resp = _async_build_response(ctx);
  _async_fulfill(ctx, resp ? ccol_success : ccol_not_enough_memory, resp);
}

/* ========================================================================== */
/*                         ASYNC IDLE POOL (TIER 2)                           */
/* ========================================================================== */

/*
 * Tier 2's own keep-alive idle pool: chmap(char *origin_key -> cvec of
 * chttp_async_ctx_t*), scoped per chttpcli (cli->idle_pools_async), mirroring
 * Tier 1's idle_pools/_idle_pool_take/_idle_pool_offer in shape and policy
 * (same CHTTP_MAX_IDLE_PER_ORIGIN/CHTTP_MAX_IDLE_TOTAL/CHTTP_IDLE_MAX_AGE_MS
 * caps) but necessarily different in mechanism: a pooled connection here is
 * still attached to the shared reactor (there is no way to detach a live
 * connection's registration without closing it), so pooling a ctx means
 * transitioning it to CHTTP_ASYNC_IDLE and letting the dispatch callbacks'
 * own IDLE-state branches keep driving it; any activity
 * (or the natural EOF/hangup a dead connection eventually produces) is
 * treated as "no longer usable" and torn down through the exact same
 * on_close path a normal failed connection would use, just entered from a
 * different state. Because there is no way to synchronously peek a reactor-
 * owned fd the way Tier 1's MSG_PEEK liveness probe does, a connection that
 * dies in the narrow window between being popped out of the pool and
 * actually being reused is instead caught by the reused/any_bytes_read
 * retry-once mechanism at the point of use (see _async_retry_hop);
 * together these two mechanisms give Tier 2 the same effective guarantee
 * Tier 1's probe-then-retry combination does.
 *
 * A pooled ctx holds its OWN engine reference (acquired in
 * _async_idle_pool_offer, released in whichever of _async_idle_pool_take's
 * reuse path or the IDLE-state on_close path claims it next); separate
 * from any chain's, since the chain that produced this connection has
 * already been (or is about to be) fully fulfilled and torn down, but the
 * pooled connection itself must keep the engine alive for as long as it
 * sits there.
 */

#ifdef RUNNING_UNIT_TESTS
/*
 * Test-only fault-injection flags, consumed (reset to false) the first time
 * each is read so an injected failure from one test can never leak into an
 * unrelated later test sharing the same process. Setters are the "White-box
 * test helpers" further below.
 *
 * The first two flags exist because the two branches they simulate cannot be
 * reached through ordinary allocator-failure injection (the g_hop_fail_mp
 * pattern tests.c already uses elsewhere): _async_idle_pool_offer's
 * cvector_push_back call can never actually fail here, since
 * CHTTP_MAX_IDLE_PER_ORIGIN (4) equals cvector's own minimum_capacity (4);
 * the per-origin list is always created with enough backing capacity for
 * every element _idle_pool_offer's own has_room check will ever let through,
 * so this push_back never needs to grow (realloc) the array, and cannot fail
 * via allocator OOM. Similarly, _async_submit_hop's reused-connection
 * event_loop_modify call performs no allocation of its own, and every one of
 * its other failure conditions (bad args, a non-fd selectable, an
 * already-removed registration, the target direction already occupied) is
 * structurally unreachable for a connection that was just popped from the
 * idle pool, where it was always read-only registered. Both branches are
 * still real, reachable code (a future refactor could raise
 * CHTTP_MAX_IDLE_PER_ORIGIN past 4, or a future event_loop change could add a
 * new failure mode) and were the site of a real, ASan-found use-after-free
 * each; these hooks exist purely to keep that fixed behavior under permanent
 * regression coverage.
 *
 * The third flag exists for the identical reason, applied to
 * _async_on_readable_impl's hard-transport-error-vs-real-EOF distinction
 * (see that function's own comment): a genuine TCP RST or TLS-level fatal
 * error arriving in a SEPARATE dispatch from the data that preceded it is a
 * real, reachable production scenario, but its exact timing relative to
 * already-buffered, already-consumed data is an OS-level race no test can
 * pin down deterministically over a real socket (unlike, say, a graceful
 * close, which this file's own eof_delimited_body_without_content_length
 * tests already exercise reliably). This hook forces the NEXT read dispatch
 * for a ctx that has already had at least one real, successful read (i.e.
 * never the very first read of a fresh/reused connection) to be treated
 * exactly as if recv()/ctls_conn_read() had returned -1/ECONNRESET, without
 * touching the real socket at all.
 */
static _Atomic bool g_force_offer_push_fail_for_tests = false;
static _Atomic bool g_force_reactivate_fail_for_tests = false;
static _Atomic bool g_force_async_hard_read_error_for_tests = false;
#endif /* RUNNING_UNIT_TESTS */

/* Decrements idle_total_count_async and, if it just reached zero, wakes
 * anyone (namely __chttpclient_destroy) waiting on idle_async_drained for
 * every pooled connection to finish tearing down. Must be called with
 * cli->lock held. */
static void _async_idle_count_dec_locked(struct chttpclient *cli) {
  if (cli->idle_total_count_async > 0) cli->idle_total_count_async--;
  if (cli->idle_total_count_async == 0)
    cond_var_broadcast(cli->idle_async_drained);
}

/* Removes `target` from its origin's idle list if it's still there (swap-
 * with-last, since cvector only supports push_back/pop_back) and decrements
 * the count. A no-op (returns false) if target isn't found; e.g. it was
 * already popped out by _async_idle_pool_take's own staleness eviction just
 * before its natural death was ALSO detected via the IDLE-state on_data/
 * on_close path; both sides are safe to call this unconditionally. Must be
 * called with cli->lock held. */
/* Deliberately does NOT call _async_idle_count_dec_locked: that decrement
 * (and the idle_async_drained broadcast it may trigger once the count
 * reaches zero) is the caller's own responsibility, deferred until the ctx
 * has ACTUALLY been freed. An earlier version of this function decremented
 * right here, at removal time; which let __chttpclient_destroy observe
 * the count reach zero and proceed to free cli (and cli->m_procs) while
 * _async_idle_ctx_finish, the only caller of this function, was still
 * mid-teardown on a reactor worker thread, reading that same freed
 * cli->m_procs through ctx->mp inside its own _async_ctx_free call: a real
 * use-after-free, caught by ThreadSanitizer (not valgrind) via a stress
 * test that cycles many connections through real, server-initiated death
 * fast enough to make the window land. See _async_idle_ctx_finish's own
 * comment for the corrected ordering. */
static bool _async_idle_remove_locked(struct chttpclient *cli,
                                      chttp_async_ctx_t *target) {
  if (!cli->idle_pools_async || !target->origin_key) return false;
  cmap_pair kp = {.ptr = target->origin_key,
                  .size = strlen(target->origin_key) + 1};
  cmap_pair *vp = NULL;
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
    cvector_pop_back(list, &last); /* removes the last element; may shrink
                                    * the backing array, so any pointer into
                                    * it taken before this call is stale */
    if (i < cvector_elem_count(list)) {
      chttp_async_ctx_t **slot2 = (chttp_async_ctx_t **)cvector_at(list, i);
      *slot2 = last;
    }
    /* Prune the now-empty per-origin list/map entry; see _idle_pool_take's
     * identical comment (Tier 1's own sibling function) for why this is
     * both necessary (unbounded distinct-origin growth otherwise, for a
     * long-running, many-origin client) and always safe (_async_idle_pool_
     * offer already handles "no entry for this origin yet" by creating one
     * fresh). */
    if (cvector_elem_count(list) == 0) {
      __cvector_destroy(list);
      chmap_delete_elem(cli->idle_pools_async, &kp);
    }
    return true;
  }
  return false;
}

/*
 * Attempts to pop a usable idle connection for `origin_key`. Returns true
 * and fills *out on success; false if no fresh candidate exists (the caller
 * falls back to a fresh connect).
 *
 * IMPORTANT; return contract: on a true return, ctx->idle_lock is left
 * LOCKED. The caller (_async_submit_hop) must keep it held for as long as
 * it takes to finish reconfiguring ctx for the new hop and attempting the
 * write, only unlocking once ctx is fully consistent again (all fields set,
 * write attempted). This is not a stylistic choice: popping a candidate out
 * of the pool's cvec makes it un-findable by a concurrent _async_idle_
 * pool_take, but does nothing to stop the reactor from independently
 * dispatching a readable/writable/error callback for its connection on
 * another thread at any moment; the connection stays fully attached to the
 * reactor for as long as it sat in the pool, exactly like every other race
 * described in this section.
 * Without holding idle_lock across the WHOLE reconfiguration (not just the
 * state/chain pair, as an earlier version of this function attempted), a
 * concurrent on_close could see ctx in a half-reconfigured state; state
 * already flipped off CHTTP_ASYNC_IDLE, but hop/pctx/wire/etc. still being
 * written by this thread; and tear ctx down (freeing it, destroying
 * idle_lock itself) while this function's caller is still using it: a real,
 * caught-in-development use-after-free, distinct from (and deeper than) the
 * earlier state/chain torn-write bug. _async_dispatch_kind (used by
 * on_readable/on_writable/on_error) takes the same lock, so any of them
 * racing this function simply blocks until the caller releases it, by which
 * point ctx is fully self-consistent one way or the other.
 *
 * The ENTIRE classify-and-pop decision for one origin runs inside a single
 * `cli->lock` critical section, scanning the per-origin cvec back-to-front
 * (from the most-recently-offered/"warmest" candidate at the highest index
 * toward the oldest at index 0; deliberately preserving the exact LIFO
 * preference the previous cvector_pop_back-based design had; scanning
 * front-to-back instead would silently start preferring the OLDEST fresh
 * candidate, a real behavioural regression, since older idle connections
 * are statistically more likely to have already been closed by the peer).
 * A candidate older than CHTTP_IDLE_MAX_AGE_MS is shut down right here
 * (reading its fd is safe: this whole walk holds cli->lock, and removal
 * from this vector, the only thing that could invalidate a candidate,
 * is *also* always gated by this same lock) but is deliberately left
 * exactly where it is in the vector, untouched otherwise (no write to
 * state/chain/hop_completed, no removal): this function does NOT tear a
 * stale candidate down itself. shutdown() forces a genuine EPOLLIN/EOF on
 * its still-read-registered fd, and the existing, entirely unmodified
 * _async_on_readable_impl idle-branch -> _async_idle_ctx_finish path (which
 * already does its own exclusive claim via _async_idle_remove_locked, and
 * already releases the pooled engine reference) reaps it for real, from
 * dispatch context. The first genuinely fresh candidate found is popped
 * (swap-with-last, mirroring _async_idle_remove_locked's own removal
 * pattern) and returned.
 *
 * This replaces an earlier design where a stale candidate was popped and
 * torn down (_async_ctx_finish) directly, from this application thread;
 * a real, ASan-reproduced use-after-free: a reactor dispatch callback for
 * that exact, still-registered ctx can legitimately be in flight (or about
 * to be invoked) on a different thread at the same moment, and the OS can
 * preempt that thread for an unbounded duration between passing
 * event_loop's own liveness check and ever touching ctx; during which
 * this application thread could free it out from under that callback. A
 * per-ctx atomic refcount pinned by every dispatch callback (an earlier
 * fix attempt) did not close this either: the pin's own very first read of
 * the refcount can itself race a concurrent free the same way. The only
 * fix that closes this by construction is the one implemented here: no
 * application thread ever tears a registered ctx down at all; a stale
 * candidate is merely marked (via shutdown(), not via any ctx field) for
 * the SAME dispatch-context reaping path an organically-dead idle
 * connection already goes through today, which is inherently safe since it
 * is inherently serialised against every other dispatch for that exact
 * registration (event_loop's own entry->dispatch_lock and entry->refcount-
 * gated "one job in flight per entry" invariant).
 *
 * Accepted tradeoff: a shutdown-but-not-yet-reaped stale candidate still
 * counts against CHTTP_MAX_IDLE_PER_ORIGIN/CHTTP_MAX_IDLE_TOTAL in
 * _async_idle_pool_offer's capacity checks until the dispatch-triggered
 * reap actually runs (typically within one reactor turnaround); a minor,
 * transient reduction in effective pool capacity, not a correctness issue,
 * traded deliberately against the alternative (this function tearing it
 * down itself, reopening the exact bug just described).
 */
static bool _async_idle_pool_take(struct chttpclient *cli,
                                  const char *origin_key,
                                  chttp_async_chain_t *chain,
                                  chttp_async_ctx_t **out) {
  chttp_async_ctx_t *ctx = NULL;
  mutex_lock(cli->lock);
  if (cli->idle_pools_async) {
    cmap_pair kp = {.ptr = (void *)origin_key, .size = strlen(origin_key) + 1};
    cmap_pair *vp = NULL;
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
          long age_ms = (now.tv_sec - cand->last_used.tv_sec) * 1000L +
                        (now.tv_nsec - cand->last_used.tv_nsec) / 1000000L;
          if (age_ms > CHTTP_IDLE_MAX_AGE_MS) {
            int fd = cand->fd;
            if (fd >= 0) shutdown(fd, SHUT_RDWR);
            continue;
          }
          chttp_async_ctx_t *last = NULL;
          cvector_pop_back(list, &last); /* may shrink the backing array;
                                          * any pointer into it taken before
                                          * this call is stale */
          if (idx < cvector_elem_count(list)) {
            chttp_async_ctx_t **slot2 =
                (chttp_async_ctx_t **)cvector_at(list, idx);
            *slot2 = last;
          }
          _async_idle_count_dec_locked(cli);
          ctx = cand;
          /* Prune the now-empty per-origin list/map entry; see
           * _idle_pool_take's identical comment (Tier 1's own sibling
           * function) for why this is both necessary and always safe.
           * Note: a stale candidate discarded above via `continue` is
           * deliberately left in the list (reaped later via dispatch, not
           * here - see this function's own file-level comment), so this
           * check can only ever fire on the genuine-reuse exit, never mid-
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
  mutex_unlock(cli->lock);
  if (!ctx) return false;

  mutex_lock(ctx->idle_lock); /* left locked; see contract above */
  /* See ctx->deadline_lock's own field comment for why this dedicated leaf
   * lock (not idle_lock, which the deadline sweep deliberately never
   * takes) is what makes this safe for the sweep to read concurrently. */
  mutex_lock(ctx->deadline_lock);
  ctx->overall_deadline = chain->overall_deadline;
  mutex_unlock(ctx->deadline_lock);
  ctx->chain = chain;
  ctx->state = CHTTP_ASYNC_WRITING;
  *out = ctx;
  return true;
}

/*
 * Offers a still-good, keep-alive-eligible connection back to cli's async
 * idle pool, bounded by the same per-origin/total caps Tier 1 uses. The
 * caller's contract is genuinely two-valued, not three: `false` means
 * "not handled, ctx is left COMPLETELY UNTOUCHED (still attached to its
 * original chain, in whatever state it was in on entry); fall back to
 * closing it normally", exactly like Tier 1's identical "a lost
 * optimisation opportunity, never a correctness issue" comment; `true`
 * means "handled, do not touch ctx again", covering BOTH of two internally
 * different outcomes: (1) the ordinary success case, where ctx genuinely
 * joins the idle pool, and (2) the "vanishingly rare" cvector_push_back OOM
 * case below, where ctx has already been torn down by this function
 * itself. Returning `false` for case (2) (an earlier version of this
 * function did exactly that) is a real double-free: the caller's own
 * fallback path for `false` calls _async_ctx_finish, which would then tear
 * down a ctx this function had already released its reference to. Found
 * via the same reference-timeline tracing that, in an earlier version of
 * this file's design, also found a per-ctx atomic refcount's own
 * resurrection race (see pending_app_teardown's field comment for how that
 * whole class of problem was ultimately closed, by construction, instead).
 *
 * On success, detaches ctx from its original chain, releasing the one chain
 * reference this hop was holding for it (the hop is over; the chain no
 * longer owns this connection, the idle pool does); forgetting this
 * release was a real, valgrind-caught reference leak during this feature's
 * own development, since a pooled connection never goes through the normal
 * _async_ctx_teardown path (which is the only OTHER place that releases a
 * ctx's chain reference) until it is later reused or evicted.
 *
 * The capacity check (is there room in the pool for this origin, creating
 * the per-origin list on demand if needed) happens FIRST, entirely under
 * cli->lock, deciding definitively whether this offer can succeed before
 * touching ctx AT ALL. Only once that is certain does this function reset
 * ctx to a clean idle state (clearing the per-hop parse/body/wire buffers
 * and marking it CHTTP_ASYNC_IDLE) and insert it; both still under the
 * SAME cli->lock critical section, so no other thread can pop from the list
 * in between. This ordering matters for two independent reasons: (1) ctx
 * must never become visible/poppable via _async_idle_pool_take while only
 * partially reset; _async_idle_pool_take unconditionally overwrites
 * whatever it finds without waiting for anything, so a concurrent take()
 * starting to reconfigure ctx for a brand new hop while this function was
 * still freeing/resetting those exact same fields for the OLD hop would be
 * a genuine concurrent-access data race on the fields themselves, not just
 * a logical inconsistency; caught during this feature's own development;
 * (2) the caller's "ctx untouched on failure" contract above would
 * otherwise be violated by a version of this function that resets ctx
 * speculatively before knowing whether the pool has room.
 */
static bool _async_idle_pool_offer(chttp_async_ctx_t *ctx) {
  struct chttpclient *cli = ctx->cli;

  /* This ctx is about to belong to the idle pool, not any chain; it needs
   * its own engine reference to keep the reactor alive while it sits here,
   * independent of whichever chain's lifetime just ended. Acquired before
   * anything else so a failure here need not unwind any pool state at all. */
  if (_client_engine_acquire() != ccol_success) return false;

  cvec list = NULL;
  bool has_room = false;
  mutex_lock(cli->lock);
  if (!cli->destroying && cli->idle_pools_async &&
      cli->idle_total_count_async < CHTTP_MAX_IDLE_TOTAL) {
    cmap_pair kp = {.ptr = ctx->origin_key,
                    .size = strlen(ctx->origin_key) + 1};
    cmap_pair *vp = NULL;
    if (chmap_get_elem_ref(cli->idle_pools_async, &kp, &vp) == ccol_success &&
        vp) {
      list = _read_cvec(vp->ptr);
    }
    /* A genuinely new origin only gets a fresh entry while the distinct-
     * origin cap still has room; see CHTTP_MAX_IDLE_ORIGINS's own comment
     * (Tier 1's _idle_pool_offer, the sibling this mirrors). At capacity,
     * list stays NULL, falling through to the same "doesn't fit, don't
     * pool it" path has_room's own check below already produces. */
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
    mutex_unlock(cli->lock);
    _client_engine_release();
    return false; /* ctx untouched; caller falls back to a normal close */
  }

  /* Committed: this ctx WILL leave chain and become idle-pooled. Capture
   * the chain reference to release below, then reset ctx while STILL
   * holding cli->lock (see the file-level comment above for why). */
  chttp_async_chain_t *old_chain = ctx->chain;

  /* _parse_ctx_free_fields only frees+nulls cur_field/cur_value/location/
   * headers; every other field (cur_field_cap/cur_value_cap in
   * particular) is left stale. That has always been safe at its other call
   * sites (_async_ctx_free, right before the whole ctx is freed; and Tier
   * 1's per-hop chttp_parse_ctx_t, a fresh stack struct every hop) because
   * the struct itself is discarded immediately after. Here it is NOT
   * discarded (ctx is about to be reused for a future hop) so a stale,
   * non-zero cur_field_cap paired with a freshly-NULLed cur_field would
   * make _accum_append's very next call skip its realloc (capacity already
   * "big enough") and memcpy into a NULL buffer. The explicit memset below
   * (after _parse_ctx_free_fields has freed everything that needs freeing)
   * restores the same all-zero state a freshly _mem_calloc'd ctx already
   * has, which every other user of chttp_parse_ctx_t implicitly relies on. */
  _parse_ctx_free_fields(&ctx->pctx);
  memset(&ctx->pctx, 0, sizeof(ctx->pctx));
  _mem_free(ctx->mp, ctx->bb.buf);
  ctx->bb.buf = NULL;
  ctx->bb.len = ctx->bb.cap = 0;
  ctx->bb.oom = false;
  ctx->bb.too_large = false;
  _mem_free(ctx->mp, ctx->wire);
  ctx->wire = NULL;
  ctx->wire_len = ctx->wire_sent = 0;
  /* Expect: 100-continue state, stale the moment this hop is done; every
   * fresh hop attempt recomputes want_100_continue/header_len from scratch
   * in _async_submit_hop/_async_retry_hop and re-arms continue_decided to
   * false there too, but resetting here as well matches this function's
   * own "restore the same all-zero state a freshly _mem_calloc'd ctx
   * already has" discipline documented above. continue_carry should always
   * already be NULL by the time a hop reaches here (consumed the moment
   * the body write it was waiting on finished; see that field's own
   * comment), but freed defensively regardless, the same as wire/bb just
   * above. */
  _mem_free(ctx->mp, ctx->continue_carry);
  ctx->continue_carry = NULL;
  ctx->continue_carry_len = 0;
  ctx->want_100_continue = false;
  ctx->header_len = 0;
  ctx->retry_unsafe = false;
  atomic_store(&ctx->continue_decided, false);
  /* Stale the moment this ctx goes idle (it described the just-finished
   * hop's request path, not anything meaningful while pooled); freed here
   * rather than left dangling until the next _async_submit_hop call
   * overwrites it, matching wire/bb's identical treatment just above. */
  _mem_free(ctx->mp, ctx->path_and_query);
  ctx->path_and_query = NULL;
  ctx->hop_completed = false;
  ctx->reused = false;
  ctx->any_bytes_read = false;
  ctx->interim_responses_seen = 0;
  /* ctx->chain and ctx->state flip together, under idle_lock; see that
   * field's comment for why an unsynchronised pair of writes here could be
   * observed torn (state already IDLE, chain not yet NULL, or vice versa)
   * by a concurrent on_data/on_ready/on_close dispatch on another thread.
   * ctx->timed_out is reset in this same critical section, as late as
   * possible: the deadline sweep only ever sets it when either ctx is in a
   * connecting state (not true here; this ctx just finished a hop cleanly)
   * or has_chain is true (node->chain != NULL), so resetting it here, right
   * where chain is nulled and state becomes IDLE, closes the window (see
   * _client_deadline_sweep_once's own comment on has_chain) in which the
   * sweep could otherwise mark this ctx timed-out based on the
   * just-finished hop's already-stale overall_deadline, poisoning a FUTURE,
   * unrelated hop that later reuses it from the idle pool: without this
   * reset, that future hop's own dead-connection retry (_async_ctx_finish's
   * `reused && !any_bytes_read` branch) is starved by a stale ctx->timed_out
   * left over from a hop that already succeeded, reporting ccol_timed_out
   * to a caller whose own deadline was never in danger. */
  mutex_lock(ctx->idle_lock);
  ctx->chain = NULL;
  ctx->state = CHTTP_ASYNC_IDLE;
  ctx->timed_out = false;
  mutex_unlock(ctx->idle_lock);

  clock_gettime(CLOCK_MONOTONIC, &ctx->last_used);
  bool pushed;
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_force_offer_push_fail_for_tests, false)) {
    /* Simulated cvector_push_back failure: the real call is deliberately
     * NOT made, so list is left exactly as a genuine OOM would leave it
     * (nothing inserted); see g_force_offer_push_fail_for_tests's own
     * comment for why the real call can never actually fail here. */
    pushed = false;
  } else
#endif
  {
    pushed = (cvector_push_back(list, &ctx) == ccol_success);
  }
  if (pushed) cli->idle_total_count_async++;
  mutex_unlock(cli->lock);

  if (!pushed) {
    /* Vanishingly rare (OOM inside cvector_push_back itself, despite room
     * being confirmed available above); ctx has already been fully
     * detached from old_chain and reset to an idle shape, so it can no
     * longer be handed back to the caller as "just fio_close it as a
     * normal active ctx" the way the has_room==false path above can. Tear
     * it down directly instead. */
    _async_chain_release(old_chain);
    _async_ctx_free(ctx);
    _client_engine_release();
    return true; /* NOT false: ctx has already been fully torn down by this
                  * function itself (unlike the has_room==false path above,
                  * which returns false with ctx completely untouched); the
                  * caller (_async_finish_connection) must not ALSO call
                  * _async_ctx_finish on it. Returning false here was a
                  * second real bug found via the same reference-timeline
                  * tracing that, in an earlier version of this file's
                  * design, also found a per-ctx atomic refcount's own
                  * resurrection race (see pending_app_teardown's field
                  * comment): _async_finish_connection's contract is "false
                  * means fall back to tearing ctx down normally", which is
                  * exactly wrong for this specific failure; it would
                  * call _async_ctx_finish a second time on a ctx this
                  * function had already released its reference to, a
                  * genuine double-free (independent of, and in addition
                  * to, that other bug). */
  }

  _async_chain_release(old_chain);
  return true;
}

/*
 * Called once a hop's response has been fully consumed, before deciding
 * whether to redirect or fulfil; offers the connection back to cli's idle
 * pool if `reusable`, otherwise closes it. Mirrors Tier 1's identical
 * reusable/_idle_pool_offer dance in chttp_do_internal, applied uniformly
 * regardless of whether this hop turns out to be a redirect or the final
 * response (see _async_handle_redirect and _async_on_data's CHTTP1_PAUSED
 * handling, both of which call this before doing anything else with the
 * connection).
 */
/*
 * This module's own guaranteed-exactly-once teardown point for an ACTIVE
 * (non-idle-pooled) ctx: every place that ends such a connection (a hard
 * error, a timeout, an explicit close after a successful/redirect
 * completion that wasn't pooled) calls this exactly once, since event_loop
 * has no unconditional terminal callback of its own to rely on instead
 * (see the "ASYNC CONNECTION STATE MACHINE" section's file-level comment).
 *
 * Deliberately does NOT pre-emptively fulfil with a generic error before
 * checking retry-eligibility: doing so would mark ctx->hop_completed and
 * incorrectly skip a legitimate reused-connection retry. If neither
 * timed_out nor the reused-retry case applies, this function fulfils
 * nothing itself; _async_ctx_teardown's chain release below carries its own
 * backstop (guarded by chain->fulfilled) that fulfils with the generic
 * ccol_http_transfer_aborted exactly once, the same value every caller of
 * this function that doesn't need a more specific code already relied on.
 */
/* Forward declaration: this function's own body calls _async_retry_hop,
 * defined much further below (after the "ASYNC IDLE POOL" section,
 * alongside the TLS/plain write helpers that also need it). */
static void _async_retry_hop(chttp_async_ctx_t *ctx);

static void _async_ctx_finish(chttp_async_ctx_t *ctx) {
  if (!ctx->hop_completed) {
    if (ctx->timed_out) { /* _Atomic; plain read is already race-free */
      _async_fulfill(ctx, ccol_timed_out, NULL);
    } else if (ctx->reused && !ctx->any_bytes_read && !ctx->retry_unsafe) {
      _async_retry_hop(ctx); /* marks ctx->hop_completed = true itself */
    }
  }
  _async_ctx_teardown(ctx);
}

/*
 * This module's own teardown point for an IDLE-pooled ctx: removes it from
 * the pool and frees it. It holds its own engine reference (acquired in
 * _async_idle_pool_offer), not any chain's, since ctx->chain is NULL while
 * idle.
 *
 * Only actually tears ctx down if THIS call is the one that successfully
 * removes it from the pool's cvec (cli->lock is the true, sole arbiter of
 * exclusive ownership here; see _async_idle_remove_locked's own bool
 * return). If the removal finds nothing (already removed by a concurrent
 * caller), this returns immediately without touching ctx again.
 *
 * This is load-bearing, not a defensive nicety: with N reactor threads
 * sharing one epoll instance (no EPOLLEXCLUSIVE-style dedup), a genuinely
 * dead connection's EOF condition is PERSISTENT (unlike a one-shot
 * readable-data event, every subsequent recv()/peek on an already-closed
 * fd keeps reporting EOF, not EWOULDBLOCK), so two sequential stale
 * dispatches for the same idle ctx can both legitimately observe real EOF
 * via the peek in _async_on_readable's idle branch and both decide to
 * evict it. An earlier version of this function unconditionally called
 * _async_ctx_free/_client_engine_release regardless of whether the removal
 * above actually found anything (the old comment even said "a no-op if
 * some other path already removed it" while the code below it was NOT,
 * in fact, a no-op); a real double-free, caught via valgrind and a
 * flaky-test repro loop against async_idle_pool.dead_connection_detected_
 * and_retried specifically because that test's second request is exactly
 * what makes the first request's pooled connection genuinely, persistently
 * dead (the server closes its end), rather than merely racing a stale-but-
 * harmless spurious dispatch.
 *
 * idle_total_count_async is decremented (and idle_async_drained possibly
 * broadcast) only in a THIRD, separate locked section, after both
 * _async_ctx_free and _client_engine_release have already fully run below,
 * not folded into the removal step above. __chttpclient_destroy treats the
 * count reaching zero as its signal that every pooled connection has
 * genuinely finished tearing down before it proceeds to free cli (and
 * cli->m_procs, which ctx->mp still points at); decrementing at removal
 * time let that signal fire while this function's own _async_ctx_free call
 * was still using cli->m_procs on this thread, a real use-after-free
 * ThreadSanitizer caught that valgrind alone had not (see
 * _async_idle_remove_locked's own comment for the full account).
 */
static void _async_idle_ctx_finish(chttp_async_ctx_t *ctx) {
  struct chttpclient *cli = ctx->cli;
  mutex_lock(cli->lock);
  bool removed = _async_idle_remove_locked(cli, ctx);
  mutex_unlock(cli->lock);
  if (!removed) return;
  _async_ctx_free(ctx);
  _client_engine_release();
  mutex_lock(cli->lock);
  _async_idle_count_dec_locked(cli);
  mutex_unlock(cli->lock);
}

static void _async_finish_connection(chttp_async_ctx_t *ctx, bool reusable) {
  if (reusable && ctx->origin_key && _async_idle_pool_offer(ctx)) return;
  _async_ctx_finish(ctx);
}

/* Forward declaration: _async_on_readable (below) calls this on a reused
 * connection that turns out to be dead before any response byte was read;
 * the full definition (after _async_connect_task, which it needs to queue
 * the replacement attempt) comes later in this file, alongside
 * _async_submit_hop. */
static void _async_retry_hop(chttp_async_ctx_t *ctx);

/* Forward declaration: _async_tls_advance (below) calls this once the
 * handshake completes, before its own definition later in this file. */
static void _async_tls_try_write(chttp_async_ctx_t *ctx);

/* Forward declaration: _async_plain_try_write/_async_tls_try_write (below)
 * call this to process any Expect: 100-continue trailing bytes carried
 * forward across a body write (see ctx->continue_carry's own field
 * comment); the full definition, shared with the ordinary on_readable
 * dispatch path, comes later in this file, alongside it. */
static void _async_process_reading_data(chttp_async_ctx_t *ctx,
                                        const char *data, size_t data_len);

/*
 * Drives the client TLS handshake one step at a time from either
 * _async_on_readable or _async_on_writable, whichever fires next (this
 * ctx's single registration is flipped between read/write direction via
 * event_loop_modify as the handshake's own WANT_READ/WANT_WRITE demands).
 * No locking needed around ctx->tls: event_loop's own per-registration
 * dispatch_lock already guarantees this ctx's read and write direction are
 * never dispatched concurrently with each other (see the file-level comment
 * above for why this replaces the old tls_lock entirely).
 */
static void _async_tls_advance(chttp_async_ctx_t *ctx) {
  ctls_handshake_result_t r = ctls_conn_handshake_step(ctx->tls);
  if (r == CTLS_HANDSHAKE_DONE) {
    ctx->state = CHTTP_ASYNC_WRITING;
    if (event_loop_modify(cli_engine_bundler.reactor, ctx->reg,
                          ccol_select_write) != ccol_success) {
      _async_ctx_finish(ctx);
      return;
    }
    _async_tls_try_write(ctx);
    return;
  }
  if (r == CTLS_HANDSHAKE_ERROR) {
    /* 0 == X509_V_OK by OpenSSL convention; ctls.h intentionally does not
     * expose OpenSSL headers to callers, so the raw value is compared
     * directly rather than via the X509_V_OK symbol (mirrors Tier 1's
     * _tls_handshake). Never retry-eligible regardless of ctx->reused: a
     * handshake failure is a TLS-level problem, not evidence the pooled
     * connection had merely gone stale.
     *
     * ctx->timed_out (_Atomic; plain read is already race-free, matching
     * _async_ctx_finish's own read of the same field) is checked FIRST: the
     * deadline sweep's shutdown(fd, SHUT_RDWR) against a stuck handshake is
     * exactly what surfaces here as CTLS_HANDSHAKE_ERROR, and without this
     * check a connect_timeout_ms/request_timeout_ms expiry was always
     * misreported as a TLS handshake/certificate failure instead of
     * ccol_timed_out, contradicting chttpclient.h's documented contract. */
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
  if (event_loop_modify(cli_engine_bundler.reactor, ctx->reg, want) !=
      ccol_success) {
    _async_ctx_finish(ctx);
  }
}

/*
 * Writes as much of ctx->wire[ctx->wire_sent..] as ctls_conn_write will
 * currently accept, tracking partial progress (ordinary short-write
 * semantics, not an error). Called once right after the handshake completes
 * and again from _async_on_writable each time write interest fires.
 */
static void _async_tls_try_write(chttp_async_ctx_t *ctx) {
  /* Expect: 100-continue - stop at the header/body boundary instead of the
   * end of wire, exactly once per hop attempt (continue_decided flips true
   * the moment either a real "100 Continue" arrives or the wait times out;
   * see CHTTP_ASYNC_AWAITING_CONTINUE's own doc comment for the full
   * three-outcome contract this mirrors from Tier 1's _chttp_send_and_read).
   */
  bool awaiting_continue =
      ctx->want_100_continue && !atomic_load(&ctx->continue_decided);
  size_t target_len = awaiting_continue ? ctx->header_len : ctx->wire_len;
  while (ctx->wire_sent < target_len) {
    ssize_t n = ctls_conn_write(ctx->tls, ctx->wire + ctx->wire_sent,
                                target_len - ctx->wire_sent);
    if (n > 0) {
      ctx->wire_sent += (size_t)n;
      continue;
    }
    if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) return;
    /* n == 0 or a hard error: the connection is dead. Retry-eligibility is
     * checked inside _async_ctx_finish itself; no pre-emptive fulfil here
     * (see that function's own comment for why). */
    _async_ctx_finish(ctx);
    return;
  }
  if (awaiting_continue) {
    /* Header block fully sent; hold the body (wire[header_len..wire_len))
     * back and wait for the server's "100 Continue"/direct final response/
     * timeout. wire must stay alive regardless of ctx->reused here - unlike
     * the ordinary full-send case below, the body bytes still to be sent
     * live in it; freeing it now would be a clean fresh-ctx use-after-free
     * the very next time this hop attempts to send the body. */
    /* continue_deadline is computed and stored BEFORE the ctx->state store
     * below, not after: the deadline sweep polls ctx->state independently,
     * on its own timer, with no coordination beyond that one atomic load,
     * and treats CHTTP_ASYNC_AWAITING_CONTINUE becoming visible as its
     * signal that continue_deadline is now meaningful to consult (see that
     * field's own comment). ctx->state is the actual publish point (a
     * concurrent atomic_load of it that observes this store is guaranteed,
     * per C11's sequentially-consistent atomics, to see every plain write
     * sequenced before that store in this thread); writing continue_deadline
     * afterward would let the sweep observe the new state while
     * continue_deadline still holds a stale or zeroed value (e.g. from
     * calloc, or a prior hop on a reused ctx), reading it as "already
     * expired" and firing the wait-timeout path immediately instead of
     * genuinely waiting up to CHTTP_100_CONTINUE_WAIT_MS. */
    mutex_lock(ctx->deadline_lock);
    chttp_deadline_t overall = ctx->overall_deadline;
    mutex_unlock(ctx->deadline_lock);
    ctx->continue_deadline =
        _deadline_earlier(_deadline_make(CHTTP_100_CONTINUE_WAIT_MS), overall);
    ctx->state = CHTTP_ASYNC_AWAITING_CONTINUE;
    if (event_loop_modify(cli_engine_bundler.reactor, ctx->reg,
                          ccol_select_read) != ccol_success) {
      _async_ctx_finish(ctx);
    }
    return;
  }
  /* A REUSED ctx's wire buffer must survive a "successful" write (accepted
   * by the local socket buffer, which does NOT guarantee the peer is
   * actually still alive to respond); see _async_retry_hop, which can run
   * against THIS ctx later if the subsequent read side discovers the
   * connection was already dead, and needs the ORIGINAL request bytes to
   * resend on a fresh connection. Freeing wire here unconditionally was a
   * real, reproducible bug (not just the earlier NULL/stale-wire_len crash
   * this comment used to describe): once that crash was fixed by also
   * zeroing wire_len/wire_sent alongside wire, _async_retry_hop's transfer
   * of a NULL wire + wire_len==0 into the retry ctx silently made the retry
   * send ZERO bytes (its own write loop's `wire_sent < wire_len` check is
   * immediately false), so the retry's connection sat open while the real
   * server-side mock waited out its own 5-second read timeout before
   * closing it; caught via async_idle_pool.dead_connection_detected_and_
   * retried failing intermittently under full-suite load (never in
   * isolation, since it needs enough concurrent connections for a write to
   * a reused-but-already-peer-closed socket to actually succeed locally
   * before the read side notices), root-caused with targeted stderr tracing
   * on both the client and the test mock server after ThreadSanitizer
   * confirmed no data race was involved. A fresh (non-reused) ctx is never
   * retried (see _async_ctx_finish's own reused-only check), so its wire is
   * still freed eagerly here, exactly as before; only the reused case needs
   * to keep it alive, until either _async_retry_hop's own transfer, a
   * successful _async_idle_pool_offer (which resets wire unconditionally
   * for the next hop), or _async_ctx_free's own unconditional free at
   * teardown claims it. */
  if (!ctx->reused) {
    _mem_free(ctx->mp, ctx->wire);
    ctx->wire = NULL;
    ctx->wire_len = ctx->wire_sent = 0;
  }
  ctx->state = CHTTP_ASYNC_READING;
  if (event_loop_modify(cli_engine_bundler.reactor, ctx->reg,
                        ccol_select_read) != ccol_success) {
    _async_ctx_finish(ctx);
    return;
  }
  /* See ctx->continue_carry's own field comment: non-NULL only after an
   * Expect: 100-continue hop whose "100 Continue" line arrived bundled with
   * the start of the real final response, in the same read() that also
   * completed this body write on a LATER dispatch (a fully synchronous
   * completion is handled inline by _async_awaiting_continue_on_data
   * itself, without ever setting this field at all - see that function's
   * own comment). Processed here, the moment the write that had to happen
   * first actually finishes, rather than waiting for a subsequent
   * on_readable dispatch that may never come (the bytes already sitting in
   * continue_carry are everything the peer is ever going to send if it
   * considers the exchange already complete). Captures mp locally first:
   * _async_process_reading_data may free ctx entirely on this same call
   * (e.g. a completed, non-redirecting final response), so ctx must not be
   * touched again afterward. */
  if (ctx->continue_carry) {
    ccol_memmgmt_procs_t *mp = ctx->mp;
    char *carry = ctx->continue_carry;
    size_t carry_len = ctx->continue_carry_len;
    ctx->continue_carry = NULL;
    ctx->continue_carry_len = 0;
    _async_process_reading_data(ctx, carry, carry_len);
    _mem_free(mp, carry);
  }
}

/*
 * Plain-HTTP counterpart to _async_tls_try_write: writes as much of
 * ctx->wire[ctx->wire_sent..] as a raw, non-blocking send() will currently
 * accept. Called once right after a fresh connection's connect completes
 * and again from _async_on_writable_impl's WRITING branch each time write
 * interest fires (a reused connection's first write attempt is
 * deliberately NOT made synchronously by _async_submit_hop itself; see
 * that function's own reused-connection comment for why), so the exact
 * same partial-write/EWOULDBLOCK/hard-failure handling is not duplicated
 * across both call sites.
 */
static void _async_plain_try_write(chttp_async_ctx_t *ctx) {
  /* See _async_tls_try_write's identical comment for why this stops at
   * header_len rather than wire_len while awaiting a "100 Continue". */
  bool awaiting_continue =
      ctx->want_100_continue && !atomic_load(&ctx->continue_decided);
  size_t target_len = awaiting_continue ? ctx->header_len : ctx->wire_len;
  while (ctx->wire_sent < target_len) {
    ssize_t n = send(ctx->fd, ctx->wire + ctx->wire_sent,
                     target_len - ctx->wire_sent, MSG_NOSIGNAL);
    if (n > 0) {
      ctx->wire_sent += (size_t)n;
      continue;
    }
    if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) return;
    _async_ctx_finish(ctx);
    return;
  }
  if (awaiting_continue) {
    /* See _async_tls_try_write's identical branch for the full rationale
     * (both the wire-buffer-lifetime note and, importantly, why
     * continue_deadline must be computed and stored BEFORE ctx->state is
     * published as CHTTP_ASYNC_AWAITING_CONTINUE, not after). */
    mutex_lock(ctx->deadline_lock);
    chttp_deadline_t overall = ctx->overall_deadline;
    mutex_unlock(ctx->deadline_lock);
    ctx->continue_deadline =
        _deadline_earlier(_deadline_make(CHTTP_100_CONTINUE_WAIT_MS), overall);
    ctx->state = CHTTP_ASYNC_AWAITING_CONTINUE;
    if (event_loop_modify(cli_engine_bundler.reactor, ctx->reg,
                          ccol_select_read) != ccol_success) {
      _async_ctx_finish(ctx);
    }
    return;
  }
  /* See _async_tls_try_write's identical comment for the full history: a
   * REUSED ctx must keep its wire buffer alive past a "successful" write,
   * since _async_retry_hop needs the original request bytes to resend if
   * the read side then discovers the peer was already dead; only a fresh
   * (never-retried) ctx frees it here eagerly. */
  if (!ctx->reused) {
    _mem_free(ctx->mp, ctx->wire);
    ctx->wire = NULL;
    ctx->wire_len = ctx->wire_sent = 0;
  }
  ctx->state = CHTTP_ASYNC_READING;
  if (event_loop_modify(cli_engine_bundler.reactor, ctx->reg,
                        ccol_select_read) != ccol_success) {
    _async_ctx_finish(ctx);
    return;
  }
  /* See ctx->continue_carry's own field comment: non-NULL only after an
   * Expect: 100-continue hop whose "100 Continue" line arrived bundled with
   * the start of the real final response, in the same read() that also
   * completed this body write on a LATER dispatch (a fully synchronous
   * completion is handled inline by _async_awaiting_continue_on_data
   * itself, without ever setting this field at all - see that function's
   * own comment). Processed here, the moment the write that had to happen
   * first actually finishes, rather than waiting for a subsequent
   * on_readable dispatch that may never come (the bytes already sitting in
   * continue_carry are everything the peer is ever going to send if it
   * considers the exchange already complete). Captures mp locally first:
   * _async_process_reading_data may free ctx entirely on this same call
   * (e.g. a completed, non-redirecting final response), so ctx must not be
   * touched again afterward. */
  if (ctx->continue_carry) {
    ccol_memmgmt_procs_t *mp = ctx->mp;
    char *carry = ctx->continue_carry;
    size_t carry_len = ctx->continue_carry_len;
    ctx->continue_carry = NULL;
    ctx->continue_carry_len = 0;
    _async_process_reading_data(ctx, carry, carry_len);
    _mem_free(mp, carry);
  }
}

/* Forward declaration: _async_on_readable's redirect-detection branches
 * (below) call this; its full definition comes after _async_connect_task,
 * which it needs in order to queue the next hop. */
static void _async_handle_redirect(chttp_async_ctx_t *ctx, bool reusable);

/*
 * Reads ctx->state, and re-checks ctx->hop_completed, together under
 * ctx->idle_lock; see that field's comment for why a plain unlocked read of
 * state is not safe here specifically. Returns CHTTP_ASYNC_DISPATCH_
 * ABANDONED (rather than "not idle") if hop_completed became true between
 * the caller's own entry check (at the very top of _async_on_readable/
 * _on_writable/_on_error) and this function's own lock acquisition; e.g.
 * one of _async_submit_hop's/_async_submit_hop_fail's application-thread
 * failure paths (see pending_app_teardown's own field comment) raced this
 * exact dispatch, setting hop_completed under the same idle_lock this
 * function also takes. The caller must return immediately in that case
 * (via _async_ctx_handle_if_abandoned below, never touching ctx directly
 * itself): falling through to "active hop" processing would misinterpret
 * an already-abandoned ctx's stale state/chain/pctx as a live hop,
 * potentially misdelivering data to the wrong chain. Note that mechanism 1
 * (_async_idle_pool_take's staleness eviction) never reaches here at all
 * any more: it does not touch state/hop_completed for a stale candidate,
 * only shutdown()s its fd, so a stale candidate is always still
 * legitimately IDLE from this function's point of view; its real teardown
 * happens via the separate _async_idle_ctx_finish path below, not via this
 * ABANDONED case. */
typedef enum {
  CHTTP_ASYNC_DISPATCH_ACTIVE,
  CHTTP_ASYNC_DISPATCH_IDLE,
  CHTTP_ASYNC_DISPATCH_ABANDONED,
} chttp_async_dispatch_kind_t;

static chttp_async_dispatch_kind_t _async_dispatch_kind(
    chttp_async_ctx_t *ctx) {
  mutex_lock(ctx->idle_lock);
  bool completed = ctx->hop_completed;
  bool idle = (ctx->state == CHTTP_ASYNC_IDLE);
  mutex_unlock(ctx->idle_lock);
  if (completed) return CHTTP_ASYNC_DISPATCH_ABANDONED;
  return idle ? CHTTP_ASYNC_DISPATCH_IDLE : CHTTP_ASYNC_DISPATCH_ACTIVE;
}

/*
 * Called from every dispatch-callback code path that discovers
 * ctx->hop_completed already true, whether via a plain top-of-function
 * fast check (_async_on_writable_impl, and _async_on_readable_impl/
 * _on_error_impl's own top-of-function check, BEFORE _async_dispatch_kind
 * is even called) or via _async_dispatch_kind's own ABANDONED result (the
 * narrower race window between that top-of-function check and this
 * function's own idle_lock-guarded re-check). Two possible origins for
 * hop_completed already being true: (1) an earlier dispatch invocation for
 * this exact registration already ran the real teardown itself before
 * setting hop_completed; nothing further to do; (2) one of
 * _async_submit_hop's/_async_submit_hop_fail's application-thread failure
 * paths set hop_completed AND pending_app_teardown, then shut ctx->fd down
 * and returned without ever touching ctx again, deliberately deferring the
 * real, destructive teardown to whichever dispatch notices (see
 * pending_app_teardown's own field comment for the full rationale).
 * Distinguishes the two via pending_app_teardown; calls _async_ctx_teardown
 * (NOT _async_ctx_finish, since hop_completed is already true, and any
 * legitimate retry was already queued by _async_retry_hop before
 * pending_app_teardown was ever set, so re-running _async_ctx_finish's own
 * retry-check here would be wrong) only for case (2). Every caller must
 * return immediately afterward regardless of which case applies; ctx may
 * be fully freed by the time this returns.
 *
 * Both the top-of-function checks in _async_on_readable_impl/_on_error_impl
 * are genuinely, primarily reachable for case (2), not a defense-in-depth
 * nicety: mechanism 2's shutdown(fd, SHUT_RDWR) specifically relies on the
 * kernel delivering an EPOLLIN/EPOLLERR condition on this still-read-
 * registered fd, which dispatches to exactly one of those two functions,
 * and their top-of-function check runs before _async_dispatch_kind is ever
 * reached. Missing this call at either of those two checks would make
 * mechanism 2 silently non-functional: ctx (and the chain reference it
 * holds) would leak permanently, and chttpclient_destroy would hang
 * forever waiting for async_in_flight_count to reach zero.
 * _async_on_writable_impl's own top-of-function check is not reachable for
 * case (2) today (event_loop_modify never mutates a registration's
 * direction on any failure path it can return through, so a reused ctx
 * whose reactivation failed stays read-registered, never write-registered)
 * but is given the identical treatment anyway, purely so correctness does
 * not depend on that staying true forever, mirroring this same function's
 * own pre-existing hop_completed check (which the file already documents
 * as "structurally cannot fire" for the read-only-registered case, kept
 * anyway).
 *
 * Can never double-fire, within one dispatch invocation or across separate
 * ones. Within one invocation: whichever of the two checks in a given
 * function sees hop_completed true first calls this and returns
 * immediately, so the second check in that same function is structurally
 * unreachable once the first has fired. Across separate invocations:
 * _event_loop_poller_collect (cthreadcomm.c) refuses to submit a second
 * job for an entry while entry->refcount > 0; _async_ctx_teardown ->
 * _async_ctx_destroy_now calls event_loop_remove (setting reg->removed =
 * true) before returning, after which no future poller pass ever builds
 * another job item referencing this reg. Combined with
 * entry->dispatch_lock serialising same-entry callbacks, _async_ctx_
 * teardown is reachable via dispatch at most once per ctx.
 */
static void _async_ctx_handle_if_abandoned(chttp_async_ctx_t *ctx) {
  if (ctx->pending_app_teardown) _async_ctx_teardown(ctx);
}

static void _async_on_writable_impl(event_loop loop, ccol_selectable *sel,
                                    chttp_async_ctx_t *ctx) {
  (void)loop;
  (void)sel;
  /* See ctx->hop_completed's field comment: a stray dispatch can still
   * arrive after this ctx already reached a terminal outcome, and must not
   * re-run any of the state-transition logic below. An idle-pooled ctx's
   * registration only ever carries read direction (see the "ASYNC IDLE
   * POOL" section), so on_writable structurally cannot fire for one; no
   * idle check is needed here (unlike on_readable/on_error). See
   * _async_ctx_handle_if_abandoned's own comment for why this call is
   * still needed here despite this path not being reachable today. */
  if (ctx->hop_completed) {
    _async_ctx_handle_if_abandoned(ctx);
    return;
  }
  /* See ctx->reg's own field comment: a dispatch for this ctx's very first
   * (write-direction) registration can legitimately arrive before
   * _async_connect_task's own `ctx->reg = event_loop_add(...)` assignment
   * has finished, especially for a loopback connect that's often already
   * writable the instant it's registered. Nothing else has happened yet in
   * that window (no read, no write), so simply returning is always safe;
   * the same level-triggered condition is reported again on the very next
   * epoll_wait, by which point ctx->reg is set. */
  if (!ctx->reg) return;

  if (ctx->state == CHTTP_ASYNC_CONNECTING) {
    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    if (getsockopt(ctx->fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0 ||
        soerr != 0) {
      /* ctx->timed_out (_Atomic; plain read is already race-free) is
       * checked first: the deadline sweep's shutdown(fd, SHUT_RDWR) against
       * a stuck connect is exactly what surfaces here as a getsockopt/
       * SO_ERROR failure, and without this check a connect_timeout_ms
       * expiry was always misreported as ccol_http_connection_failed
       * instead of the ccol_timed_out chttpclient.h documents. */
      _async_fulfill(
          ctx, ctx->timed_out ? ccol_timed_out : ccol_http_connection_failed,
          NULL);
      _async_ctx_finish(ctx);
      return;
    }
    if (!ctx->is_unix) _apply_tcp_nodelay(ctx->fd);

    if (ctx->is_https) {
      /* Configured cert/key/ca path(s) not being readable is deferred all
       * the way to here (mirroring Tier 1's own _rebuild_tls_ctx_locked
       * comment) rather than failing at chttpclient_set_tls time; but that
       * deferred failure is checked synchronously in
       * _chttp_do_async_internal (tls_ctx_usable), before any connection is
       * even opened, so reaching here means TLS is genuinely usable. */
      ctx->tls = ctls_conn_create_client(ctx->chain->tls_ctx, ctx->fd,
                                         ctx->host, ctx->verify_host, NULL);
      if (!ctx->tls) {
        _async_fulfill(ctx, ccol_not_enough_memory, NULL);
        _async_ctx_finish(ctx);
        return;
      }
      ctx->state = CHTTP_ASYNC_TLS_HANDSHAKING;
      _async_tls_advance(ctx);
      return;
    }

    ctx->state = CHTTP_ASYNC_WRITING;
    /* Falls through to the WRITING branch below to attempt the first write
     * immediately (the fd is already known writable right now), rather than
     * waiting for a separate on_writable dispatch. */
  }

  if (ctx->state == CHTTP_ASYNC_TLS_HANDSHAKING) {
    _async_tls_advance(ctx);
    return;
  }

  if (ctx->state == CHTTP_ASYNC_WRITING) {
    if (ctx->tls) {
      _async_tls_try_write(ctx);
      return;
    }
    _async_plain_try_write(ctx);
    return;
  }
  if (ctx->state == CHTTP_ASYNC_AWAITING_CONTINUE) {
    /* Only reachable via the deadline sweep's own continue_deadline expiry
     * check, which flips this registration to write direction specifically
     * to deliver this dispatch (see the "ASYNC DEADLINE SWEEP" section);
     * there is nothing of ours ever pending to flush on this direction
     * otherwise while awaiting continue, unlike the ordinary WRITING state
     * above. Claim the decision first, exactly like every exit from
     * _async_awaiting_continue_on_data; a genuine "100 Continue" that
     * arrived and was already claimed by a racing on_readable dispatch
     * just before this one runs (event_loop's own per-entry dispatch_lock
     * orders the two, but does not decide which one wins) means that
     * dispatch has already taken over sending the body, and this one has
     * nothing left to do. */
    if (atomic_exchange(&ctx->continue_decided, true)) return;
    /* Timed out with no answer either way: send the body anyway, matching
     * curl's own CURLOPT_EXPECT_100_TIMEOUT_MS behaviour and Tier 1's
     * identical timeout branch in _chttp_send_and_read. ctx->retry_unsafe
     * deliberately stays false here (see its own field comment): unlike a
     * confirmed "100 Continue", a bare timeout gives no evidence the peer
     * is even still alive, so the usual reused-connection retry-once
     * safety net still applies if this write (or the read that follows it)
     * discovers the connection was already dead. */
    /* See ctx->interim_responses_seen's own field comment: Tier 1's
     * identical timeout branch hands the final-response read to a fresh
     * _chttp_read_message_loop call with its own independent counter
     * regardless of how the continue-wait ended, so this must reset here
     * too, not just on a confirmed "100 Continue", or a hop that discarded
     * interim responses while waiting would start its final-response read
     * with less than the documented 64-response budget left. */
    ctx->interim_responses_seen = 0;
    ctx->state = CHTTP_ASYNC_WRITING;
    if (ctx->tls) {
      _async_tls_try_write(ctx);
    } else {
      _async_plain_try_write(ctx);
    }
    return;
  }
  /* CHTTP_ASYNC_READING: nothing of ours is pending to flush; a stray
   * on_writable here is a no-op. */
}

/*
 * event_loop's own public callback shape (event_writable_fn/
 * event_readable_fn/event_error_fn all take `void *arg`, the value handed
 * to event_loop_add); these thin wrappers exist purely to cast `arg` back
 * to chttp_async_ctx_t* before handing off to the real logic in
 * _async_on_writable_impl/_on_readable_impl/_on_error_impl below.
 *
 * No pinning/refcounting of any kind happens here (an earlier version of
 * this design had a per-ctx atomic refcount, pinned here via
 * _async_ctx_pin; both were removed once the underlying use-after-free was
 * closed by construction instead; see pending_app_teardown's own field
 * comment for the full rationale). Safety now rests entirely on "no
 * application thread ever frees a registered ctx"; the only thing that can
 * ever call the real, destructive teardown for a ctx these wrappers are
 * dispatched for is a dispatch callback itself, and event_loop's own
 * entry->dispatch_lock plus entry->refcount-gated "one job in flight per
 * entry" invariant (cthreadcomm.c) together guarantee that happens at most
 * once per ctx, with no other thread able to be touching ctx at the same
 * time.
 */
static void _async_on_writable(event_loop loop, ccol_selectable *sel,
                               void *arg) {
  _async_on_writable_impl(loop, sel, (chttp_async_ctx_t *)arg);
}

/*
 * Parses data[0..data_len) as one or more CHTTP_ASYNC_READING-state
 * messages against ctx->parser, exactly as a normal on_readable dispatch
 * would. Shared by _async_on_readable_impl's own READING-state read (the
 * ordinary case, data fresh off the socket) and _async_plain_try_write/
 * _async_tls_try_write's post-body-write continue_carry replay (data
 * already read earlier, during the AWAITING_CONTINUE wait, and stashed
 * because the body write it arrived alongside had not finished yet); both
 * callers hand this function a message boundary they have never fed to
 * ctx->parser before, so its own internal looping/reset behaviour needs no
 * caller-visible distinction between the two origins.
 *
 * Looping (rather than a single chttp1_parser_execute call) is what lets
 * this discard an interim informational (1xx) response and keep parsing
 * (possibly still within THIS SAME chunk, if a fast server already wrote
 * its next message in the same buffer) instead of misreading the first 1xx
 * it sees as the final response. By the time this function ever runs, any
 * Expect: 100-continue decision has already been made (see
 * _async_awaiting_continue_on_data, which owns that decision and is never
 * reached from here), so every 1xx seen here is discarded unconditionally
 * and only a final (>=200) response (or a redirect, itself always >=200)
 * ends the loop, mirroring Tier 1's own general-path handling in
 * _chttp_read_message_loop. `data`/`data_len` walk forward across
 * iterations as trailing bytes from a discarded message are re-fed to a
 * freshly reinitialised ctx->parser; chttp1_parser_execute's own contract
 * forbids feeding more bytes to an already-CHTTP1_PAUSED instance, so a
 * fresh instance (mirroring how a brand-new hop's own ctx->parser is set up
 * in _async_submit_hop/_async_retry_hop) is required for each subsequent
 * message, exactly like Tier 1's own "fresh parser per message" convention.
 */
static void _async_process_reading_data(chttp_async_ctx_t *ctx,
                                        const char *data, size_t data_len) {
  for (;;) {
    chttp1_errno_t err = chttp1_parser_execute(&ctx->parser, data, data_len);
    if (err == CHTTP1_PAUSED) {
      /* Message complete; the parser intentionally pauses right after,
       * exactly like Tier 1's own CHTTP1_PAUSED handling. */
      size_t consumed = chttp1_parser_consumed(&ctx->parser);
      bool trailing_this_msg = consumed < data_len;

      if (ctx->pctx.status_code >= 100 && ctx->pctx.status_code < 200) {
        /* An interim informational response (e.g. "103 Early Hints", RFC
         * 8297) arriving ahead of the real response; discard it and keep
         * reading on this same connection, mirroring Tier 1's own
         * general-path handling in _chttp_read_message_loop. Never
         * terminal: ctx->hop_completed stays false, and neither
         * _async_handle_redirect nor a fulfil/teardown runs here.
         *
         * Bounded, exactly like Tier 1's identical loop (see
         * CHTTP_MAX_INTERIM_RESPONSES's own comment): a server that never
         * stops sending interim responses would otherwise keep this ctx,
         * and the chain/future waiting on it, alive indefinitely. */
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
        call_once(client_http1_settings_bundler.once, _init_chttp1_settings);
        chttp1_parser_init(&ctx->parser,
                           &client_http1_settings_bundler.settings);
        ctx->parser.data = &ctx->pctx;
        if (!trailing_this_msg) return; /* wait for more via on_readable */
        data += consumed;
        data_len -= consumed;
        continue; /* re-parse the remaining bytes as the next message */
      }

      /* A final (>=200) response, or a redirect (always >=200 by
       * definition: 3xx). */
      ctx->hop_completed = true;
      if (trailing_this_msg) ctx->pctx.trailing_garbage = true;
      bool keep_alive =
          chttp1_should_keep_alive(&ctx->parser) && !ctx->pctx.trailing_garbage;

      if (ctx->pctx.will_redirect) {
        _async_handle_redirect(ctx,
                               keep_alive); /* closes/pools the connection */
      } else {
        /* Capture chain (with a temporary extra retain; see
         * _async_handle_redirect's identical one for why: _async_finish_
         * connection below can trigger a concurrent teardown of THIS ctx's
         * own chain reference on another reactor thread, which could free
         * chain before the _async_fulfill_chain call below runs if nothing
         * else were holding it) and build the response BEFORE calling
         * _async_finish_connection: when keep_alive is true, that call can
         * successfully offer ctx to the idle pool, which resets ctx->chain
         * to NULL and ctx->pctx/ctx->bb for reuse (see
         * _async_idle_pool_offer) as part of a normal, expected, successful
         * outcome; so both ctx->chain and ctx->pctx/ctx->bb must be
         * captured/extracted first, or a NULL ctx->chain would crash the
         * fulfil below (or the response would be built from already-cleared
         * fields). Finishing the connection before fulfilling also matters
         * independently: fulfilling can unblock the caller immediately
         * (e.g. a ctpool_future_get on another thread), and if that caller
         * then destroys cli, _async_finish_connection's idle-pool-offer
         * path touching cli->lock afterward would race a use-after-free. */
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
    /* else: message not yet complete, wait for more on_readable */
    return;
  }
}

/*
 * Handles data[0..data_len) arriving while ctx->state ==
 * CHTTP_ASYNC_AWAITING_CONTINUE (the header block has been sent; the body
 * is being deliberately withheld pending the server's answer). Mirrors
 * Tier 1's _chttp_send_and_read's own post-header wait exactly (see that
 * function's own doc comment for the full three-outcome contract), adapted
 * to this module's event-driven dispatch: this function only ever runs
 * from _async_on_readable_impl (a genuine byte arrived), never from the
 * deadline-sweep-triggered timeout path (see _async_on_writable_impl's own
 * CHTTP_ASYNC_AWAITING_CONTINUE branch for that one).
 *
 * ctx->continue_decided (an _Atomic bool) is the single guard deciding
 * which of this function and the timeout path actually gets to act; see
 * that field's own comment for the full race analysis. Every exit from
 * this function that would otherwise act on the parsed message claims it
 * first via atomic_exchange and backs off silently if it discovers the
 * decision was already made (by the timeout path racing ahead of a
 * genuinely late-arriving response).
 */
static void _async_awaiting_continue_on_data(chttp_async_ctx_t *ctx,
                                             const char *data,
                                             size_t data_len) {
  for (;;) {
    chttp1_errno_t err = chttp1_parser_execute(&ctx->parser, data, data_len);
    if (err == CHTTP1_PAUSED) {
      size_t consumed = chttp1_parser_consumed(&ctx->parser);
      bool trailing_this_msg = consumed < data_len;

      if (ctx->pctx.status_code == 100) {
        /* The expected outcome: the server confirms it wants the body.
         * Claim the decision first; if the timeout path already claimed it
         * (this "100 Continue" simply arrived too late to matter), there is
         * nothing left to do - that path has already taken over sending the
         * body. */
        if (atomic_exchange(&ctx->continue_decided, true)) return;

        ccol_retval_t rrv = _parse_ctx_reset_for_continue(&ctx->pctx);
        if (rrv != ccol_success) {
          ctx->hop_completed = true;
          _async_fulfill(ctx, ccol_not_enough_memory, NULL);
          _async_ctx_finish(ctx);
          return;
        }
        call_once(client_http1_settings_bundler.once, _init_chttp1_settings);
        chttp1_parser_init(&ctx->parser,
                           &client_http1_settings_bundler.settings);
        ctx->parser.data = &ctx->pctx;
        /* The body is now being handed to a connection the server itself
         * just confirmed it is alive and ready to read from; see
         * ctx->retry_unsafe's own field comment for why this disqualifies
         * the usual reused-connection retry-once safety net regardless of
         * what happens next. */
        ctx->retry_unsafe = true;
        /* A genuine "100 Continue" is itself a non-discarded message, so the
         * run of CONSECUTIVE discarded interim responses this field tracks
         * restarts here; see ctx->interim_responses_seen's own field comment
         * for why leaving it un-reset would let interim responses discarded
         * during THIS wait silently eat into the final-response read's own
         * budget, unlike Tier 1's _chttp_send_and_read, which hands the two
         * phases to separate _chttp_read_message_loop calls with
         * independent counters. */
        ctx->interim_responses_seen = 0;

        if (trailing_this_msg) {
          /* A fast/optimistic server already wrote (at least the start of)
           * its real final response in the same read as the "100 Continue"
           * line, before the body it hasn't received yet; see
           * ctx->continue_carry's own field comment for why these bytes
           * cannot simply be re-parsed inline right here (the body write
           * below may not complete synchronously) and must be stashed
           * instead. */
          ctx->continue_carry =
              (char *)_mem_alloc(ctx->mp, data_len - consumed);
          if (!ctx->continue_carry) {
            ctx->hop_completed = true;
            _async_fulfill(ctx, ccol_not_enough_memory, NULL);
            _async_ctx_finish(ctx);
            return;
          }
          memcpy(ctx->continue_carry, data + consumed, data_len - consumed);
          ctx->continue_carry_len = data_len - consumed;
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
        /* Some OTHER interim response (e.g. "103 Early Hints") arriving
         * ahead of either "100 Continue" or a direct final answer; discard
         * and keep waiting within the same continue_deadline window,
         * mirroring Tier 1's _chttp_read_message_loop(stop_at_status=100),
         * which discards anything that is not the awaited status too. */
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
        call_once(client_http1_settings_bundler.once, _init_chttp1_settings);
        chttp1_parser_init(&ctx->parser,
                           &client_http1_settings_bundler.settings);
        ctx->parser.data = &ctx->pctx;
        if (!trailing_this_msg) return; /* wait for more via on_readable */
        data += consumed;
        data_len -= consumed;
        continue;
      }

      /* A final (>=200) response arrived directly, without ever seeing a
       * "100 Continue" first; RFC 7231 SS5.1.1 fully permits a server to
       * reject a request this way without ever wanting the body. This
       * response IS the final answer and the body must never be sent.
       * Never keep-alive eligible regardless of what chttp1_should_keep_
       * alive/the response's own Connection header say: see
       * _chttp_send_and_read's identical comment (Tier 1's own counterpart
       * of this exact branch) for why pooling a connection the peer may
       * still be expecting a withheld body on would desync the next,
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
    /* else: message not yet complete (e.g. mid status/header line); wait
     * for more via on_readable, still within continue_deadline. */
    return;
  }
}

static void _async_on_readable_impl(event_loop loop, ccol_selectable *sel,
                                    chttp_async_ctx_t *ctx) {
  (void)loop;
  (void)sel;

  /* This hop already reached a terminal outcome (fulfilled (including a
   * TLS handshake failure detected mid-handshake) or handed off to a
   * redirect hop) via a previous dispatch for this same ctx; a spurious
   * extra on_readable can still arrive afterward (e.g. every route in the
   * test server's mock sends "Connection: close" and closes its end right
   * after writing the response, so the peer's EOF can be observed in a
   * SEPARATE dispatch from the one that already consumed the response
   * bytes and hit CHTTP1_PAUSED). Re-running the completion logic would be
   * harmless for a plain fulfill (guarded by chain->fulfilled) or a repeat
   * ctls_conn_handshake_step call on an already-failed handshake (which
   * just returns another, ignored, error), but _async_handle_redirect is
   * NOT idempotent; it unconditionally queues another hop every time it
   * runs, so without this guard a single hop could fan out into multiple
   * redirect chains sharing the same chain state, corrupting its refcount
   * bookkeeping. Checked before the TLS_HANDSHAKING branch too, since a
   * handshake failure detected by one dispatch must stop a second, racing
   * dispatch from re-entering _async_tls_advance. event_loop's own
   * per-registration dispatch_lock already serialises every dispatch for
   * this ctx's single registration against itself, so a plain bool is
   * sufficient here; no additional locking needed. */
  if (ctx->hop_completed) {
    /* See _async_ctx_handle_if_abandoned's own comment: this is the
     * PRIMARY, reachable path a mechanism-2 deferred teardown (see
     * pending_app_teardown's field comment) is actually reaped through,
     * since shutdown(fd, SHUT_RDWR) dispatches to exactly this function
     * (or _async_on_error_impl) before _async_dispatch_kind is ever
     * consulted below. */
    _async_ctx_handle_if_abandoned(ctx);
    return;
  }
  /* See ctx->reg's own field comment for why this can legitimately be NULL
   * on a very early dispatch, and why simply returning is always safe. */
  if (!ctx->reg) return;

  chttp_async_dispatch_kind_t kind = _async_dispatch_kind(ctx);
  if (kind == CHTTP_ASYNC_DISPATCH_ABANDONED) {
    /* A concurrent staleness-eviction (or any other hop_completed
     * transition) raced this exact dispatch between the hop_completed
     * check above and _async_dispatch_kind's own lock acquisition; see
     * that function's own comment. This is the narrower race window
     * mechanism 2 can also be reaped through, in addition to the
     * top-of-function check above; _async_ctx_handle_if_abandoned is a
     * no-op if some other, earlier dispatch already handled it. */
    _async_ctx_handle_if_abandoned(ctx);
    return;
  }
  if (kind == CHTTP_ASYNC_DISPATCH_IDLE) {
    /* With N reactor threads all calling epoll_wait on one shared epoll
     * instance (no EPOLLEXCLUSIVE-style dedup), a single underlying
     * readiness event can legitimately produce more than one sequential
     * dispatch for the same fd: e.g. a response that arrives in two TCP
     * segments can have both segments' readiness independently observed by
     * two different reactor threads' own epoll_wait calls before either has
     * had a chance to actually drain the socket, so the first dispatch
     * fully reads and processes the response (transitioning this ctx to
     * IDLE and pooling it) and a second, already-in-flight dispatch for the
     * very same original event still follows afterward. With N reactor
     * threads able to dispatch concurrently, a bare dispatch here is NOT
     * sufficient evidence that the connection is actually dead or has
     * genuinely unexpected data; an actual non-blocking read is required
     * to tell a stale, already-handled readiness apart from real activity.
     * Found via targeted stderr tracing after a real, reproducible failure
     * in async_idle_pool.sequential_requests_reuse_connection (pooled
     * connections were being spuriously evicted moments after being
     * offered), not assumed from code inspection alone. */
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
      /* Nothing actually available: this was a stale/duplicate dispatch for
       * an event already fully handled by another thread. The connection
       * remains genuinely idle and pooled; nothing to do. */
      return;
    }
    /* pn == 0 (peer closed), pn > 0 (unexpected data), or a hard error: the
     * connection is genuinely no longer safely reusable. */
    _async_idle_ctx_finish(ctx);
    return;
  }

  if (ctx->state == CHTTP_ASYNC_TLS_HANDSHAKING) {
    _async_tls_advance(ctx);
    return;
  }

  char buf[8192];
  ssize_t n = 0;
  bool eof = false;
  bool hard_error = false;
  bool injected = false;

#ifdef RUNNING_UNIT_TESTS
  /* See g_force_async_hard_read_error_for_tests's own comment (above the
   * "ASYNC IDLE POOL" section) for why this exists and why it only takes
   * effect once ctx->any_bytes_read is already true. */
  if (ctx->any_bytes_read &&
      atomic_exchange(&g_force_async_hard_read_error_for_tests, false)) {
    hard_error = true;
    injected = true;
  }
#endif

  if (!injected) {
    if (ctx->tls) {
      n = ctls_conn_read(ctx->tls, buf, sizeof(buf));
      if (n < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) return;
        /* A genuine TLS/transport-level error (SSL_ERROR_SYSCALL/
         * SSL_ERROR_SSL, mapped by ctls.c's _ctls_classify_io_result to
         * errno == ECONNRESET), NOT a graceful close_notify; ctls_conn_read
         * reports THAT as n == 0, exactly like a plain socket EOF, in the
         * `else` branch below. Must never be folded into `eof`: see
         * hard_error's own handling a few lines down for why. */
        hard_error = true;
      } else {
        eof = (n == 0);
      }
    } else {
      do {
        n = recv(ctx->fd, buf, sizeof(buf), 0);
      } while (n < 0 && errno == EINTR);
      if (n < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) return;
        /* Same distinction as the TLS branch above: recv() returning -1 with
         * a real errno (ECONNRESET, ENOTCONN, ...) is a hard transport
         * error, never the orderly FIN-driven close recv() reports as
         * n == 0. */
        hard_error = true;
      } else {
        eof = (n == 0);
      }
    }
  }
  if (n > 0) ctx->any_bytes_read = true;

  if (hard_error) {
    /* Deliberately never reaches chttp1_parser_finish below: unlike a real
     * EOF, a hard error gives no guarantee the peer sent everything it
     * meant to, and chttp1_parser_finish would happily report a complete,
     * successful EOF-delimited body if the parser happened to already be
     * sitting in CHTTP1_ST_BODY_EOF (a response with neither Content-Length
     * nor chunked Transfer-Encoding); silently truncating the response
     * instead of reporting the failure. A connection RESET mid-transfer
     * (or a TLS-level fatal alert) must not be indistinguishable from the
     * peer finishing normally. Mirrors Tier 1's own _chttp_read_message
     * (returns ccol_http_transfer_aborted immediately for any read error
     * other than EWOULDBLOCK, never calling chttp1_parser_finish) and this
     * same function's _async_on_error_impl sibling, which already gets a
     * genuine EPOLLERR dispatch right; only this function's own read-result
     * classification had collapsed the eof-vs-error distinction into one.
     * _async_ctx_finish itself checks reused/any_bytes_read and retries if
     * eligible; no pre-emptive fulfil here (see its own comment). */
    _async_ctx_finish(ctx);
    return;
  }

  if (eof) {
    /* Mirrors Tier 1's own n==0/EOF handling in _chttp_read_response: a
     * clean chttp1_parser_finish with a complete message is valid for
     * responses that signal their end via connection-close rather than
     * Content-Length/chunked framing. A reused connection that produces
     * this before any response byte came back is Tier 1's retry-once
     * scenario, checked BEFORE marking hop_completed/fulfilling; the whole
     * point is that nothing has failed for the caller yet.
     *
     * fe == CHTTP1_PAUSED (not just CHTTP1_OK) is the EXPECTED outcome for a
     * valid EOF-delimited body; see chttp1_parser_finish's own doc comment
     * and Tier 1's identical comment in _chttp_read_response. */
    chttp1_errno_t fe = chttp1_parser_finish(&ctx->parser);
    bool ok = ((fe == CHTTP1_OK || fe == CHTTP1_PAUSED) &&
               ctx->pctx.message_complete);
    if (!ok) {
      /* _async_ctx_finish itself checks reused/any_bytes_read and retries
       * if eligible; no pre-emptive fulfil here (see its own comment). */
      _async_ctx_finish(ctx);
      return;
    }
    if (ctx->state == CHTTP_ASYNC_AWAITING_CONTINUE) {
      /* An EOF-delimited (no Content-Length/chunked) final response
       * arriving directly, without ever seeing a "100 Continue" first;
       * this is still Tier 1's "server answered directly, body never sent"
       * outcome (see _chttp_send_and_read's identical comment), just
       * signalled via connection-close framing instead of a length header.
       * Claim the decision first, exactly like every other exit from
       * _async_awaiting_continue_on_data; back off silently if the
       * deadline-sweep timeout path already claimed it. Never reusable
       * either way: this eof path never pools its connection regardless
       * (see _async_fulfill_success's own comment), so no extra keep_alive
       * override is needed here beyond that already-existing behaviour. */
      if (atomic_exchange(&ctx->continue_decided, true)) return;
    }
    ctx->hop_completed = true;
    if (ctx->pctx.will_redirect) {
      /* Peer-closed (rather than Content-Length/chunked) framing is never
       * keep-alive eligible; matches Tier 1's _chttp_read_response, which
       * hard-codes *keep_alive_out = false for this exact case. */
      _async_handle_redirect(ctx, false);
    } else {
      _async_fulfill_success(ctx);
      _async_ctx_finish(ctx);
    }
    return;
  }

  if (ctx->state == CHTTP_ASYNC_AWAITING_CONTINUE) {
    _async_awaiting_continue_on_data(ctx, buf, (size_t)n);
    return;
  }
  _async_process_reading_data(ctx, buf, (size_t)n);
}

static void _async_on_readable(event_loop loop, ccol_selectable *sel,
                               void *arg) {
  /* See _async_on_writable's identical comment above. */
  _async_on_readable_impl(loop, sel, (chttp_async_ctx_t *)arg);
}

static void _async_on_error_impl(event_loop loop, ccol_selectable *sel,
                                 chttp_async_ctx_t *ctx) {
  (void)loop;
  (void)sel;
  if (ctx->hop_completed) {
    /* See _async_ctx_handle_if_abandoned's own comment: this is the
     * PRIMARY, reachable path a mechanism-2 deferred teardown (see
     * pending_app_teardown's field comment) is actually reaped through,
     * since shutdown(fd, SHUT_RDWR) dispatches to exactly this function
     * (or _async_on_readable_impl) before _async_dispatch_kind is ever
     * consulted below. */
    _async_ctx_handle_if_abandoned(ctx);
    return;
  }
  /* See ctx->reg's own field comment for why this can legitimately be NULL
   * on a very early dispatch (the fd erroring out essentially immediately
   * after being registered), and why simply returning is always safe: an
   * fd-level error condition is persistent at the OS level, so a later
   * dispatch (once ctx->reg is visible) will observe the same error. */
  if (!ctx->reg) return;
  chttp_async_dispatch_kind_t kind = _async_dispatch_kind(ctx);
  if (kind == CHTTP_ASYNC_DISPATCH_ABANDONED) {
    /* See _async_on_readable_impl's identical check; narrower race window
     * mechanism 2 can also be reaped through. */
    _async_ctx_handle_if_abandoned(ctx);
    return;
  }
  if (kind == CHTTP_ASYNC_DISPATCH_IDLE) {
    _async_idle_ctx_finish(ctx);
    return;
  }
  /* cthreadcomm.c's write-direction dispatch policy always prioritizes an
   * EPOLLERR/EPOLLHUP bit over EPOLLOUT (is_error = is_err, is_writable =
   * !is_err; unlike the read direction, there is no has_writer-style carve
   * out), so a failed non-blocking connect() (the ordinary case for any
   * non-loopback host, where the kernel reports EPOLLOUT|EPOLLERR|EPOLLHUP
   * together once the RST/timeout arrives) dispatches HERE, never to
   * _async_on_writable_impl's own CHTTP_ASYNC_CONNECTING branch (which
   * diagnoses the failure via getsockopt(SO_ERROR)); the same applies to a
   * TLS handshake killed by an RST mid-handshake, which never reaches
   * _async_tls_advance's CTLS_HANDSHAKE_ERROR branch (ctls_conn_
   * verify_result-based diagnosis) either. Without the two checks below,
   * both cases fell through to the generic backstop
   * ccol_http_transfer_aborted instead of the specific, documented
   * ccol_http_connection_failed/ccol_http_tls_handshake_failed/
   * ccol_http_tls_cert_verification_failed chttpclient.h promises "once the
   * request is actually in flight". Neither state is ever reached by a
   * reused ctx (a pooled connection re-enters at CHTTP_ASYNC_WRITING, never
   * CONNECTING/TLS_HANDSHAKING), so fulfilling directly here, exactly like
   * the sibling diagnosis sites do, can never suppress a legitimate
   * reused-connection retry. */
  /* ctx->timed_out (_Atomic; plain read is already race-free) is checked
   * first in both branches below: the deadline sweep's shutdown(fd,
   * SHUT_RDWR) against a stuck connect/handshake dispatches here exactly
   * like a genuine peer-side failure would, and without this check a
   * connect_timeout_ms/request_timeout_ms expiry was always misreported as
   * ccol_http_connection_failed/ccol_http_tls_handshake_failed/
   * ccol_http_tls_cert_verification_failed instead of the ccol_timed_out
   * chttpclient.h documents. */
  if (ctx->state == CHTTP_ASYNC_CONNECTING) {
    _async_fulfill(
        ctx, ctx->timed_out ? ccol_timed_out : ccol_http_connection_failed,
        NULL);
    _async_ctx_finish(ctx);
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
  /* An fd-level error with no more specific diagnosis available; rely on
   * _async_ctx_finish's own retry-check-then-backstop logic exactly like
   * the analogous cases above, rather than pre-emptively fulfilling and
   * accidentally disabling a legitimate reused-connection retry. */
  _async_ctx_finish(ctx);
}

static void _async_on_error(event_loop loop, ccol_selectable *sel, void *arg) {
  /* See _async_on_writable's identical comment above. */
  _async_on_error_impl(loop, sel, (chttp_async_ctx_t *)arg);
}

/*
 * Runs on a cli_engine_bundler.dns_pool worker: resolves DNS (TCP) or builds
 * the sockaddr_un (unix), then issues ONE non-blocking connect() and registers
 * the resulting fd with the reactor for write-readiness; never blocks the
 * worker waiting for the connect to actually complete. This division of
 * labor (synchronous DNS resolution plus a single non-blocking connect
 * attempt, on an offload thread, rather than blocking the worker through the
 * entire connect) is deliberate: tying up this fixed-size worker pool for an
 * entire connect's duration would regress throughput under high concurrency,
 * whereas registering for write-readiness lets a single epoll instance
 * cheaply track thousands of pending connects. For TCP, only the first
 * candidate address that either connects immediately or returns EINPROGRESS
 * is used; a candidate that fails outright falls through to the next
 * address. Never touches the caller's thread.
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
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
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
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)ctx->port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(ctx->host, port_str, &hints, &res) != 0 || !res) {
      _async_fulfill(ctx, ccol_http_host_resolution_failed, NULL);
      _async_ctx_teardown(ctx);
      return;
    }
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
      int cand = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK,
                        ai->ai_protocol);
      if (cand < 0) continue;
      if (connect(cand, ai->ai_addr, ai->ai_addrlen) == 0 ||
          errno == EINPROGRESS) {
        fd = cand;
        break;
      }
      close(cand);
    }
    freeaddrinfo(res);
    if (fd < 0) {
      _async_fulfill(ctx, ccol_http_connection_failed, NULL);
      _async_ctx_teardown(ctx);
      return;
    }
  }

  /* ctx->fd/state are _Atomic (see their own field comments), so these
   * plain assignments are already race-free against the deadline sweep; an
   * independent thread, registered against this ctx since before it was
   * ever submitted here (see _async_submit_hop), which can run at any time,
   * including concurrently with this exact assignment. This also covers the
   * case where connect_timeout_ms already expired while this task merely
   * sat queued on cli_engine_bundler.dns_pool (e.g. a saturated pool): the
   * sweep cannot shut down an fd that doesn't exist yet, so on that path it can
   * only mark ctx->timed_out and wait; checked here, the moment a real fd
   * finally exists. */
  ctx->fd = fd;
  ctx->state = CHTTP_ASYNC_CONNECTING;

  if (ctx->timed_out) {
    /* Never registered with the reactor, so nothing else could act on this
     * fd yet; report the timeout ourselves and let _async_ctx_teardown (via
     * _async_ctx_destroy_now) close fd for us. Closing it manually here,
     * before _client_deadline_unregister has run, was a real violation of
     * this module's own "unregister before close" invariant (see
     * _async_ctx_destroy_now's own doc comment: this ctx is already linked
     * into the deadline registry by this point, registered before this
     * task was ever submitted). _async_ctx_destroy_now already closes
     * ctx->fd itself, unconditionally, AFTER unregistering - closing it a
     * second time here would only have been safe because it was set to -1
     * immediately after, making the later close a no-op; removing the
     * manual close entirely is both simpler and correct. */
    _async_fulfill(ctx, ccol_timed_out, NULL);
    _async_ctx_teardown(ctx);
    return;
  }

  char *err = NULL;
  event_handlers_t handlers = {
      .on_readable = _async_on_readable,
      .on_writable = _async_on_writable,
      .on_error = _async_on_error,
  };
  /* idle_lock, held across both the event_loop_add call and this thread's
   * own read-back of ctx->reg right after: the moment event_loop_add makes
   * this registration live, a reactor thread is free to dispatch it (a
   * loopback connect is very often already writable immediately), drive the
   * whole hop to completion, and reach _async_ctx_free; racing this
   * thread's own still-in-flight "ctx->reg = ..." write and the "if
   * (!ctx->reg)" read right after it, a genuine write/read race on ctx->reg
   * itself (caught by ThreadSanitizer, not by inspection). _async_ctx_free
   * takes the same lock, briefly, as its very first action, which is enough
   * to guarantee this window has fully completed (ctx->reg published, and
   * this function has stopped touching ctx) before free() can proceed; no
   * dispatch path needs idle_lock to observe ctx->reg itself (see its own
   * field comment: a NULL read there is already handled as a harmless,
   * self-healing no-op), so this adds no new contention on that side. */
  mutex_lock(ctx->idle_lock);
  ctx->reg = event_loop_add(cli_engine_bundler.reactor,
                            selectable_from_fd(fd, ccol_select_write), handlers,
                            ctx, &err);
  bool reg_failed = !ctx->reg;
  mutex_unlock(ctx->idle_lock);
  if (reg_failed) {
    _async_fulfill(ctx, ccol_not_enough_memory, NULL);
    _async_ctx_teardown(ctx);
  }
}

/*
 * Called when a REUSED connection turns out to be dead before any response
 * byte was read (write failure, immediate EOF, or an error detected by the
 * reactor); mirrors Tier 1's identical "retry exactly once against a
 * brand-new connection" liveness-probe-failure recovery in
 * chttp_do_internal.
 *
 * Rather than reusing old_ctx's own struct in place for the new attempt
 * (which would require neutralising its dispatch callbacks to guard against
 * a dispatch already in flight for the OLD registration: such a dispatch
 * could otherwise arrive and observe ctx fields that have since been
 * repurposed for the NEW attempt; and that neutralisation is itself unsafe
 * if some OTHER, independent teardown was already mid-flight when we tried
 * it), this allocates a fresh ctx and transfers just what the retry needs
 * (the already-serialized wire bytes, host/port, origin_key, the empty response
 * headers map, and hop metadata) out of old_ctx, nulling those fields there
 * so old_ctx's own upcoming normal teardown (via its caller, exactly as if
 * this were an ordinary failure) doesn't double-free them. old_ctx is left
 * otherwise untouched and continues through its NORMAL teardown path
 * afterward; this function does not free it or touch its chain reference;
 * only ctx->hop_completed is set, both to prevent old_ctx's own on_data/
 * on_close from re-triggering this a second time and because, from
 * old_ctx's own perspective, it genuinely has reached a terminal outcome.
 *
 * Retains a SECOND chain reference for the new ctx (old_ctx keeps its own
 * until its own teardown releases it); retaining before old_ctx's release
 * can possibly happen guarantees the chain's refcount never dips to zero
 * prematurely during the handoff, exactly like a redirect hop's own
 * retain-before-release ordering.
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

  /* ctx is a brand-new, still-private object here (not yet registered with
   * the deadline registry or event_loop), so no concurrent reader could
   * possibly race this write regardless; deadline_lock is still used, for
   * uniformity with every other write site (see its own field comment). */
  mutex_lock(ctx->deadline_lock);
  ctx->overall_deadline = chain->overall_deadline;
  mutex_unlock(ctx->deadline_lock);
  ctx->chain = chain;
  ctx->cli = old_ctx->cli;
  ctx->hop = old_ctx->hop;
  ctx->cur_method = old_ctx->cur_method;
  ctx->is_https = old_ctx->is_https;
  ctx->verify_host = old_ctx->verify_host;

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
  /* The retry resends the exact same request, so it carries forward the
   * exact same want_100_continue/header_len decision old_ctx already made
   * (mirroring how cur_method/cur_body are carried forward elsewhere in
   * this file's redirect machinery); reaching this function at all
   * guarantees old_ctx never got past sending the header block (see
   * _async_ctx_finish's retry-eligibility check, which requires
   * !retry_unsafe - retry_unsafe only ever becomes true once the body has
   * actually been sent following a confirmed "100 Continue"), so ctx->wire
   * still holds the complete, unsent body ready to go. continue_decided
   * starts false again for this fresh attempt, exactly like a brand-new
   * hop. */
  ctx->want_100_continue = old_ctx->want_100_continue;
  ctx->header_len = old_ctx->header_len;
  atomic_store(&ctx->continue_decided, false);

  ctx->pctx.mp = chain->mp;
  ctx->pctx.is_head_request = old_ctx->pctx.is_head_request;
  /* Streaming (chain->write_fn set) delivers body bytes straight to the
   * caller's callback; buffered uses ctx->bb; see _async_build_response's
   * own comment for why this must be consistent with what it later does. */
  ctx->pctx.requested_sink_fn =
      chain->write_fn ? chain->write_fn : _sink_buffered;
  ctx->pctx.requested_sink_ctx = chain->write_fn ? chain->write_ctx : &ctx->bb;
  ctx->pctx.headers = old_ctx->pctx.headers; /* empty; nothing was ever
                                              * parsed into it, since retry
                                              * requires !any_bytes_read */
  old_ctx->pctx.headers = NULL;
  ctx->bb.mp = chain->mp;
  ctx->bb.max_size = chain->max_response_body_size;

  ctx->reused = false; /* the retry itself is a fresh connection */
  ctx->any_bytes_read = false;
  /* A retry always connects fresh (see above), so it needs its own
   * connect_deadline exactly like _async_submit_hop's fresh path; the
   * chain's overall_deadline is unaffected (it was never per-hop) and
   * continues to apply unchanged across the retry. */
  ctx->connect_deadline = _deadline_make(chain->connect_timeout_ms);

  call_once(client_http1_settings_bundler.once, _init_chttp1_settings);
  chttp1_parser_init(&ctx->parser, &client_http1_settings_bundler.settings);
  ctx->parser.data = &ctx->pctx;

  /* Registered BEFORE submitting; see _async_submit_hop's identical
   * comment for why (a worker could otherwise run this hop to completion
   * and free ctx before this thread registers it). */
  _client_deadline_register(ctx);

  ccol_retval_t sr = ctpool_submit(cli_engine_bundler.dns_pool,
                                   _async_connect_task, ctx, NULL);
  if (sr != ccol_success) {
    _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
    _async_ctx_free(ctx); /* unregisters ctx too */
    _async_chain_release(chain);
  }
}

/*
 * Prepares and submits one hop of a redirect chain; used for both the very
 * first hop (from _chttp_do_async_internal) and every subsequent redirect
 * hop (from _async_handle_redirect). Retains one chain reference on success
 * (released when this hop's ctx is eventually torn down); on any failure,
 * that retain is released again internally (net effect: no refcount change)
 * and the chain's future is fulfilled with a specific error code before
 * returning, so callers never need their own fallback fulfil-on-failure
 * logic here; only _async_chain_release's generic backstop remains as a
 * true last resort for paths that can't be reached with a more specific
 * error (e.g. an already-in-flight connection dying).
 *
 * body_data/body_len/body_content_type describe THIS hop's body (usually
 * chain->body_data/len/content_type verbatim, or all-empty once a
 * non-preserving redirect has rewritten the method to GET); passed
 * explicitly rather than always read from chain because the caller (Tier 1
 * loop equivalent) is the one that knows, from the previous hop's status
 * code, whether to preserve or drop them.
 *
 * Returns true if the hop was successfully queued.
 */

/*
 * Handles a setup failure (headers-map allocation, request serialisation, or
 * origin_key allocation) that occurs AFTER a reused connection has already
 * been popped from the idle pool; i.e. ctx->fd is a live, registered
 * connection, not a not-yet-connected fresh ctx, and ctx->idle_lock is still
 * held per _async_idle_pool_take's return contract. Report the error and
 * mark the ctx terminal (both because it already has been, and to stop
 * _async_ctx_finish's retry-check from queuing a pointless retry of what is
 * an allocation failure, not a dead connection).
 *
 * Crucially, this function runs on an ordinary application thread (whatever
 * thread called chttpclient_do_async, or a redirect-driven caller), and
 * ctx's event_loop registration is still fully live at this point; a
 * reactor dispatch callback for it can legitimately be in flight, or about
 * to be invoked, on a different thread at this exact moment. Directly
 * calling _async_ctx_teardown here (an earlier version of this function did
 * exactly that) is therefore a real use-after-free: see
 * pending_app_teardown's own field comment for the full rationale, which
 * this function is one of the two sites that comment describes. Instead:
 * mark pending_app_teardown (together with hop_completed, in that order;
 * see the field comment's own note on why the order does not matter here
 * but is kept consistent anyway), capture ctx->fd into a local, release
 * idle_lock, and shutdown() the fd to force a genuine EPOLLIN/EPOLLERR
 * dispatch on this still-read-registered fd; whichever dispatch callback
 * observes it (see _async_ctx_handle_if_abandoned) performs the real,
 * destructive teardown safely, from dispatch context. Do not touch ctx
 * again after the shutdown() call.
 *
 * A fresh (not yet connected) ctx never had idle_lock locked and has no such
 * attachment to worry about (ctx->reg is still NULL, so no dispatch can be
 * in flight for it), and is simply freed directly, exactly like every other
 * pre-connect failure path.
 */
static void _async_submit_hop_fail(chttp_async_ctx_t *ctx, ccol_retval_t rv) {
  chttp_async_chain_t *chain = ctx->chain;
  _async_fulfill_chain(chain, rv, NULL);
  if (ctx->reused) {
    ctx->pending_app_teardown = true;
    ctx->hop_completed = true;
    int fd = ctx->fd;
    mutex_unlock(ctx->idle_lock);
    /* _client_engine_release() below releases the SEPARATE engine
     * reference _async_idle_pool_offer acquired for this ctx while it sat
     * in the idle pool; _async_ctx_teardown (once the deferred dispatch
     * finally runs it) only ever releases ctx's chain reference and knows
     * nothing about this one. This is a fourth exit from the idle pool
     * this reference must be released on, alongside a successful reuse
     * (below in _async_submit_hop), organic idle-connection death
     * (_async_idle_ctx_finish), and staleness eviction
     * (_async_idle_pool_take); missing it here leaked one engine reference
     * per reused-connection setup failure (an allocation or serialisation
     * failure occurring after a pooled connection was already popped).
     * Safe to call from this thread directly: it only ever touches a
     * global counter, never ctx. */
    _client_engine_release();
    if (fd >= 0) shutdown(fd, SHUT_RDWR);
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
    /* Mirrors Tier 1's identical per-hop check: the configured cert/key/ca
     * path(s) were not readable (or failed to load/parse) at set_tls time;
     * see chttp_do_internal's own identical check for why
     * ccol_http_tls_cert_load_failed is the correct code here. */
    _url_free(chain->mp, &url);
    _async_fulfill_chain(chain, ccol_http_tls_cert_load_failed, NULL);
    _async_chain_release(chain);
    return false;
  }

  /* A caller-set (chttp_request_set_header) Authorization header is dropped,
   * permanently, the first time a hop's origin differs from
   * chain->initial_origin_key (hop 0's origin); mirrors Tier 1's identical
   * initial_origin_key/explicit_auth_suppressed locals in chttp_do_internal
   * exactly, and matches curl's own CVE-2018-1000007-hardened default (see
   * that function's own comment for the full rationale). Safe to mutate
   * chain->explicit_auth_suppressed here without any additional locking, for
   * the same reason chain->carried_auth* below is safe to mutate unguarded
   * (see this function's own next comment). */
  if (chain->initial_origin_key && !chain->explicit_auth_suppressed &&
      strcmp(chain->initial_origin_key, url.origin_key) != 0) {
    chain->explicit_auth_suppressed = true;
  }

  /* Auto-injected-from-userinfo Authorization carry-forward; mirrors Tier
   * 1's identical carried_auth/carried_auth_origin locals in
   * chttp_do_internal exactly (same-origin carry, permanent cross-origin
   * drop). Safe to mutate chain->carried_auth* here without any additional
   * locking: _async_submit_hop runs strictly sequentially per chain (one
   * hop's setup always completes (including this mutation) before the
   * next hop is ever submitted), the same invariant every other unguarded
   * chain-field mutation in this file's redirect machinery already relies
   * on (e.g. the method/body downgrade decision in _async_handle_redirect).
   */
  const char *effective_auth = NULL;
  if (url.userinfo_authorization) {
    effective_auth = url.userinfo_authorization;
  } else if (chain->carried_auth &&
             strcmp(chain->carried_auth_origin, url.origin_key) == 0) {
    effective_auth = chain->carried_auth;
  }
  if (url.userinfo_authorization) {
    char *na = ccol_strdup(chain->mp, url.userinfo_authorization);
    char *no = ccol_strdup(chain->mp, url.origin_key);
    if (!na || !no) {
      _mem_free(chain->mp, na);
      _mem_free(chain->mp, no);
      _url_free(chain->mp, &url);
      _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
      _async_chain_release(chain);
      return false;
    }
    _mem_free(chain->mp, chain->carried_auth);
    _mem_free(chain->mp, chain->carried_auth_origin);
    chain->carried_auth = na;
    chain->carried_auth_origin = no;
  } else if (chain->carried_auth &&
             strcmp(chain->carried_auth_origin, url.origin_key) != 0) {
    _mem_free(chain->mp, chain->carried_auth);
    _mem_free(chain->mp, chain->carried_auth_origin);
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
    /* Only a fresh connection actually goes through CHTTP_ASYNC_CONNECTING/
     * TLS_HANDSHAKING; a reused one skips straight to WRITING below, so
     * connect_deadline is simply never consulted for it (see the "ASYNC
     * DEADLINE SWEEP" section). */
    ctx->connect_deadline = _deadline_make(chain->connect_timeout_ms);
  }
  /* For the reused case, _async_idle_pool_take has already set ctx->chain,
   * ctx->overall_deadline, and ctx->state (to CHTTP_ASYNC_WRITING) under
   * ctx->idle_lock, and returned with that lock STILL HELD; see its own
   * doc comment for why. This function must keep it held for everything
   * below, only releasing it once the write attempt (success or failure)
   * at the bottom of this function has actually happened; ctx is not safe
   * for any concurrent dispatch to touch until then. The re-assignment
   * below is a harmless no-op for the reused case (already set to the
   * same values) and the only assignment for the fresh case; still goes
   * through deadline_lock regardless (see its own field comment), since
   * for the reused case this ctx may already be registered with the
   * deadline sweep from an earlier fresh connect. */
  mutex_lock(ctx->deadline_lock);
  ctx->overall_deadline = chain->overall_deadline;
  mutex_unlock(ctx->deadline_lock);
  ctx->chain = chain;
  ctx->hop = hop;
  ctx->cur_method = method;
  ctx->is_https = url.is_https;
  ctx->is_ipv6 = url.is_ipv6;
  ctx->verify_host = chain->verify_host;
  ctx->reused = reused;
  ctx->any_bytes_read = false;
  ctx->hop_completed = false;
  ctx->interim_responses_seen = 0;

  /* Captured fresh on EVERY hop, even a reused connection: the request path
   * (and, unlike host/port/origin_key, that's true even when the origin
   * doesn't change) can differ from the previous hop that used this same
   * pooled connection. Needed by _async_handle_redirect to resolve a
   * relative-path Location header against THIS hop's own URL, mirroring
   * Tier 1's per-hop chttp_url_t url local; see this field's own struct
   * comment for the crash this fixes. Freed first since a reused ctx may
   * still be carrying the previous owner's copy (idle-pool offer only clears
   * it opportunistically, not as a correctness requirement). */
  _mem_free(chain->mp, ctx->path_and_query);
  ctx->path_and_query = ccol_strdup(chain->mp, url.path_and_query);
  if (!ctx->path_and_query) {
    _url_free(chain->mp, &url);
    _async_submit_hop_fail(ctx, ccol_not_enough_memory);
    return false;
  }

  ctx->pctx.mp = chain->mp;
  ctx->pctx.is_head_request = (method == CHTTP_HEAD);
  /* Streaming (chain->write_fn set) delivers body bytes straight to the
   * caller's callback; buffered uses ctx->bb; see _async_build_response's
   * own comment for why this must be consistent with what it later does. */
  ctx->pctx.requested_sink_fn =
      chain->write_fn ? chain->write_fn : _sink_buffered;
  ctx->pctx.requested_sink_ctx = chain->write_fn ? chain->write_ctx : &ctx->bb;
  ctx->bb.mp = chain->mp;
  ctx->bb.max_size = chain->max_response_body_size;

  char *herr = NULL;
  ctx->pctx.headers =
      chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                        ccol_string, chain->mp, NULL, &herr);
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
  /* Without this, _serialize_request never actually emits "expect:
   * 100-continue" on the wire (it reads req->expect_continue, not anything
   * chain-level), even though ctx->want_100_continue below is computed
   * independently from chain->expect_continue and correctly engages the
   * write-splitting/wait machinery regardless - a real bug caught by
   * async_step_a.expect_continue_field_reaches_the_wire, which found the
   * header count on the wire coming back 0 despite the wait/timeout timing
   * proving the wait itself was genuinely happening. */
  hop_req.expect_continue = chain->expect_continue;

  chttp_header_presence_t hop_hp;
  prv = _serialize_request(chain->mp, &hop_req, &url, effective_auth,
                           chain->explicit_auth_suppressed, &ctx->wire,
                           &ctx->wire_len, &hop_hp);
  if (prv != ccol_success) {
    _url_free(chain->mp, &url);
    _async_submit_hop_fail(ctx, prv);
    return false;
  }
  /* Mirrors Tier 1's identical use_100_continue local in chttp_do_internal
   * exactly (see that function's own comment for why has_explicit_expect
   * must come from _serialize_request's own case-insensitive scan rather
   * than a second, naive lookup); body_carrying_method is recomputed here
   * rather than threaded through from hop_req, since chttp_method_t has no
   * public accessor for it outside _serialize_request's own internal use. */
  bool body_carrying_method_ac =
      (method == CHTTP_POST || method == CHTTP_PUT || method == CHTTP_PATCH);
  ctx->want_100_continue = chain->expect_continue && !hop_hp.has_expect &&
                           body_carrying_method_ac && body_data && body_len > 0;
  ctx->header_len = ctx->want_100_continue ? ctx->wire_len - body_len : 0;
  atomic_store(&ctx->continue_decided, false);

  if (!ctx->origin_key) {
    /* Always set except on the reused path, where it already carries the
     * origin this connection was pooled under (identical to url.origin_key
     * by construction; _async_idle_pool_take only ever returns a
     * connection filed under the exact origin_key being looked up). */
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

  call_once(client_http1_settings_bundler.once, _init_chttp1_settings);
  chttp1_parser_init(&ctx->parser, &client_http1_settings_bundler.settings);
  ctx->parser.data = &ctx->pctx;

  if (reused) {
    /* Already connected (and, if HTTPS, already handshaked); skip
     * CONNECTING/TLS_HANDSHAKING entirely. ctx->state was already moved to
     * CHTTP_ASYNC_WRITING (under idle_lock, together with ctx->chain) above,
     * before any of the other per-hop fields below were touched. The
     * registration's direction must be flipped from read (its steady
     * idle-pooled state) to write; the actual write attempt is deliberately
     * NOT made here, on this (non-reactor) calling thread; it is left
     * entirely to _async_on_writable's own dispatch, exactly like a fresh
     * connection's first write already works. This is not just simpler; it
     * is required for correctness: the moment event_loop_modify flips this
     * registration to write interest, an already-running reactor thread is
     * free to dispatch _async_on_writable for it concurrently with this
     * calling thread (event_loop's own guarantee only prevents two
     * dispatches of the same registration from running concurrently with
     * EACH OTHER; it says nothing about a non-callback caller like this
     * function racing a dispatch it just made possible). An earlier version
     * of this function attempted the write here directly, which both raced
     * that concurrent dispatch and separately failed to transition
     * ctx->state/direction to READING on a fully-completed write; a real,
     * reproducible hang in async_idle_pool.sequential_requests_reuse_
     * connection, found via gdb thread backtraces on the hung process
     * rather than assumed from code inspection alone. */
    ccol_retval_t reactivate_rv;
#ifdef RUNNING_UNIT_TESTS
    if (atomic_exchange(&g_force_reactivate_fail_for_tests, false)) {
      /* Simulated event_loop_modify failure: the real call is deliberately
       * NOT made, so ctx->reg is left exactly as a genuine failure would
       * leave it (still read-registered; see this branch's own comment
       * below on why event_loop_modify never mutates direction on any
       * failure path it can return through). See
       * g_force_reactivate_fail_for_tests's own comment for why the real
       * call can never actually fail here. */
      reactivate_rv = ccol_unexpected_failure;
    } else
#endif
    {
      reactivate_rv = event_loop_modify(cli_engine_bundler.reactor, ctx->reg,
                                        ccol_select_write);
    }
    if (reactivate_rv != ccol_success) {
      /* See pending_app_teardown's own field comment: ctx's event_loop
       * registration is still fully live here (it was just popped from
       * the idle pool for reuse), so a reactor dispatch callback can
       * legitimately be in flight, or about to be invoked, on a
       * different thread at this exact moment. Calling
       * _async_ctx_teardown directly from this (non-dispatch) thread
       * would be a real use-after-free (an earlier version of this
       * function did exactly that, found via ASan); instead, mark ctx
       * terminal (BEFORE calling _async_retry_hop, which itself sets
       * ctx->hop_completed = true as its own very first statement; the
       * write order between the two flags does not matter for
       * correctness today, since _async_on_writable_impl's own
       * top-of-function check cannot structurally be dispatched during
       * this window regardless per that check's own comment, but
       * pending_app_teardown is set first anyway purely so correctness
       * never comes to depend on that) and shut its fd down below to
       * force a genuine EPOLLIN/EPOLLERR dispatch, letting whichever
       * dispatch callback notices (_async_ctx_handle_if_abandoned)
       * perform the real, destructive teardown safely, from dispatch
       * context. Do not touch ctx again after the shutdown() call. */
      ctx->pending_app_teardown = true;
      _async_retry_hop(ctx); /* marks ctx->hop_completed = true itself,
                              * and queues a brand-new ctx to actually
                              * retry the request; this ctx (the reused
                              * one whose reactivation just failed) is
                              * abandoned from here on. */
      int fd = ctx->fd;
      mutex_unlock(ctx->idle_lock);
      /* Releases the SEPARATE engine reference _async_idle_pool_offer
       * acquired for this ctx while it sat in the idle pool;
       * _async_ctx_teardown (once the deferred dispatch finally runs it)
       * only ever releases ctx's chain reference and knows nothing about
       * this one. This is the same leak class _async_submit_hop_fail's
       * reused branch was fixed for (see its own comment); this
       * event_loop_modify-failure exit was missed at the time. Safe to
       * call from this thread directly: it only ever touches a global
       * counter, never ctx. */
      _client_engine_release();
      if (fd >= 0) shutdown(fd, SHUT_RDWR);
      return true;
    }
    /* ctx is fully consistent again (every per-hop field above is already
     * set, and the registration now correctly reflects WRITING), so
     * idle_lock can finally be released; any concurrent dispatch that was
     * blocked waiting for it now proceeds against a coherent ctx, and (since
     * the fd is almost certainly already writable, being freshly reused) may
     * do so essentially immediately. */
    mutex_unlock(ctx->idle_lock);
    /* This ctx's connection now belongs to the chain, not the idle pool;
     * release the engine reference it was holding while pooled (the
     * chain's own single reference, held for its whole lifetime, covers it
     * from here on). No deadline (re-)registration needed here: this ctx
     * was already registered back when it was first created on its very
     * first (fresh) hop attempt, and registration persists across every
     * idle-pool cycle since; see _client_deadline_register's own comment. */
    _client_engine_release();
    return true;
  }

  /* Registered BEFORE submitting, not after: once ctpool_submit hands ctx to
   * a worker, that worker can connect, run the whole hop to completion, and
   * free ctx before this thread would otherwise get a chance to register it
   *; registering first guarantees ctx is already safely in the registry
   * for the entire time any other thread can possibly touch it. */
  _client_deadline_register(ctx);

  ccol_retval_t sr = ctpool_submit(cli_engine_bundler.dns_pool,
                                   _async_connect_task, ctx, NULL);
  if (sr != ccol_success) {
    _async_fulfill_chain(chain, ccol_not_enough_memory, NULL);
    _async_ctx_teardown(ctx); /* unregisters ctx too, via _async_ctx_free */
    return false;
  }
  return true;
}

/*
 * Called once a hop's response has fully parsed as a redirect (pctx.
 * will_redirect). Resolves the Location header against this hop's URL,
 * applies Tier 1's identical method/body preservation rules (307/308 keep
 * the method and body; any other redirect status downgrades to GET and
 * drops the body unless the current method is already HEAD), finishes this
 * hop's connection (offering it to the idle pool if `reusable`, otherwise
 * closing it; see _async_finish_connection), and only THEN queues the
 * next hop.
 *
 * That order is deliberate and load-bearing, not cosmetic, for the close
 * case specifically: _async_ctx_free's close(fd) call runs synchronously,
 * on THIS thread, before _async_finish_connection returns. The next hop's
 * connect() call runs on a completely different ctpool worker thread and,
 * on a loopback redirect chain that opens and closes a fresh fd every hop
 * in quick succession, can be handed back the EXACT SAME fd number the
 * kernel just freed; but only once that worker thread actually gets to
 * run, which cannot happen until ctpool_submit for the next hop is called.
 * Finishing the connection unconditionally FIRST (so the old fd is fully
 * closed, and its event_loop registration fully removed, before
 * _async_submit_hop ever calls ctpool_submit for the next hop) guarantees
 * there is no window where the new hop's socket() could receive the old
 * fd's number while it might still be registered with the reactor; when the
 * connection is pooled instead, no fd is freed at all, so this ordering
 * costs nothing there either.
 *
 * Whether the handoff to the next hop succeeds or not, this connection's
 * job is done either way; on failure, _async_submit_hop has already
 * fulfilled the future with a specific error (or, for a Location that fails
 * to resolve, this function never creates a next hop at all and the
 * chain's refcount is unaffected); either way, this ctx's own teardown
 * (inside _async_finish_connection) is the one guaranteed to notice, via
 * _async_chain_release's backstop, that nothing fulfilled the future, and
 * do so itself with a generic error.
 */
static void _async_handle_redirect(chttp_async_ctx_t *ctx, bool reusable) {
  chttp_async_chain_t *chain = ctx->chain;
  /* Temporary, extra reference: _async_finish_connection below can trigger
   * (via _async_ctx_finish's teardown, or a successful idle-pool-offer's
   * own explicit release) a full teardown of THIS ctx's chain reference on
   * a DIFFERENT, concurrently-running reactor thread; if that happened to
   * be the last live reference, chain would be freed while this function
   * is still using its local `chain` pointer below (in _async_submit_hop's
   * argument and _mem_free(chain->mp, ...)). This is the same class of "a
   * concurrent teardown can free something a synchronous caller still
   * needs" race documented throughout this section, just one level higher
   * than the ctx-local ones. Retaining here first, and releasing only once
   * this function is completely done with `chain`, guarantees it never
   * hits zero out from under us regardless of how fast a concurrent
   * teardown races. */
  _async_chain_retain(chain);

  if (ctx->hop >= CHTTP_MAX_REDIRECTS) {
    /* This response is itself the 51st request in the chain and is ALSO a
     * redirect; following it would exceed the 50-redirect budget. Report it
     * as an error instead of silently delivering it, mirroring Tier 1's
     * identical cap check in chttp_do_internal (the deliberate design this
     * codebase shipped with previously, reversed per an explicit request: a
     * caller relying on ccol_http_too_many_redirects to detect a redirect
     * loop needs an actual error here, not a stale 3xx response it has to
     * notice and interpret itself). No URL resolution or next-hop work is
     * attempted at all. */
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
  /* Both fields above (is_ipv6, path_and_query below) used to be left at
   * their memset-zero default here, since chttp_async_ctx_t had no fields to
   * source them from: is_ipv6 always false silently produced an unbracketed
   * "scheme://<ipv6-literal>:port/path" redirect target that failed to
   * re-parse on the next hop, and a NULL path_and_query crashed the process
   * (a NULL-pointer strchr() inside _merge_ref_path) the moment a server
   * sent a genuinely relative (not absolute-path, not a full URL, not
   * protocol-relative) Location header. Both ctx fields are now captured
   * fresh on every hop in _async_submit_hop; see their own struct comments. */
  base.path_and_query = ctx->path_and_query;

  char *next_url = _resolve_redirect_url(chain->mp, &base, ctx->pctx.location);
  bool preserve =
      (ctx->pctx.status_code == 307 || ctx->pctx.status_code == 308);
  chttp_method_t next_method = ctx->cur_method;
  /* Read chain->body_dropped, not chain->body_data/body_len/body_content_type
   * unconditionally: once an earlier hop on this chain has already dropped
   * the body (a non-preserving redirect), it must STAY dropped for every
   * later hop, even one that is itself 307/308 (which preserves "whatever
   * the current body is", not "the chain's original, hop-0 body"); see
   * chain->body_dropped's own field comment for the full rationale and why
   * Tier 1's equivalent (its cur_body loop-local, overwritten in place) does
   * not need a similar dedicated flag. */
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
  /* Captured BEFORE _async_finish_connection, not after: that call can
   * trigger (via a concurrent reactor thread's dispatch) the normal
   * teardown of THIS ctx itself (freeing it), so ctx->hop must not be read
   * afterward. The temporary chain retain above only protects `chain`; it
   * does nothing for ctx, which is never safe to touch once its own
   * connection has been handed to _async_finish_connection. */
  int next_hop = ctx->hop + 1;

  _async_finish_connection(ctx, reusable);

  if (next_url) {
    _async_submit_hop(chain, next_url, next_method, next_body_data,
                      next_body_len, next_body_ct, next_hop);
    _mem_free(chain->mp, next_url);
  }
  _async_chain_release(chain); /* release the temporary ref taken above */
}

/*
 * Shared pre-flight validation for Tier 2 (_chttp_do_async_internal) and
 * Tier 3 (chttpclient_do_pooled/_streaming): validates cli/req, parses
 * req->url into *url_out (caller must _url_free it on ccol_success), and
 * checks TLS usability; the same three checks Tier 1's chttp_do_internal
 * performs, with the exact same result codes (ccol_invalid_args,
 * whatever _parse_chttp_url returns, ccol_http_tls_cert_load_failed).
 *
 * Tier 1 is NOT refactored to call this: its equivalent checks are woven
 * into the per-hop loop of chttp_do_internal, re-run fresh on every hop
 * (a redirect can change URL/scheme hop to hop, so there's no single
 * upfront check to extract there the way Tier 2/3 have; they only ever
 * need this once, before a chain/future exists at all). Tier 1 already
 * returns fully specific ccol_retval_t codes natively, so extracting its
 * inline logic would touch already-hardened, already-shipped code for no
 * functional benefit.
 *
 * This exists specifically so Tier 3 does not have to accept
 * chttpclient_do_async/_streaming's collapsed "NULL for any pre-queue
 * failure"; it can call this directly first and return the specific
 * code, matching Tier 1's error granularity for exactly the two failure
 * classes (bad URL, TLS unusable) that can be detected before a request is
 * ever queued.
 */
static ccol_retval_t _chttp_async_preflight_check(struct chttpclient *cli,
                                                  const chttp_request_t *req,
                                                  chttp_url_t *url_out) {
  if (!cli || !req) return ccol_invalid_args;

  ccol_retval_t prv = _parse_chttp_url(cli->m_procs, req->url, url_out);
  if (prv != ccol_success) return prv;

  mutex_lock(cli->lock);
  bool tls_ctx_usable = cli->tls_ctx_usable;
  mutex_unlock(cli->lock);

  if (url_out->is_https && !tls_ctx_usable) {
    /* Configured cert/key/ca path(s) were not readable (or ctls itself
     * failed to load/parse them) at set_tls time; that failure was
     * deferred here rather than aborting the process (see
     * _rebuild_tls_ctx_locked). Mirrors Tier 1's identical check and its
     * choice of error code for it; see chttp_do_internal's own comment on
     * this exact check for why ccol_http_tls_cert_load_failed, not
     * ccol_http_tls_handshake_failed, is correct here. */
    _url_free(cli->m_procs, url_out);
    return ccol_http_tls_cert_load_failed;
  }
  return ccol_success;
}

/*
 * Internal entry point: submits req for asynchronous execution against cli,
 * returning a future the caller must eventually pair with exactly one
 * ctpool_future_free (after an optional ctpool_future_get/_done). Returns
 * NULL if the request could not even be queued (bad arguments, invalid URL,
 * TLS unusable, OOM, or the engine failing to start); mirrors
 * ctpool_submit_future's own "NULL on failure" convention. Once a future has
 * been created, every subsequent failure (including one on a later redirect
 * hop) instead fulfils that future with a specific error and still returns
 * it, exactly like Tier 1 returns a ccol_retval_t instead of aborting
 * silently. Every queued hop is unconditionally freed, and the whole chain's
 * one engine reference released, once the redirect chain reaches a terminal
 * connection state (see chttp_async_chain_t's file-level comment).
 */
static ctpool_future *_chttp_do_async_internal(struct chttpclient *cli,
                                               const chttp_request_t *req,
                                               chttpcli_write_fn write_fn,
                                               void *write_ctx) {
  chttp_url_t url;
  if (_chttp_async_preflight_check(cli, req, &url) != ccol_success) return NULL;
  ccol_memmgmt_procs_t *mp = cli->m_procs;
  /* Captured before freeing url (which is otherwise only needed for the
   * pre-check above; _async_submit_hop re-parses req->url itself on every
   * hop): the whole chain's origin-changed-since-hop-0 tracking (see
   * chttp_async_chain_t.initial_origin_key's own comment) needs hop 0's
   * origin_key to persist for the chain's entire lifetime, not just this
   * function's own stack frame. */
  char *initial_origin_key = ccol_strdup(mp, url.origin_key);
  _url_free(mp, &url);
  if (!initial_origin_key) return NULL;

  /* Read and pin the client's TLS context under its lock, exactly like
   * Tier 1's chttp_do_internal does; ctls_ctx_retain pins it against a
   * concurrent chttpclient_set_tls freeing/rebuilding it while this request
   * is still using it. Pinned unconditionally (not just for an https first
   * hop) since a redirect chain can hop between http and https, exactly
   * like Tier 1's own tls_ctx local; the pinned reference is released once
   * by _async_chain_release (via ctls_ctx_release) when the whole chain's
   * last hop is torn down. */
  ctls_ctx_t *tls_ctx;
  bool tls_ctx_usable;
  bool verify_host;
  long connect_timeout_ms;
  long request_timeout_ms;
  size_t max_response_body_size;
  mutex_lock(cli->lock);
  tls_ctx = cli->tls_ctx;
  tls_ctx_usable = cli->tls_ctx_usable;
  verify_host = cli->tls.verify_host;
  connect_timeout_ms = cli->connect_timeout_ms;
  request_timeout_ms = cli->request_timeout_ms;
  max_response_body_size = cli->max_response_body_size;
  if (tls_ctx) ctls_ctx_retain(tls_ctx);
  mutex_unlock(cli->lock);

  if (_client_engine_acquire() != ccol_success) {
    if (tls_ctx) ctls_ctx_release(tls_ctx);
    _mem_free(mp, initial_origin_key);
    return NULL;
  }

  char *ferr = NULL;
  ctpool_future *future = ctpool_future_create_detached(&ferr);
  if (!future) {
    if (tls_ctx) ctls_ctx_release(tls_ctx);
    _client_engine_release();
    _mem_free(mp, initial_origin_key);
    return NULL;
  }
  /* From here on a future exists and is always returned to the caller (who
   * owns pairing it with exactly one ctpool_future_free); any subsequent
   * failure fulfils it with a specific error instead of returning NULL,
   * exactly mirroring this function's previous, single-hop behaviour on a
   * ctpool_submit failure. Captured into a local now: once hop 0 is queued
   * below, a worker may run the request to completion and free the chain
   * via _async_chain_release before this function's own thread runs another
   * instruction; reading chain->future afterward would be a
   * use-after-free. */
  ctpool_future *f = future;

  chttp_async_chain_t *chain = _async_chain_create(
      mp, cli, future, (chmap)req->headers, req->body.data, req->body.len,
      req->body.content_type, initial_origin_key, tls_ctx, tls_ctx_usable,
      verify_host, connect_timeout_ms, request_timeout_ms,
      max_response_body_size, write_fn, write_ctx, req->expect_continue);
  /* _async_chain_create takes a deep copy of initial_origin_key (matching
   * how it already handles req_headers/body_data/body_content_type); this
   * function's own local copy is never retained beyond this call, whether
   * or not chain creation succeeded. */
  _mem_free(mp, initial_origin_key);
  if (!chain) {
    if (tls_ctx) ctls_ctx_release(tls_ctx);
    /* _async_chain_create fails only on an allocation failure (calloc,
     * mutex_init, or one of the deep-copy allocations it performs), so
     * ccol_not_enough_memory is always the accurate cause here. Reported
     * via a real chttpcli_async_result_t (mirroring exactly what
     * _async_fulfill_chain builds on its own success path) rather than
     * fulfilling with a bare NULL, so chttpclient_async_result_get returns
     * a genuine result carrying that error code instead of collapsing this
     * specific, common failure into the same NULL a cancelled future
     * produces - which Tier 3 (chttpclient_do_pooled/_streaming) would
     * otherwise have no way to distinguish from any other NULL-result
     * cause, reporting the generic ccol_unexpected_failure instead of
     * ccol_not_enough_memory. Only falls back to the bare-NULL fulfill (the
     * same last-resort this function's own _async_fulfill_chain already
     * uses when it can't even allocate ITS result struct) if this smaller
     * allocation also fails. */
    chttpcli_async_result_t *result =
        (chttpcli_async_result_t *)_mem_calloc(mp, 1, sizeof(*result));
    if (result) {
      result->rv = ccol_not_enough_memory;
      result->resp = NULL;
      result->_m_procs = mp;
      ctpool_future_fulfill(future, result);
    } else {
      ctpool_future_fulfill(future, NULL); /* producer side; caller's own
                                            * ref is released via its
                                            * eventual ctpool_future_free(f)
                                            * call */
    }
    _client_engine_release();
    return f;
  }
  _async_submit_hop(chain, req->url, req->method, req->body.data, req->body.len,
                    req->body.content_type, 0);
  return f;
}

/* White-box test helpers exposing internal engine state. Not part of the
 * public API; gated so these symbols do not leak into a production build
 * of libccollections.so (the surrounding engine code itself is no longer
 * gated now that chttpclient_do_async/_streaming are real public callers,
 * but these five functions exist purely for test instrumentation). */
#ifdef RUNNING_UNIT_TESTS
/* Exposes sizeof(chttp_async_chain_t) (an internal, non-public type) so a
 * test can build a size-targeted fault-injecting allocator that fails
 * exactly the chain struct's own calloc call inside _async_chain_create,
 * regardless of how many other allocations of different sizes happen to
 * precede it (URL parsing, etc.) - deterministic and immune to drift from a
 * future change shifting the number of preceding allocation calls, unlike
 * an index-based fault injector. */
size_t _chttp_async_chain_struct_size_for_tests(void) {
  return sizeof(chttp_async_chain_t);
}

int _chttpclient_engine_ref_count_for_tests(void) {
  call_once(cli_engine_bundler.once, _client_engine_globals_init);
  mutex_lock(cli_engine_bundler.mutex);
  int n = (int)cli_engine_bundler.reactor_refs;
  mutex_unlock(cli_engine_bundler.mutex);
  return n;
}

bool _chttpclient_engine_running_for_tests(void) {
  call_once(cli_engine_bundler.once, _client_engine_globals_init);
  mutex_lock(cli_engine_bundler.mutex);
  bool running = (cli_engine_bundler.reactor != EVENT_LOOP_INVALID);
  mutex_unlock(cli_engine_bundler.mutex);
  return running;
}

ccol_retval_t _chttpclient_engine_acquire_for_tests(void) {
  return _client_engine_acquire();
}

void _chttpclient_engine_release_for_tests(void) { _client_engine_release(); }

void _chttpclient_engine_wait_for_quiescence_for_tests(void) {
  _client_engine_wait_for_quiescence();
}

size_t _chttpclient_engine_num_reactor_threads_for_tests(void) {
  call_once(cli_engine_bundler.once, _client_engine_globals_init);
  mutex_lock(cli_engine_bundler.mutex);
  size_t n = cli_engine_bundler.last_resolved_num_reactor_threads;
  mutex_unlock(cli_engine_bundler.mutex);
  return n;
}

/* Rewrites every currently-pooled Tier 2/3 idle connection's last_used
 * timestamp far enough into the past to make _async_idle_pool_take's own
 * CHTTP_IDLE_MAX_AGE_MS staleness check treat it as aged-out on the very
 * next pop, without a test actually waiting out the real 60-second window.
 * Test-only: exists purely to make the staleness-eviction path in
 * _async_idle_pool_take deterministically reachable. */
void _chttpclient_force_async_idle_stale_for_tests(struct chttpclient *cli) {
  mutex_lock(cli->lock);
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
  mutex_unlock(cli->lock);
}

/* Reads cli->idle_total_count_async: how many connections are currently
 * sitting in Tier 2/3's async idle pool, across every origin. Test-only:
 * lets a test verify live pool membership directly instead of inferring it
 * indirectly, needed to state an ordinal-independent invariant ("whenever
 * this pool is empty, the engine's ref count contributed by it must be
 * zero too") that holds regardless of exactly which internal allocation an
 * injected OOM failure happens to land on. */
size_t _chttpclient_async_idle_total_count_for_tests(struct chttpclient *cli) {
  mutex_lock(cli->lock);
  size_t n = cli->idle_total_count_async;
  mutex_unlock(cli->lock);
  return n;
}

/* Reads how many distinct origin keys cli->idle_pools (Tier 1) currently
 * holds an entry for. Test-only: lets a test verify that popping the last
 * connection for an origin actually prunes that origin's own now-empty
 * chmap entry, rather than merely leaving an empty per-origin cvec behind
 * forever (see _idle_pool_take's own pruning comment). */
size_t _chttpclient_idle_pools_key_count_for_tests(struct chttpclient *cli) {
  mutex_lock(cli->lock);
  size_t n = cli->idle_pools ? chmap_elem_count(cli->idle_pools) : 0;
  mutex_unlock(cli->lock);
  return n;
}

/* Async (Tier 2/3) counterpart of _chttpclient_idle_pools_key_count_for_
 * tests above, for cli->idle_pools_async. */
size_t _chttpclient_idle_pools_async_key_count_for_tests(
    struct chttpclient *cli) {
  mutex_lock(cli->lock);
  size_t n =
      cli->idle_pools_async ? chmap_elem_count(cli->idle_pools_async) : 0;
  mutex_unlock(cli->lock);
  return n;
}

/* Overrides CHTTP_MAX_IDLE_ORIGINS's effective value (shared by both
 * _idle_pool_offer and _async_idle_pool_offer) for the remainder of this
 * process; see g_max_idle_origins_override_for_tests's own comment. Pass 0
 * to restore the real compile-time constant. Process-wide, not per-client,
 * exactly like every other *_for_tests fault-injection hook in this file. */
void _chttpclient_set_max_idle_origins_for_tests(size_t n) {
  atomic_store(&g_max_idle_origins_override_for_tests, n);
}

/* Resolves h to its underlying struct chttpclient* WITHOUT pinning it (does
 * not touch pending_resolve_count at all): a bare slot-table lookup, safe
 * for tests specifically because test code calling this runs synchronously,
 * single-threaded, with no concurrent destroy to race in the first place;
 * unlike _chttpcli_resolve, there is no matching _unpin call a test needs to
 * remember, which would otherwise be an easy gap to leave (a forgotten
 * unpin would leave pending_resolve_count permanently nonzero on that
 * client, silently hanging every future chttpclient_destroy call against
 * it). Returns NULL under the exact same conditions _chttpcli_resolve does. */
struct chttpclient *_chttpcli_resolve_for_tests(chttpcli h) {
  call_once(chttpcli_slot_table.once, _chttpcli_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  rw_lock_rdlock(chttpcli_slot_table.rwlock);
  struct chttpclient *raw = NULL;
  if (idx < cvector_elem_count(chttpcli_slot_table.slots)) {
    chttpcli_slot_t *slot =
        (chttpcli_slot_t *)cvector_at(chttpcli_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  rw_lock_unlock(chttpcli_slot_table.rwlock);
  return raw;
}

/* Reads how many slots the chttpcli handle table currently holds (grown
 * ones plus freed-but-not-yet-reused ones): lets a test assert that a
 * create/destroy churn loop reuses freed slots rather than growing the
 * table without bound. */
size_t _chttpcli_slot_table_capacity_for_tests(void) {
  call_once(chttpcli_slot_table.once, _chttpcli_slot_table_init_globals);
  rw_lock_rdlock(chttpcli_slot_table.rwlock);
  size_t n = cvector_elem_count(chttpcli_slot_table.slots);
  rw_lock_unlock(chttpcli_slot_table.rwlock);
  return n;
}

/* Forces _async_idle_pool_offer's very next cvector_push_back call (for any
 * client, any origin) to be treated as if it had failed, without touching
 * the real allocator or the real per-origin list at all. Consumed
 * automatically the first time that call site is reached afterward; call
 * again before each attempt that needs to exercise this path. See
 * g_force_offer_push_fail_for_tests's own comment (above the "ASYNC IDLE
 * POOL" section) for why this scenario cannot be reached via ordinary
 * allocator-failure injection. */
void _chttpclient_force_offer_push_fail_once_for_tests(void) {
  atomic_store(&g_force_offer_push_fail_for_tests, true);
}

/* Forces _async_submit_hop's very next reused-connection event_loop_modify
 * call (for any client) to be treated as if it had failed, without actually
 * touching the real registration at all. Consumed automatically the first
 * time that call site is reached afterward. See
 * g_force_reactivate_fail_for_tests's own comment (above the "ASYNC IDLE
 * POOL" section) for why this scenario cannot be reached via ordinary
 * allocator-failure injection. */
void _chttpclient_force_reactivate_fail_once_for_tests(void) {
  atomic_store(&g_force_reactivate_fail_for_tests, true);
}

/* Forces _async_on_readable_impl's very next read dispatch for a ctx that
 * has already had at least one real, successful read (i.e. never the very
 * first read of a fresh/reused connection) to be treated exactly as if
 * recv()/ctls_conn_read() had returned -1/ECONNRESET, without touching the
 * real socket at all. Consumed automatically the first time that condition
 * is reached afterward. See g_force_async_hard_read_error_for_tests's own
 * comment (above the "ASYNC IDLE POOL" section) for why a real TCP RST's
 * timing can't be pinned down deterministically over an actual socket. */
void _chttpclient_force_async_hard_read_error_once_for_tests(void) {
  atomic_store(&g_force_async_hard_read_error_for_tests, true);
}
#endif /* RUNNING_UNIT_TESTS */

ctpool_future *chttpclient_do_async(chttpcli h, const chttp_request_t *req) {
  struct chttpclient *raw = _chttpcli_resolve(h);
  if (!raw) return NULL;
  ctpool_future *f = _chttp_do_async_internal(raw, req, NULL, NULL);
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
  ctpool_future *f = _chttp_do_async_internal(raw, req, write_fn, write_ctx);
  _chttpcli_resolve_unpin(raw);
  return f;
}

chttpcli_async_result_t *chttpclient_async_result_get(ctpool_future *f) {
  return (chttpcli_async_result_t *)ctpool_future_get(f);
}

void chttpclient_async_result_free(chttpcli_async_result_t *result) {
  if (!result) return;
  _mem_free(result->_m_procs, result);
}

/* ========================================================================== */
/*                    POOLED-SYNC API (TIER 3)                                */
/* ========================================================================== */

/*
 * Both functions below are thin wrappers: submit via Tier 2, block on the
 * future, unwrap the result into the exact same ccol_retval_t/resp_out (or
 * status_code_out) shape chttpclient_do/chttpclient_do_streaming use, then
 * free the future and its result before returning; the caller never sees
 * ctpool_future or chttpcli_async_result_t at all. _chttp_async_preflight_
 * check runs first specifically so a bad URL or unusable TLS config is
 * reported with the same specific code chttpclient_do would use, rather
 * than chttpclient_do_async/_streaming's collapsed NULL for that case (see
 * that helper's own comment). An allocation failure building the async
 * chain itself is reported as the specific ccol_not_enough_memory (see
 * _chttp_do_async_internal's own chain-creation-failure branch); any OTHER
 * pre-queue failure (the engine failing to start, ctpool_future_create_
 * detached itself failing) still collapses to ccol_unexpected_failure,
 * since those surface as a bare NULL future from chttpclient_do_async/
 * _streaming with no result object to carry a more specific code at all.
 */

ccol_retval_t chttpclient_do_pooled(chttpcli h, const chttp_request_t *req,
                                    chttpcli_response **resp_out) {
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;

  struct chttpclient *raw = _chttpcli_resolve(h);
  if (!raw) return ccol_invalid_args;

  chttp_url_t url;
  ccol_retval_t prv = _chttp_async_preflight_check(raw, req, &url);
  if (prv != ccol_success) {
    _chttpcli_resolve_unpin(raw);
    return prv;
  }
  _url_free(raw->m_procs, &url);
  _chttpcli_resolve_unpin(raw); /* raw is not touched again; everything
      below operates on the original handle h, via chttpclient_do_async's
      own independent resolve */

  ctpool_future *f = chttpclient_do_async(h, req);
  if (!f) return ccol_unexpected_failure;

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

  chttp_url_t url;
  ccol_retval_t prv = _chttp_async_preflight_check(raw, req, &url);
  if (prv != ccol_success) {
    _chttpcli_resolve_unpin(raw);
    return prv;
  }
  _url_free(raw->m_procs, &url);
  _chttpcli_resolve_unpin(raw); /* raw is not touched again; everything
      below operates on the original handle h, via chttpclient_do_async_
      streaming's own independent resolve */

  ctpool_future *f =
      chttpclient_do_async_streaming(h, req, write_fn, write_ctx);
  if (!f) return ccol_unexpected_failure;

  chttpcli_async_result_t *result = chttpclient_async_result_get(f);
  if (!result) {
    ctpool_future_free(f);
    return ccol_unexpected_failure;
  }

  ccol_retval_t rv = result->rv;
  if (result->resp) {
    /* Streaming's chttpcli_response always exists internally (its body/
     * headers just stay NULL; see _async_build_response) purely so
     * status_code has somewhere to travel through the single
     * chttpcli_async_result_t.resp channel; this function's own public
     * contract has no resp_out at all (mirrors chttpclient_do_streaming
     * exactly), so it is unwrapped and freed here, never exposed. */
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

chttpcli create_chttpclient_mp(ccol_memmgmt_procs_t *mprocs, char **err_str) {
  if (mprocs && !ccol_verify_memmgmt_procs(mprocs, err_str))
    return CHTTPCLI_INVALID;

  ccol_memmgmt_procs_t *mp = NULL;
  if (mprocs) {
    mp = (ccol_memmgmt_procs_t *)mprocs->malloc(sizeof(ccol_memmgmt_procs_t));
    if (!mp) {
      if (err_str) *err_str = CCOL_ERR_STR("failed to allocate mprocs");
      return CHTTPCLI_INVALID;
    }
    mem_cpy(mp, mprocs, sizeof(ccol_memmgmt_procs_t));
  }

  struct chttpclient *cli =
      (struct chttpclient *)_mem_calloc(mp, 1, sizeof(struct chttpclient));
  if (!cli) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate client");
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }

  cli->m_procs = mp;
  cli->tls = CHTTP_TLS_DEFAULT;
  if (mutex_init(cli->lock) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to initialize mutex");
    _mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }
  /* CLOCK_MONOTONIC: _slot_acquire hands this condvar a CLOCK_MONOTONIC
   * timespec (from _deadline_make) via cond_var_timedwait; see
   * _cond_var_init_monotonic's own comment for why the clock must match. */
  if (_cond_var_init_monotonic(&cli->available) != 0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("failed to initialize condition variable");
    mutex_destroy(cli->lock);
    _mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }
  if (cond_var_init(cli->idle_async_drained) != 0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("failed to initialize condition variable");
    mutex_destroy(cli->lock);
    cond_var_destroy(cli->available);
    _mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }
  if (mutex_init(cli->async_count_lock) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to initialize mutex");
    mutex_destroy(cli->lock);
    cond_var_destroy(cli->available);
    cond_var_destroy(cli->idle_async_drained);
    _mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }
  if (cond_var_init(cli->async_count_drained) != 0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("failed to initialize condition variable");
    mutex_destroy(cli->lock);
    cond_var_destroy(cli->available);
    cond_var_destroy(cli->idle_async_drained);
    mutex_destroy(cli->async_count_lock);
    _mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }

  char *herr = NULL;
  cli->idle_pools =
      chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                        ccol_pointer, mp, NULL, &herr);
  if (!cli->idle_pools) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate idle pool map");
    mutex_destroy(cli->lock);
    cond_var_destroy(cli->available);
    cond_var_destroy(cli->idle_async_drained);
    mutex_destroy(cli->async_count_lock);
    cond_var_destroy(cli->async_count_drained);
    _mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }

  cli->idle_pools_async =
      chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                        ccol_pointer, mp, NULL, &herr);
  if (!cli->idle_pools_async) {
    if (err_str)
      *err_str = CCOL_ERR_STR("failed to allocate async idle pool map");
    __chmap_destroy(cli->idle_pools);
    mutex_destroy(cli->lock);
    cond_var_destroy(cli->available);
    cond_var_destroy(cli->idle_async_drained);
    mutex_destroy(cli->async_count_lock);
    cond_var_destroy(cli->async_count_drained);
    _mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }

  if (_rebuild_tls_ctx_locked(cli) != ccol_success) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to build default TLS context");
    __chmap_destroy(cli->idle_pools);
    __chmap_destroy(cli->idle_pools_async);
    mutex_destroy(cli->lock);
    cond_var_destroy(cli->available);
    cond_var_destroy(cli->idle_async_drained);
    mutex_destroy(cli->async_count_lock);
    cond_var_destroy(cli->async_count_drained);
    _mem_free(mp, cli);
    if (mp) mp->free(mp);
    return CHTTPCLI_INVALID;
  }

  chttpcli h = _chttpcli_handle_slot_acquire(cli);
  if (h == 0) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate client slot");
    if (cli->tls_ctx) ctls_ctx_release(cli->tls_ctx);
    __chmap_destroy(cli->idle_pools);
    __chmap_destroy(cli->idle_pools_async);
    mutex_destroy(cli->lock);
    cond_var_destroy(cli->available);
    cond_var_destroy(cli->idle_async_drained);
    mutex_destroy(cli->async_count_lock);
    cond_var_destroy(cli->async_count_drained);
    _mem_free(mp, cli);
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
  mutex_lock(cli->lock);
  cli->configured_pool_size = n;
  cli->pool_cap = _resolve_pool_cap(n);
  cli->pool_initialized = true;
  cond_var_broadcast(cli->available);
  mutex_unlock(cli->lock);
  _chttpcli_resolve_unpin(cli);
  return ccol_success;
}

ccol_retval_t chttpclient_set_connect_timeout(chttpcli h, long ms) {
  struct chttpclient *cli = _chttpcli_resolve(h);
  if (!cli) return ccol_invalid_args;
  mutex_lock(cli->lock);
  cli->connect_timeout_ms = ms;
  mutex_unlock(cli->lock);
  _chttpcli_resolve_unpin(cli);
  return ccol_success;
}

ccol_retval_t chttpclient_set_request_timeout(chttpcli h, long ms) {
  struct chttpclient *cli = _chttpcli_resolve(h);
  if (!cli) return ccol_invalid_args;
  mutex_lock(cli->lock);
  cli->request_timeout_ms = ms;
  mutex_unlock(cli->lock);
  _chttpcli_resolve_unpin(cli);
  return ccol_success;
}

ccol_retval_t chttpclient_set_max_response_body_size(chttpcli h,
                                                     size_t max_bytes) {
  struct chttpclient *cli = _chttpcli_resolve(h);
  if (!cli) return ccol_invalid_args;
  mutex_lock(cli->lock);
  cli->max_response_body_size = max_bytes;
  mutex_unlock(cli->lock);
  _chttpcli_resolve_unpin(cli);
  return ccol_success;
}

ccol_retval_t chttpclient_set_tls(chttpcli h, const chttp_tls_config_t *tls) {
  if (!h) return ccol_invalid_args;
  /* A client certificate and its private key are a pair: providing exactly
   * one of the two is never valid configuration, and silently treating it
   * as "no client certificate configured" (the effect of _rebuild_tls_ctx_
   * locked's have_cert_pair check further down, which simply requires
   * both) would leave an mTLS deployment believing it presents a client
   * certificate when it never does, with no error surfaced anywhere. This
   * check only ever inspects the caller-supplied tls argument, never cli,
   * so it stays here, before any resolve, needing no pin/unpin of its
   * own. */
  if (tls && ((tls->cert_path && !tls->key_path) ||
              (!tls->cert_path && tls->key_path)))
    return ccol_invalid_args;

  struct chttpclient *cli = _chttpcli_resolve(h);
  if (!cli) return ccol_invalid_args;

  mutex_lock(cli->lock);

  _mem_free(cli->m_procs, cli->owned_cert_path);
  _mem_free(cli->m_procs, cli->owned_key_path);
  _mem_free(cli->m_procs, cli->owned_ca_bundle_path);
  cli->owned_cert_path = cli->owned_key_path = cli->owned_ca_bundle_path = NULL;

  if (!tls) {
    cli->tls = CHTTP_TLS_DEFAULT;
    ccol_retval_t rv = _rebuild_tls_ctx_locked(cli);
    mutex_unlock(cli->lock);
    _chttpcli_resolve_unpin(cli); /* exit 1 of 4 */
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
    mutex_unlock(cli->lock);
    _chttpcli_resolve_unpin(cli); /* exit 2 of 4 */
    return rv;
  }

oom:
  _mem_free(cli->m_procs, cli->owned_cert_path);
  _mem_free(cli->m_procs, cli->owned_key_path);
  _mem_free(cli->m_procs, cli->owned_ca_bundle_path);
  cli->owned_cert_path = cli->owned_key_path = cli->owned_ca_bundle_path = NULL;
  cli->tls = CHTTP_TLS_DEFAULT;
  /* cli->tls_ctx/tls_ctx_usable still describe whatever configuration was
   * in effect before this call started (built by a previous, successful
   * chttpclient_set_tls); cli->tls itself was just reset to the default
   * above, so leaving tls_ctx/tls_ctx_usable untouched would make the
   * client's declared configuration and its actual runtime TLS behavior
   * silently diverge (requests would keep using the OLD cert/CA material
   * forever, with nothing about cli->tls indicating that). Rebuilding now
   * brings tls_ctx back in sync with the (default) config this call is
   * actually leaving in place; its own return is not this function's
   * result, since the real failure to report is the strdup OOM above, not
   * whatever rebuilding a plain default config does or doesn't need to
   * allocate. */
  _rebuild_tls_ctx_locked(cli);
  mutex_unlock(cli->lock);
  _chttpcli_resolve_unpin(cli); /* exit 3 of 4 (reachable from all three
                                    strdup failure checks above) */
  return ccol_not_enough_memory;
}

/* ========================================================================== */
/*                         CLIENT DESTRUCTION                                 */
/* ========================================================================== */

void __chttpclient_destroy(chttpcli cli) {
  if (!cli) return;

  /* Defensive: if the caller passed the handle chttp_default_client()
   * returns (its own doc comment invites passing it to chttpclient_set_*,
   * and nothing stops a caller from also passing it here), clear the
   * singleton's own copy of this handle first. Without this,
   * default_client_bundler.client would keep holding a handle this call is
   * about to invalidate; both handed straight back out by any later
   * chttp_default_client()/chttp_do()/chttp_get() call in this process (a
   * use-after-destroy, since default_client_bundler.once never re-fires to
   * rebuild it), and destroyed a second time by this file's own
   * process-exit destructor, which is now exactly the fatal double-destroy
   * this redesign exists to catch rather than a silent double-free. A no-op
   * (compare-and-swap fails harmlessly) for any client actually created via
   * create_chttpclient/_mp, which can never equal this singleton's handle. */
  chttpcli expected = cli;
  atomic_compare_exchange_strong(&default_client_bundler.client, &expected, 0);

  /* Resolve cli through the slot table, marking the slot not-in-use in the
   * same critical section as the lookup: this is what makes a second,
   * concurrent (or later, sequential) destroy call on the same handle value
   * see a resolve failure rather than racing this call's own teardown; see
   * the slot table's own file-level comment and _chttpcli_resolve's
   * comment for the full design. A stale or already-destroyed handle
   * reaching here is exactly the misuse this redesign exists to catch: it
   * is fatal, not a silent use-after-free/double-free. */
  call_once(chttpcli_slot_table.once, _chttpcli_slot_table_init_globals);
  uint32_t idx = (uint32_t)(cli >> 32);
  uint32_t gen = (uint32_t)(cli & 0xFFFFFFFFu);
  rw_lock_wrlock(chttpcli_slot_table.rwlock);
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
    rw_lock_unlock(chttpcli_slot_table.rwlock);
    fatal_err(
        "chttpclient_destroy: handle is stale or already destroyed "
        "(double-destroy / use-after-destroy of a chttpcli handle)");
  }
  slot->in_use = false; /* blocks ALL future resolves for this handle from
                            this instant, including a second concurrent
                            destroy attempt */
  rw_lock_unlock(chttpcli_slot_table.rwlock);

  mutex_lock(raw->lock);
  raw->destroying = true;
  cond_var_broadcast(raw->available);
  /* Combined predicate, not two sequential loops: pending_resolve_count
   * (see that field's own comment on struct chttpclient) and in_flight_count
   * both independently gate "is anyone still touching this object". */
  while (atomic_load(&raw->pending_resolve_count) > 0 ||
         raw->in_flight_count > 0)
    cond_var_wait(raw->available, raw->lock);
  mutex_unlock(raw->lock);

  /* Wait for every ACTIVE (not yet idle-pooled) Tier 2/3 async chain
   * created for this client to finish before touching anything else below:
   * an in-flight chain can still be connecting/handshaking/writing/reading
   * on a reactor or DNS/connect-pool thread, dereferencing chain->cli/
   * ctx->cli (raw->lock, raw->idle_pools_async, raw->m_procs) at essentially
   * any point until it either tears down or joins the idle pool this
   * function drains further below. Without this wait, a caller doing
   * `f = chttpclient_do_async(cli, req); chttpclient_destroy(cli);` would
   * free raw out from under a still-in-flight request; see this field's own
   * comment on struct chttpclient for why a dedicated lock/condvar is used
   * here rather than raw->lock/available. This must run before the idle-pool
   * draining below: an active chain completing while this wait is still in
   * progress is exactly what is expected to feed fresh entries into that
   * pool, which the idle-pool draining logic then cleans up. */
  mutex_lock(raw->async_count_lock);
  while (raw->async_in_flight_count > 0)
    cond_var_wait(raw->async_count_drained, raw->async_count_lock);
  mutex_unlock(raw->async_count_lock);

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

  /* Tier 2's own idle pool (see the "ASYNC IDLE POOL" section earlier in
   * this file). Shut down every currently pooled connection's fd; each
   * one's own IDLE-state dispatch (_async_idle_ctx_finish) removes it from
   * the pool and frees it (releasing its idle-held engine reference)
   * asynchronously once the reactor observes it; wait for
   * idle_total_count_async to reach zero before proceeding, since raw is
   * about to be freed below and those deferred teardowns read
   * raw->lock/raw->idle_pools_async. shutdown() (rather than closing the fd
   * directly here) is safe to call while still holding raw->lock, exactly
   * like the deadline sweep's identical use of it: it has no synchronous
   * application-level callback of its own, so there is no
   * reentrancy/lock-order hazard in calling it from inside this loop. */
  mutex_lock(raw->lock);
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
  /* idle_total_count_async only reaches zero once _async_idle_ctx_finish
   * has actually removed and torn down every pooled connection for real
   * (that decrement happens strictly after _async_ctx_free returns; see
   * that function's own comment), including any stale candidate this
   * shutdown sweep above is only now forcing an EOF/error dispatch for.
   * A stale candidate a concurrent _async_idle_pool_take walk (mechanism 1;
   * see that function's own comment) left shutdown-but-not-yet-reaped is
   * still counted here too, since it's still sitting in the vector.
   * Mechanism 2's deferred teardowns (pending_app_teardown; see that
   * field's own comment) are covered by the async_in_flight_count wait
   * already passed above: such a ctx still holds ctx->chain (and hence the
   * chain's own reference) until its own dispatch-triggered
   * _async_ctx_teardown actually runs, so that earlier wait cannot have
   * returned while one is still pending either. */
  while (raw->idle_total_count_async > 0)
    cond_var_wait(raw->idle_async_drained, raw->lock);
  mutex_unlock(raw->lock);

  if (raw->idle_pools_async) {
    cmap_iterator *it = chashmap_begin_iter(raw->idle_pools_async, NULL);
    for (; it; it = it->_next_fn(it)) {
      cvec list = _read_cvec(it->val_pair->ptr);
      if (list) __cvector_destroy(list);
    }
    __chmap_destroy(raw->idle_pools_async);
  }

  if (raw->tls_ctx) ctls_ctx_release(raw->tls_ctx);

  mutex_destroy(raw->lock);
  cond_var_destroy(raw->available);
  cond_var_destroy(raw->idle_async_drained);
  mutex_destroy(raw->async_count_lock);
  cond_var_destroy(raw->async_count_drained);

  ccol_memmgmt_procs_t *mp = raw->m_procs;
  _mem_free(mp, raw->owned_cert_path);
  _mem_free(mp, raw->owned_key_path);
  _mem_free(mp, raw->owned_ca_bundle_path);
  _mem_free(mp, raw);
  if (mp) mp->free(mp);

  /* Release the slot last, only after raw is fully torn down and freed:
   * this is what makes the slot's generation bump (and the free-index
   * push-back) mark the handle as reusable, not any earlier step. Re-fetch
   * by idx rather than reusing `slot`: a concurrent create_chttpclient_mp's
   * own _chttpcli_handle_slot_acquire call in between may have reallocated
   * slots' backing array via cvector_push_back, invalidating any pointer
   * into it taken before this second lock acquisition; idx itself is
   * stable. */
  rw_lock_wrlock(chttpcli_slot_table.rwlock);
  chttpcli_slot_t *slot2 =
      (chttpcli_slot_t *)cvector_at(chttpcli_slot_table.slots, idx);
  slot2->ptr = NULL;
  slot2->generation++; /* bumps this slot's generation past whatever value
      the just-freed cli's handle carried, so that stale handle can never
      again match a FUTURE acquire's generation for this same index */
  cvector_push_back(chttpcli_slot_table.free_indices, &idx);
  rw_lock_unlock(chttpcli_slot_table.rwlock);
}

/* ========================================================================== */
/*                         REQUEST EXECUTION                                  */
/* ========================================================================== */

/*
 * Sends `wire` (the fully serialized request, `wire_len` bytes, with the
 * body (if any) appended verbatim at the end, exactly `body_len` bytes)
 * and reads the response, honoring chttp_request_t.expect_continue when
 * `use_100_continue` is true (the caller has already confirmed this hop
 * genuinely has a body to hold back: body_carrying_method && body.data &&
 * body.len > 0, matching _serialize_request's own condition for having
 * emitted the "expect: 100-continue" header in the first place).
 *
 * Non-100-continue case: unchanged single-shot send-then-read, exactly what
 * this logic looked like before this function existed.
 *
 * 100-continue case: sends only the header portion first, then waits up to
 * CHTTP_100_CONTINUE_WAIT_MS (bounded by whatever is left of `overall`) for
 * either:
 *   - a "100 Continue" interim response: pctx is reset (a fresh header map,
 *     matching this codebase's "fresh state per message" convention; the
 *     interim response's own headers must never leak into the final one),
 *     the body is sent, and the real final response is read, carrying
 *     forward any bytes a fast/optimistic server already sent past the
 *     interim message's own boundary in the same read (see
 *     _chttp_read_message's own doc comment for why this can happen and why
 *     it must not be silently dropped as garbage);
 *   - the server answering directly without a "100 Continue" at all (RFC
 *     7231 SS5.1.1 explicitly permits this, e.g. to reject a request
 *     without wanting the body); that response IS the final response, and
 *     the body is never sent;
 *   - a timeout: the body is sent anyway and the final response is read
 *     normally, matching curl's own CURLOPT_EXPECT_100_TIMEOUT_MS behavior.
 *
 * The wait itself is driven through _chttp_read_message_loop (stop_at_status
 * = 100), not a single _chttp_read_message call: a server may legitimately
 * send some OTHER interim 1xx status (e.g. "103 Early Hints", RFC 8297)
 * ahead of either "100 Continue" or its final answer, and that must be
 * discarded and waited past too, still within this same wait window,
 * rather than being misread as the final response itself.
 *
 * *retry_unsafe_out is set true the moment this hop sends the body onto the
 * wire AFTER having received an explicit "100 Continue" from the server on
 * THIS connection, and stays false in every other case (including the
 * timeout-then-send-anyway branch, which never received any confirmation).
 * chttp_do_internal's reused-connection retry-once safety net normally
 * assumes a reused connection MIGHT already have been dead before this hop
 * ever wrote a byte to it (see that function's own comment); an explicit
 * "100 Continue" response disproves that assumption outright for this
 * specific connection, so a subsequent read failure with zero final-response
 * bytes is no longer safe to interpret as "nothing was ever sent, retry is
 * free" - the body has already been handed to a peer proven alive and
 * willing to receive it moments earlier, and blindly resending it to a
 * second, unrelated connection risks the server processing a non-idempotent
 * request twice.
 */
static ccol_retval_t _chttp_send_and_read(
    chttp_conn_t *conn, const char *wire, size_t wire_len, size_t body_len,
    bool use_100_continue, chttp_deadline_t *overall, chttp_parse_ctx_t *pctx,
    bool *keep_alive_out, bool *any_bytes_read_out, bool *retry_unsafe_out) {
  *retry_unsafe_out = false;
  if (!use_100_continue) {
    ccol_retval_t prv = _chttp_send_all(conn, wire, wire_len, overall);
    if (prv != ccol_success) return prv;
    return _chttp_read_response(conn, pctx, overall, keep_alive_out,
                                any_bytes_read_out);
  }

  size_t header_len = wire_len - body_len;
  ccol_retval_t prv = _chttp_send_all(conn, wire, header_len, overall);
  if (prv != ccol_success) return prv;

  chttp_deadline_t continue_dl = _deadline_make(CHTTP_100_CONTINUE_WAIT_MS);
  chttp_deadline_t wait_dl = _deadline_earlier(continue_dl, *overall);

  char *leftover = NULL;
  size_t leftover_len = 0;
  prv = _chttp_read_message_loop(conn, pctx, &wait_dl, NULL, 0, 100,
                                 keep_alive_out, any_bytes_read_out, &leftover,
                                 &leftover_len);
  if (prv == ccol_timed_out) {
    /* No interim response within the wait window; but the abandoned
     * interim parse attempt may still have left partial state on pctx (a
     * status line parsed without ever reaching CHTTP1_PAUSED, in the
     * pathological case of a server splitting even the interim response's
     * own few bytes across multiple slow writes); reset before reusing pctx
     * for the real, unrelated final response, exactly as the "100 Continue
     * seen" branch below already does. */
    ccol_retval_t rrv = _parse_ctx_reset_for_continue(pctx);
    if (rrv != ccol_success) return rrv;
    prv = _chttp_send_all(conn, wire + header_len, body_len, overall);
    if (prv != ccol_success) return prv;
    /* The abandoned interim read above may have already set
     * *any_bytes_read_out true (a partial "100 Con..." fragment arrived
     * before the wait window expired); that must not leak into the
     * unrelated final response read below, or a genuinely failed final read
     * on a reused connection would be mistaken for "some response bytes
     * already handed to the caller" and skip the safe retry-once fallback.
     * Mirrors the identical reset the "100 Continue seen" branch below
     * already does before its own final read. */
    *any_bytes_read_out = false;
    return _chttp_read_response(conn, pctx, overall, keep_alive_out,
                                any_bytes_read_out);
  }
  if (prv != ccol_success) {
    _mem_free(pctx->mp, leftover);
    return prv;
  }

  if (pctx->status_code != 100) {
    /* Server answered directly; this already IS the final response and the
     * body must never be sent. This connection can never be safely reused
     * regardless of what chttp1_should_keep_alive() concluded from the
     * response's own Connection header: RFC 7231 SS5.1.1 only SHOULDs a
     * server close the connection after rejecting a request this way, it
     * does not REQUIRE it, so a compliant server can perfectly legally
     * leave the connection open while still expecting the body this hop
     * never sent. Pooling it anyway would let the next unrelated request
     * on this client write its own bytes onto a connection the server is
     * still parsing as this hop's leftover body, desyncing the two
     * requests on a shared, reused connection. Any bytes past the
     * response's own boundary are additionally genuine trailing garbage
     * (nothing legitimate can follow a final response on a connection
     * whose body was never sent), tracked here purely for diagnostics. */
    *keep_alive_out = false;
    if (leftover_len > 0) pctx->trailing_garbage = true;
    _mem_free(pctx->mp, leftover);
    return ccol_success;
  }

  prv = _parse_ctx_reset_for_continue(pctx);
  if (prv != ccol_success) {
    _mem_free(pctx->mp, leftover);
    return prv;
  }

  prv = _chttp_send_all(conn, wire + header_len, body_len, overall);
  if (prv != ccol_success) {
    _mem_free(pctx->mp, leftover);
    return prv;
  }
  /* The body has now been handed to a connection the server itself just
   * confirmed (via "100 Continue") it was alive and ready to read from; see
   * this function's own doc comment for why that disqualifies the caller's
   * usual reused-connection retry-once safety net regardless of what the
   * final read below does next. */
  *retry_unsafe_out = true;

  *any_bytes_read_out = false;
  prv = _chttp_read_response_carry(conn, pctx, overall, leftover, leftover_len,
                                   keep_alive_out, any_bytes_read_out);
  _mem_free(pctx->mp, leftover);
  return prv;
}

static ccol_retval_t chttp_do_internal(
    struct chttpclient *cli, const chttp_request_t *req, bool streaming,
    chttpcli_write_fn user_write_fn, void *user_write_ctx,
    chttpcli_response **resp_out, int *status_code_out) {
  /* resp_out is checked and *resp_out is NULLed FIRST, before the cli/req
   * checks below, so a NULL cli/req/user_write_fn failure ALSO leaves
   * *resp_out deterministically NULL, not just failures past this point.
   * Every one of this function's many later early-return/break failure
   * paths then leaves *resp_out untouched, correctly, since it is already
   * NULL from here on rather than garbage from the caller's own
   * uninitialized local. chttpclient_resp_free() is documented as safe to
   * call with NULL specifically to license an unconditional-free cleanup
   * idiom (`resp = NULL; rv = chttpclient_do(...); ...;
   * chttpclient_resp_free(resp);`), and chttpclient_do_pooled/_streaming
   * (Tier 3) already do exactly this; Tier 1 not doing the same was a real
   * gap, not a documented contract. */
  if (!streaming) {
    if (!resp_out) return ccol_invalid_args;
    *resp_out = NULL;
  }
  if (!cli || !req) return ccol_invalid_args;
  if (streaming && !user_write_fn) return ccol_invalid_args;

  /* request_timeout_ms is read, and the overall deadline anchored, BEFORE
   * acquiring a concurrency-limiter slot below: chttpclient_set_request_
   * timeout's documented contract is "the maximum time from when
   * chttpclient_do is called...", not "...from when a pool slot becomes
   * available", so time spent blocked on a saturated pool
   * (chttpclient_set_pool_size) must count against it too. _slot_acquire is
   * itself deadline-aware for exactly this reason; see its own comment. */
  long request_timeout_ms;
  mutex_lock(cli->lock);
  request_timeout_ms = cli->request_timeout_ms;
  mutex_unlock(cli->lock);
  chttp_deadline_t overall_dl = _deadline_make(request_timeout_ms);

  ccol_retval_t rv = _slot_acquire(cli, &overall_dl);
  if (rv != ccol_success) return rv;

  ccol_memmgmt_procs_t *mp = cli->m_procs;

  long connect_timeout_ms;
  chttp_tls_config_t tls_cfg;
  ctls_ctx_t *tls_ctx;
  bool tls_ctx_usable;
  size_t max_response_body_size;
  mutex_lock(cli->lock);
  connect_timeout_ms = cli->connect_timeout_ms;
  tls_cfg = cli->tls;
  tls_ctx = cli->tls_ctx;
  tls_ctx_usable = cli->tls_ctx_usable;
  max_response_body_size = cli->max_response_body_size;
  if (tls_ctx)
    ctls_ctx_retain(
        tls_ctx); /* pin: a concurrent set_tls must not free this under us */
  mutex_unlock(cli->lock);

  char *cur_url = ccol_strdup(mp, req->url);
  if (!cur_url) {
    if (tls_ctx) ctls_ctx_release(tls_ctx);
    _slot_release(cli);
    return ccol_not_enough_memory;
  }

  chttp_request_body_t empty_body = CHTTP_NO_BODY;
  chttp_method_t cur_method = req->method;
  chttp_request_body_t cur_body = req->body;

  /* Auto-injected-from-userinfo Authorization, carried across hops as long
   * as the origin (scheme+host+port) doesn't change; dropped permanently
   * (curl's own default behavior, without opting into trusted-redirect
   * credential forwarding) the first time it does, and never re-acquired
   * even if a later hop circles back to the original origin. */
  char *carried_auth = NULL;
  char *carried_auth_origin = NULL;

  /* A caller-supplied (chttp_request_set_header) Authorization header has no
   * origin of its own to track the way carried_auth above does; it is
   * dropped, permanently, the first time a redirect crosses to a different
   * origin than the one the ORIGINAL request targeted, matching curl's own
   * CVE-2018-1000007-hardened default (a user-set Authorization header must
   * not be forwarded to a different host on redirect). initial_origin_key is
   * captured once, from hop 0's own URL; explicit_auth_suppressed latches
   * true (and stays true) the first time a later hop's origin differs from
   * it. See _serialize_request's suppress_explicit_authorization parameter
   * and its own has_auth adjustment for how this interacts with the
   * per-hop userinfo-derived case above. */
  char *initial_origin_key = NULL;
  bool explicit_auth_suppressed = false;

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
    } else if (!explicit_auth_suppressed &&
               strcmp(initial_origin_key, url.origin_key) != 0) {
      explicit_auth_suppressed = true;
    }

    if (url.is_https && !tls_ctx_usable) {
      /* Configured cert/key/ca path(s) were not readable (or ctls itself
       * failed to load/parse them) at set_tls time; that failure was
       * deferred here rather than aborting the process (see
       * _rebuild_tls_ctx_locked). ccol_http_tls_cert_load_failed, not
       * ccol_http_tls_handshake_failed: no handshake was ever attempted,
       * and ctls.c's own ctls_ctx_cert_add/_trust already report exactly
       * this failure mode with this exact code (see ctls.h); reusing it
       * here rather than a generic handshake-failure code lets a caller
       * distinguish "my local cert/key/CA file is bad" from "the peer
       * failed the handshake" or "the peer's certificate didn't verify". */
      _url_free(mp, &url);
      result = ccol_http_tls_cert_load_failed;
      break;
    }

    const char *effective_auth = NULL;
    if (url.userinfo_authorization) {
      effective_auth = url.userinfo_authorization;
    } else if (carried_auth &&
               strcmp(carried_auth_origin, url.origin_key) == 0) {
      effective_auth = carried_auth;
    }
    if (url.userinfo_authorization) {
      char *na = ccol_strdup(mp, url.userinfo_authorization);
      char *no = ccol_strdup(mp, url.origin_key);
      if (!na || !no) {
        _mem_free(mp, na);
        _mem_free(mp, no);
        _url_free(mp, &url);
        result = ccol_not_enough_memory;
        break;
      }
      _mem_free(mp, carried_auth);
      _mem_free(mp, carried_auth_origin);
      carried_auth = na;
      carried_auth_origin = no;
    } else if (carried_auth &&
               strcmp(carried_auth_origin, url.origin_key) != 0) {
      _mem_free(mp, carried_auth);
      _mem_free(mp, carried_auth_origin);
      carried_auth = NULL;
      carried_auth_origin = NULL;
    }

    chttp_request_t hop_req = *req;
    hop_req.method = cur_method;
    hop_req.body = cur_body;

    char *wire = NULL;
    size_t wire_len = 0;
    chttp_header_presence_t hop_hp = {0};
    prv =
        _serialize_request(mp, &hop_req, &url, effective_auth,
                           explicit_auth_suppressed, &wire, &wire_len, &hop_hp);
    if (prv != ccol_success) {
      _url_free(mp, &url);
      result = prv;
      break;
    }

    chttp_deadline_t connect_dl = _deadline_make(connect_timeout_ms);

    chttp_conn_t conn;
    bool reused = _idle_pool_take(cli, url.origin_key, &conn);
    if (!reused) {
      prv = _conn_open(mp, &url, url.is_https, tls_ctx, tls_cfg.verify_host,
                       &connect_dl, &overall_dl, &conn);
      if (prv != ccol_success) {
        _mem_free(mp, wire);
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
    pctx.headers = chmap_create_full(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,
                                     ccol_string, ccol_string, mp, NULL, &herr);
    if (!pctx.headers) {
      _mem_free(mp, wire);
      _conn_teardown(mp, &conn);
      _url_free(mp, &url);
      result = ccol_not_enough_memory;
      break;
    }

    bool body_carrying_method =
        (cur_method == CHTTP_POST || cur_method == CHTTP_PUT ||
         cur_method == CHTTP_PATCH);
    /* Mirrors _serialize_request's own "only emit expect: 100-continue when
     * the caller hasn't already set an explicit Expect header" check
     * (chttp_header_presence_t.has_expect); without this, a caller who sets
     * both req->expect_continue and their own non-"100-continue" Expect
     * header gets no "expect:" line on the wire at all (correctly
     * suppressed) but this hop still stalls for CHTTP_100_CONTINUE_WAIT_MS
     * waiting for an interim response that, by construction, can never
     * arrive. Reuses hop_hp (the presence struct _serialize_request already
     * computed over this exact same header map, a few lines above) instead
     * of running a second full scan over req->headers here. */
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
      /* The reused connection may have died between our liveness probe and
       * this attempt; either the write silently succeeded into the local
       * send buffer before the peer's close became visible, or the read
       * never produced a single byte. Either way nothing has been parsed or
       * handed to the caller yet, so it is safe to retry exactly once
       * against a brand-new connection. (retry_unsafe overrides this: it
       * means the body was already sent to this connection AFTER an
       * explicit "100 Continue" proved the connection and server were both
       * alive moments earlier, so a retry would risk the server processing
       * a non-idempotent body twice; see _chttp_send_and_read's own doc
       * comment.) */
      _conn_teardown(mp, &conn);
      /* Fresh connect deadline for this retry's brand-new connection, NOT
       * the hop-level `connect_dl` computed above (which covers only the
       * `!reused` fresh-connect branch and is otherwise never consumed
       * against wall-clock time before this point). The failed attempt
       * against the dead reused connection just above went through
       * _chttp_send_and_read, bounded only by `overall_dl`, and can
       * legitimately take a non-trivial amount of time to fail (e.g. a
       * half-open/blackholed peer that never responds until some of
       * overall_dl elapses); reusing the stale `connect_dl` here would let
       * that unrelated elapsed time silently eat into the fresh
       * connection's own configured connect_timeout_ms budget, causing this
       * retry's connect attempt to time out (or get much less than
       * connect_timeout_ms) even though a full, fresh budget was
       * configured. Mirrors _async_retry_hop's identical fix for the exact
       * same Tier 2/3 scenario (see that function's own connect_deadline
       * field comment). */
      chttp_deadline_t retry_connect_dl = _deadline_make(connect_timeout_ms);
      prv = _conn_open(mp, &url, url.is_https, tls_ctx, tls_cfg.verify_host,
                       &retry_connect_dl, &overall_dl, &conn);
      if (prv == ccol_success) {
        reused = false;
        prv = _chttp_send_and_read(&conn, wire, wire_len, cur_body.len,
                                   use_100_continue, &overall_dl, &pctx,
                                   &keep_alive, &any_bytes_read, &retry_unsafe);
      }
    }
    _mem_free(mp, wire);
    if (prv != ccol_success) {
      _conn_teardown(mp, &conn);
      _parse_ctx_free_fields(&pctx);
      _mem_free(mp, bb.buf);
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
        /* This response is itself the 51st request in the chain and is
         * ALSO a redirect; following it would exceed the 50-redirect
         * budget. Report it as an error instead of silently delivering it
         * (the deliberate design this codebase shipped with previously,
         * reversed per an explicit request: a caller relying on
         * ccol_http_too_many_redirects to detect a redirect loop needs an
         * actual error here, not a stale 3xx response it has to notice and
         * interpret itself). */
        _parse_ctx_free_fields(&pctx);
        _mem_free(mp, bb.buf);
        _url_free(mp, &url);
        result = ccol_http_too_many_redirects;
        break;
      }
      char *next_url = _resolve_redirect_url(mp, &url, pctx.location);
      bool preserve = (pctx.status_code == 307 || pctx.status_code == 308);
      _parse_ctx_free_fields(&pctx);
      _mem_free(mp, bb.buf);
      _url_free(mp, &url);
      _mem_free(mp, cur_url);
      /* NULLed immediately after freeing (not just reassigned on the
       * success path below): a redirect status with an empty or otherwise
       * unresolvable Location header (RFC 3986 SS5.2/5.3 resolution
       * failure, not just OOM; _resolve_redirect_url's very first check
       * is `if (!location || !*location) return NULL;`, trivially
       * reachable via a plain server response, no malformed input needed)
       * makes next_url NULL and falls through to the post-loop cleanup's
       * own _mem_free(mp, cur_url) below with this pointer still holding
       * the just-freed value; a real, remotely-triggerable double-free
       * caught by clang's static analyzer, not by any dynamic test (no
       * existing mock route sends a redirect with an empty Location). */
      cur_url = NULL;

      if (!next_url) {
        result = ccol_http_transfer_aborted;
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

    chttpcli_response *resp =
        (chttpcli_response *)_mem_calloc(mp, 1, sizeof(chttpcli_response));
    if (!resp) {
      _parse_ctx_free_fields(&pctx);
      _mem_free(mp, bb.buf);
      _url_free(mp, &url);
      result = ccol_not_enough_memory;
      break;
    }
    resp->_m_procs = mp;
    resp->status_code = pctx.status_code;
    resp->body = bb.buf;
    resp->body_len = bb.len;
    resp->headers = pctx.headers;
    pctx.headers = NULL; /* ownership transferred to resp */
    _parse_ctx_free_fields(&pctx);
    _url_free(mp, &url);
    *resp_out = resp;
    result = ccol_success;
    break;
  }

  _mem_free(mp, cur_url);
  _mem_free(mp, carried_auth);
  _mem_free(mp, carried_auth_origin);
  _mem_free(mp, initial_origin_key);
  if (tls_ctx) ctls_ctx_release(tls_ctx);
  _slot_release(cli);
  return result;
}

ccol_retval_t chttpclient_do(chttpcli h, const chttp_request_t *req,
                             chttpcli_response **resp_out) {
  struct chttpclient *raw = _chttpcli_resolve(h);
  if (!raw) return ccol_invalid_args;
  ccol_retval_t rv =
      chttp_do_internal(raw, req, false, NULL, NULL, resp_out, NULL);
  /* chttp_do_internal, above, is called completely unmodified: it still
   * runs its own existing _slot_acquire/_slot_release pair internally,
   * bracketing the whole request including the actual blocking network
   * I/O, exactly as today. */
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
  atomic_store(&default_client_bundler.client, create_chttpclient(NULL));
}

chttpcli chttp_default_client(void) {
  call_once(default_client_bundler.once, _init_default_client);
  return atomic_load(&default_client_bundler.client);
}

__attribute__((destructor)) static void _cleanup_default_client(void) {
  /* Clear first, then destroy: __chttpclient_destroy's own defensive
   * compare-and-swap (see its doc comment) would otherwise race this
   * function's own read of the handle in the vanishingly unlikely case
   * another thread is concurrently destroying the same handle at process
   * exit; clearing here first makes that CAS in __chttpclient_destroy a
   * guaranteed no-op instead of a second racing writer. */
  chttpcli cli = atomic_exchange(&default_client_bundler.client, 0);
  if (cli) __chttpclient_destroy(cli); /* unchanged */

  /* The one client this library itself might still own has just been
   * destroyed and its slot released. Free the slot table's own bookkeeping
   * arrays so make memtest's --show-leak-kinds=all does not report them as
   * still-reachable - but ONLY if no other, application-owned chttpcli
   * handle is still in-use. This library does not control
   * __attribute__((destructor)) ordering across a process's various shared
   * objects/atexit handlers, so an application that relies on process exit
   * to reclaim a client it created directly (rather than calling
   * chttpclient_destroy itself) may still have a live handle touched by a
   * destructor/atexit handler that happens to run after this one
   * (chttpclient_destroy/_do/_set_*, or even create_chttpclient again).
   * Freeing the shared slot table out from under a still-live handle would
   * turn that into a use-after-free; skipping the free instead leaves
   * exactly the same already-accepted "caller never destroyed their client"
   * leak this comment already documents for the struct itself, just now
   * covering the slot table's bookkeeping arrays too.
   *
   * MUST call_once here too, even though the line above already might have:
   * if cli was 0 (the default client was never created in this process at
   * all; e.g. an application that links this library only for
   * cvector/chashmap/chttpserver and never touches chttpclient), the
   * `if (cli) __chttpclient_destroy(cli);` line is skipped entirely, meaning
   * call_once was never invoked anywhere in this process, and the block
   * below would lock a never-pthread_mutex_init'd mutex. This is a
   * deterministic trigger, not a rare race: it fires on 100% of runs of any
   * process that links this .so without ever creating a chttpcli handle;
   * __attribute__((destructor)) functions run unconditionally for the whole
   * shared object regardless of which parts of it were actually used. */
  call_once(chttpcli_slot_table.once, _chttpcli_slot_table_init_globals);
  rw_lock_wrlock(chttpcli_slot_table.rwlock);
  bool any_slot_in_use = false;
  size_t slot_count = cvector_elem_count(chttpcli_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++) {
    chttpcli_slot_t *slot =
        (chttpcli_slot_t *)cvector_at(chttpcli_slot_table.slots, i);
    if (slot->in_use) {
      any_slot_in_use = true;
      break;
    }
  }
  if (!any_slot_in_use) {
    __cvector_destroy(chttpcli_slot_table.slots);
    __cvector_destroy(chttpcli_slot_table.free_indices);
  }
  rw_lock_unlock(chttpcli_slot_table.rwlock);
}

/* ========================================================================== */
/*                         CONVENIENCE API                                    */
/* ========================================================================== */

ccol_retval_t chttp_do(const chttp_request_t *req,
                       chttpcli_response **resp_out) {
  /* Checked and NULLed here, before chttp_default_client() rather than
   * deferred to chttpclient_do's own identical check: the !cli early return
   * just below would otherwise leave *resp_out untouched on that path, the
   * same class of gap chttp_do_internal itself was fixed for (see its own
   * comment). */
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;
  chttpcli cli = chttp_default_client();
  if (!cli) return ccol_unexpected_failure;
  return chttpclient_do(cli, req, resp_out);
}

ccol_retval_t chttp_run_query(chttp_method_t method, const char *url,
                              const chttp_request_body_t *body, chmap headers,
                              chttpcli_response **resp_out) {
  /* resp_out is checked and *resp_out is NULLed before the url check
   * specifically so a NULL-url failure ALSO leaves *resp_out
   * deterministically NULL, not just failures past this point; see
   * chttp_do_internal's own identical ordering rationale. */
  if (!resp_out) return ccol_invalid_args;
  *resp_out = NULL;
  if (!url) return ccol_invalid_args;
  chttp_request_t *req = chttp_request_new(method, url, body, NULL);
  if (!req) return ccol_not_enough_memory;

  req->headers = headers;
  ccol_retval_t rv = chttp_do(req, resp_out);
  req->headers =
      NULL; /* headers not owned by req; do not let free destroy it */
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

const char *chttpclient_resp_header(const chttpcli_response *resp,
                                    const char *name) {
  if (!resp || !name || !resp->headers) return NULL;

  /* A caller doing several known-name header lookups per response (a
   * realistic, likely hot pattern) previously paid one heap _mem_alloc/
   * _mem_free pair per lookup just to hold the lower-cased copy of `name`.
   * Every real HTTP header name (RFC 7230 SS3.2's "token" grammar; every
   * registered name in the IANA registry, and any realistic custom one) is
   * far shorter than this stack buffer, so the allocation is skipped
   * entirely in the overwhelmingly common case; only a pathologically long
   * name (longer than this buffer could ever legitimately be, since actual
   * response headers of that length would themselves be rejected/unusual
   * long before reaching this call) falls back to the heap, exactly as
   * before. */
  enum { STACK_NAME_BUF = 128 };
  char stack_lower[STACK_NAME_BUF];
  size_t nlen = strlen(name);
  char *lower;
  char *heap_lower = NULL;
  if (nlen < STACK_NAME_BUF) {
    lower = stack_lower;
  } else {
    heap_lower = (char *)_mem_alloc(resp->_m_procs, nlen + 1);
    if (!heap_lower) return NULL;
    lower = heap_lower;
  }
  for (size_t i = 0; i <= nlen; i++)
    lower[i] = (char)tolower((unsigned char)name[i]);

  cmap_pair kp = {.ptr = lower, .size = nlen + 1};
  cmap_pair *vp = NULL;
  ccol_retval_t rv = chmap_get_elem_ref((chmap)resp->headers, &kp, &vp);
  if (heap_lower) _mem_free(resp->_m_procs, heap_lower);

  if (rv != ccol_success || !vp) return NULL;
  return (const char *)vp->ptr;
}

void chttpclient_resp_free(chttpcli_response *resp) {
  if (!resp) return;
  ccol_memmgmt_procs_t *mp = resp->_m_procs;
  _mem_free(mp, resp->body);
  if (resp->headers) __chmap_destroy((chmap)resp->headers);
  _mem_free(mp, resp);
  /* mp is NOT freed: it is owned by the client, not the response. */
}
