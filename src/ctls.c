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
#include <errno.h>
#include <fcntl.h>
#include <internal/cascii.h>
#include <internal/chashinsert.h>
#include <internal/csock.h>
#include <internal/ctls.h>
#include <internal/test_helpers/ctls.h>
#include <limits.h>
#include <netinet/in.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/auxv.h>
#endif

// Process-wide self-signed root key
//
// The RSA key every self-signed certificate of this module is signed with,
// made once per process on first need. It belongs to the process, not to a
// context, so a once flag and its own mutex guard it; finalized stops a new
// key after the exit handler freed it.
static struct {
  EVP_PKEY *key;
  ccol__mutex mutex;
  ccol__once_flag once;
  bool finalized;
} ctls_root_key_bundle = {.once = CCOL__ONCE_INIT};

// Initializes the root key mutex; runs once through the bundle's once flag
// before the first use of the key.
static void ctls_root_key_globals_init(void) {
  if (ccol__mutex_init(ctls_root_key_bundle.mutex) != 0)
    ccol__fatal_err("ctls root key: failed to initialize mutex");
}

// The certificates and CRLs of the platform's default CA file, parsed once per
// process at the first ctls_ctx_trust_system() and shared by every context
// that trusts the system store. The list never changes once stored; each
// store holds its own references, so freeing it at exit leaves live contexts
// intact. The mutex makes concurrent first users parse once and orders every
// read against the free at exit.
static struct {
  STACK_OF(X509_INFO) * infos;
  ccol__mutex mutex;
  ccol__once_flag once;
  bool finalized;
} ctls_system_ca_bundle = {.once = CCOL__ONCE_INIT};

// Initializes the system CA mutex; runs once before the first use of the
// list.
static void ctls_system_ca_globals_init(void) {
  if (ccol__mutex_init(ctls_system_ca_bundle.mutex) != 0)
    ccol__fatal_err("ctls system CA bundle: failed to initialize mutex");
}

// Frees the root key and the parsed default CA file at exit, so valgrind finds
// nothing of ours left. It's registered after OpenSSL's own cleanup, so it
// runs first; a destructor would run too late. It sets finalized in both
// bundles: Afterwards no root key is made, and a system-trust context parses
// the file for itself. A caller holding a key reference keeps it.
static void ctls_process_state_at_exit(void) {
  ccol__call_once(ctls_root_key_bundle.once, ctls_root_key_globals_init);
  ccol__mutex_lock(ctls_root_key_bundle.mutex);
  EVP_PKEY_free(ctls_root_key_bundle.key);
  ctls_root_key_bundle.key = NULL;
  ctls_root_key_bundle.finalized = true;
  ccol__mutex_unlock(ctls_root_key_bundle.mutex);

  ccol__call_once(ctls_system_ca_bundle.once, ctls_system_ca_globals_init);
  ccol__mutex_lock(ctls_system_ca_bundle.mutex);
  sk_X509_INFO_pop_free(ctls_system_ca_bundle.infos, X509_INFO_free);
  ctls_system_ca_bundle.infos = NULL;
  ctls_system_ca_bundle.finalized = true;
  ccol__mutex_unlock(ctls_system_ca_bundle.mutex);
}

// Makes the exit handler register only once.
static ccol__once_flag ctls_process_state_exit_once = CCOL__ONCE_INIT;

// Registers ctls_process_state_at_exit with atexit(); runs once.
static void ctls_process_state_exit_register(void) {
  (void)atexit(ctls_process_state_at_exit);
}

// Registers the exit handler once. Call it only after an OpenSSL call has run,
// so OpenSSL's cleanup was registered first and runs after ours.
static void ctls_process_state_exit_arm(void) {
  ccol__call_once(ctls_process_state_exit_once,
                  ctls_process_state_exit_register);
}

#ifdef RUNNING_UNIT_TESTS
// Tells whether the root key is still held, so the test suite can check that
// the exit handler frees it.
bool ctls__root_key_held_for_tests(void) {
  return __atomic_load_n(&ctls_root_key_bundle.key, __ATOMIC_SEQ_CST) != NULL;
}
#endif

// Gives a reference to the process root key (free it with EVP_PKEY_free()),
// making a 2048-bit RSA key under the bundle mutex on first use, for the
// self-signed certificate code. It uses EVP_PKEY_CTX key generation, which
// neither OpenSSL 3 nor LibreSSL deprecates. It gives NULL when generation
// fails (the next call tries again) or after the exit handler ran, and arms
// the exit handler when it makes the key.
static EVP_PKEY *ctls_get_root_key(void) {
  ccol__call_once(ctls_root_key_bundle.once, ctls_root_key_globals_init);
  ccol__mutex_lock(ctls_root_key_bundle.mutex);
  if (!ctls_root_key_bundle.key && !ctls_root_key_bundle.finalized) {
    EVP_PKEY *key = NULL;
    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (!kctx || EVP_PKEY_keygen_init(kctx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048) <= 0 ||
        EVP_PKEY_keygen(kctx, &key) <= 0) {
      EVP_PKEY_free(key);
      key = NULL;
    }
    EVP_PKEY_CTX_free(kctx);
    ctls_root_key_bundle.key = key;
    if (key) ctls_process_state_exit_arm();
  }
  EVP_PKEY *key = ctls_root_key_bundle.key;
  if (key && EVP_PKEY_up_ref(key) != 1) key = NULL;
  ccol__mutex_unlock(ctls_root_key_bundle.mutex);
  return key;
}

// Connection ex_data index
//
// The SSL ex_data index that maps an SSL object to its ctls_conn, for the SNI
// and ALPN callbacks. idx starts at -1, the "not registered" sentinel and also
// SSL_get_ex_new_index()'s failure value: 0 would read as registered and
// share index 0 with SSL_set_app_data(), so other code holding the SSL could
// read and overwrite our pointer.
static struct {
  int idx;
  ccol__mutex mutex;
  ccol__once_flag once;
} ctls_conn_ex_idx_bundle = {.idx = -1};

#ifdef RUNNING_UNIT_TESTS
// Counts the index lookups that took the mutex, for a white-box test.
static long g_ctls_ex_idx_locked_calls_for_tests = 0;
// Gives the locked lookup count, so a test can check that a registered index
// is read without the lock.
long ctls__ex_idx_locked_calls_for_tests(void) {
  return __atomic_load_n(&g_ctls_ex_idx_locked_calls_for_tests,
                         __ATOMIC_RELAXED);
}
#endif

// Initializes the ex_data index mutex; runs once.
static void ctls_ex_idx_globals_init(void) {
  if (ccol__mutex_init(ctls_conn_ex_idx_bundle.mutex) != 0)
    ccol__fatal_err("ctls conn ex_data index: failed to initialize mutex");
}

// Gives the connection ex_data index, registering it on first use. Every
// connection create and every SNI and ALPN callback asks for it, so a
// registered index is read with one acquire load and no lock; only a call that
// finds -1 takes the mutex, registers and publishes with a release store. It
// gives -1 when registration fails, and the next call tries again.
static int ctls_conn_ex_idx(void) {
  int idx = __atomic_load_n(&ctls_conn_ex_idx_bundle.idx, __ATOMIC_ACQUIRE);
  if (__builtin_expect(idx >= 0, 1)) return idx;
#ifdef RUNNING_UNIT_TESTS
  __atomic_fetch_add(&g_ctls_ex_idx_locked_calls_for_tests, 1,
                     __ATOMIC_RELAXED);
#endif
  ccol__call_once(ctls_conn_ex_idx_bundle.once, ctls_ex_idx_globals_init);
  ccol__mutex_lock(ctls_conn_ex_idx_bundle.mutex);
  idx = __atomic_load_n(&ctls_conn_ex_idx_bundle.idx, __ATOMIC_RELAXED);
  if (idx < 0) {
    idx = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
    __atomic_store_n(&ctls_conn_ex_idx_bundle.idx, idx, __ATOMIC_RELEASE);
  }
  ccol__mutex_unlock(ctls_conn_ex_idx_bundle.mutex);
  return idx;
}

// Socket BIO that never raises SIGPIPE
//
// The BIO_METHOD every connection reads and writes through. It acts like
// OpenSSL's socket BIO but writes with send(2) and MSG_NOSIGNAL, so a write to
// a gone peer (the close_notify of ctls_conn_destroy() included) fails with
// EPIPE instead of killing the process, without touching the application's
// signal disposition. Each BIO holds a count on the method (live_bios); the
// exit destructor frees an unused method, or marks it retired so the last BIO
// frees it, because a later destructor can still close a connection. The
// mutex is never destroyed, since a release can come after the destructor.
static struct {
  BIO_METHOD *meth;
  size_t live_bios;
  bool retired;
  ccol__mutex mutex;
  ccol__once_flag once;
} ctls_sock_bio_bundle = {.once = CCOL__ONCE_INIT};

// Initializes the socket BIO mutex; runs once.
static void ctls_sock_bio_globals_init(void) {
  if (ccol__mutex_init(ctls_sock_bio_bundle.mutex) != 0)
    ccol__fatal_err("ctls socket BIO: failed to initialize mutex");
}

// The BIO flag that records end of file, read back through BIO_CTRL_EOF.
// OpenSSL names it; on LibreSSL we use a bit no LibreSSL flag takes.
#ifdef BIO_FLAGS_IN_EOF
#define CTLS_SOCK_BIO_EOF BIO_FLAGS_IN_EOF
#else
#define CTLS_SOCK_BIO_EOF 0x800
#endif

// Follows the TLS record framing of the bytes the socket BIO hands up.
// ctls_conn_read() uses it to tell whether a peer closed without close_notify
// on a record boundary, and on LibreSSL (no SSL_has_pending())
// ctls_conn_has_pending_input() uses it to tell whether the TLS layer holds a
// partial record. With read-ahead off, the TLS layer holds part of a record
// exactly while hdr_got isn't 0. A first header byte outside 20 to 24 means
// the framing isn't one we follow, and the tracker gives up (lost), which
// reports pending input from then on; that's always safe.
typedef struct {
  uint32_t body_left;
  uint8_t hdr_got;
  uint8_t len_hi;
  bool lost;
} ctls_rec_track;

// Advances the record tracker rt over n bytes at p that the BIO just read:
// counts header bytes, takes the body length from bytes 3 and 4, and skips
// body bytes. It sets lost on a bad content type.
static void ctls_rec_track_feed(ctls_rec_track *rt, const char *p, int n) {
  while (n > 0 && !rt->lost) {
    if (rt->hdr_got < 5) {
      uint8_t c = (uint8_t)*p++;
      n--;
      if (rt->hdr_got == 0 && (c < 20 || c > 24)) {
        rt->lost = true;
        return;
      }
      if (rt->hdr_got == 3) rt->len_hi = c;
      if (rt->hdr_got == 4) rt->body_left = (uint32_t)rt->len_hi << 8 | c;
      if (++rt->hdr_got == 5 && rt->body_left == 0) rt->hdr_got = 0;
    } else {
      uint32_t take = (uint32_t)n < rt->body_left ? (uint32_t)n : rt->body_left;
      p += take;
      n -= (int)take;
      rt->body_left -= take;
      if (rt->body_left == 0) rt->hdr_got = 0;
    }
  }
}

#ifdef RUNNING_UNIT_TESTS
// Feeds count chunks of the given lens to a fresh tracker and gives whether it
// would report pending input, so a test can drive record boundaries
// directly.
bool ctls__rec_track_pending_after_for_tests(const char *const *chunks,
                                             const int *lens, int count) {
  ctls_rec_track rt = {0};
  for (int i = 0; i < count; i++) ctls_rec_track_feed(&rt, chunks[i], lens[i]);
  return rt.hdr_got != 0 || rt.lost;
}
#endif

// Gives the descriptor of BIO b, kept in its data pointer so a write needs no
// control call.
static inline int ctls_sock_bio_fd(BIO *b) {
  return (int)(intptr_t)BIO_get_data(b);
}

// The BIO write: Sends inl bytes with MSG_NOSIGNAL and sets the retry flag
// when the socket would block. Gives the bytes sent or -1.
static int ctls_sock_bio_write(BIO *b, const char *in, int inl) {
  errno = 0;
  ssize_t n = send(ctls_sock_bio_fd(b), in, (size_t)inl, CCOL_MSG_NOSIGNAL);
  int ret = n < 0 ? -1 : (int)n;
  BIO_clear_retry_flags(b);
  if (ret <= 0 && BIO_sock_should_retry(ret)) BIO_set_retry_write(b);
  return ret;
}

// The BIO read: Receives up to outl bytes, sets the retry flag when the
// socket would block or the EOF flag at end of stream, and feeds what it read
// to the connection's record tracker, which the connection hands over through
// the BIO's callback argument (this BIO has no callback). Gives the bytes
// read, 0 at EOF or -1.
static int ctls_sock_bio_read(BIO *b, char *out, int outl) {
  if (!out) return 0;
  errno = 0;
  ssize_t n = recv(ctls_sock_bio_fd(b), out, (size_t)outl, 0);
  int ret = n < 0 ? -1 : (int)n;
  BIO_clear_retry_flags(b);
  if (ret <= 0) {
    if (BIO_sock_should_retry(ret))
      BIO_set_retry_read(b);
    else if (ret == 0)
      BIO_set_flags(b, CTLS_SOCK_BIO_EOF);
  }
  ctls_rec_track *rt = (ctls_rec_track *)BIO_get_callback_arg(b);
  if (ret > 0 && rt) ctls_rec_track_feed(rt, out, ret);
  return ret;
}

// The BIO puts: Writes the string str through ctls_sock_bio_write().
static int ctls_sock_bio_puts(BIO *b, const char *str) {
  return ctls_sock_bio_write(b, str, (int)strlen(str));
}

// The BIO control call. It handles what OpenSSL's record layer and helpers
// ask of a socket BIO: Setting and getting the fd and the close flag, the EOF
// flag (how an abrupt close is told from a retryable read), and the poll
// descriptors behind SSL_get_rpoll_descriptor(). Everything else gives 0.
static long ctls_sock_bio_ctrl(BIO *b, int cmd, long num, void *ptr) {
  switch (cmd) {
    case BIO_C_SET_FD:
      if (BIO_get_shutdown(b) && BIO_get_init(b)) close(ctls_sock_bio_fd(b));
      BIO_clear_flags(b, ~0);
      BIO_set_data(b, (void *)(intptr_t)*(int *)ptr);
      BIO_set_shutdown(b, (int)num);
      BIO_set_init(b, 1);
      return 1;
    case BIO_C_GET_FD:
      if (!BIO_get_init(b)) return -1;
      if (ptr) *(int *)ptr = ctls_sock_bio_fd(b);
      return ctls_sock_bio_fd(b);
    case BIO_CTRL_GET_CLOSE:
      return BIO_get_shutdown(b);
    case BIO_CTRL_SET_CLOSE:
      BIO_set_shutdown(b, (int)num);
      return 1;
    case BIO_CTRL_DUP:
    case BIO_CTRL_FLUSH:
      return 1;
    case BIO_CTRL_EOF:
      return BIO_test_flags(b, CTLS_SOCK_BIO_EOF) != 0;
#ifdef BIO_CTRL_GET_RPOLL_DESCRIPTOR
    case BIO_CTRL_GET_RPOLL_DESCRIPTOR:
    case BIO_CTRL_GET_WPOLL_DESCRIPTOR: {
      if (!BIO_get_init(b)) return 0;
      BIO_POLL_DESCRIPTOR *pd = (BIO_POLL_DESCRIPTOR *)ptr;
      pd->type = BIO_POLL_DESCRIPTOR_TYPE_SOCK_FD;
      pd->value.fd = ctls_sock_bio_fd(b);
      return 1;
    }
#endif
    default:
      return 0;
  }
}

// The BIO constructor: Starts with no fd and no flags.
static int ctls_sock_bio_create(BIO *b) {
  BIO_set_init(b, 0);
  BIO_set_data(b, NULL);
  BIO_clear_flags(b, ~0);
  return 1;
}

// The BIO destructor: Closes the fd only when the BIO owns it (ours never do)
// and clears the state.
static int ctls_sock_bio_destroy(BIO *b) {
  if (!b) return 0;
  if (BIO_get_shutdown(b) && BIO_get_init(b)) close(ctls_sock_bio_fd(b));
  BIO_set_init(b, 0);
  BIO_clear_flags(b, ~0);
  return 1;
}

// Builds the BIO_METHOD with a new type index that carries
// BIO_TYPE_DESCRIPTOR, so BIO_find_type() and SSL_get_fd() find it. Gives
// NULL on failure.
static BIO_METHOD *ctls_sock_bio_method_build(void) {
  int type = BIO_get_new_index();
  if (type == -1) return NULL;
  BIO_METHOD *m = BIO_meth_new(
      type | BIO_TYPE_SOURCE_SINK | BIO_TYPE_DESCRIPTOR, "ccol socket");
  if (!m) return NULL;
  if (!BIO_meth_set_write(m, ctls_sock_bio_write) ||
      !BIO_meth_set_read(m, ctls_sock_bio_read) ||
      !BIO_meth_set_puts(m, ctls_sock_bio_puts) ||
      !BIO_meth_set_ctrl(m, ctls_sock_bio_ctrl) ||
      !BIO_meth_set_create(m, ctls_sock_bio_create) ||
      !BIO_meth_set_destroy(m, ctls_sock_bio_destroy)) {
    BIO_meth_free(m);
    return NULL;
  }
  return m;
}

static void ctls_sock_bio_release(void);

// Builds a socket BIO over fd that never closes fd, for a new connection. It
// first asks ccol_sock_nosigpipe() to stop SIGPIPE on the socket where that's
// per socket, and refuses the socket when it can't. It builds the method when
// there's none (also after the destructor retired one) and charges one count
// on it before unlocking, so the method can't be freed while BIO_new() uses
// it. Gives NULL on failure, and then charges nothing.
static BIO *ctls_sock_bio_new(int fd) {
  if (ccol_sock_nosigpipe(fd) != 0) return NULL;
  ccol__call_once(ctls_sock_bio_bundle.once, ctls_sock_bio_globals_init);
  ccol__mutex_lock(ctls_sock_bio_bundle.mutex);
  if (!ctls_sock_bio_bundle.meth) {
    ctls_sock_bio_bundle.meth = ctls_sock_bio_method_build();
    ctls_sock_bio_bundle.retired = false;
  }
  BIO_METHOD *meth = ctls_sock_bio_bundle.meth;
  if (meth) ctls_sock_bio_bundle.live_bios++;
  ccol__mutex_unlock(ctls_sock_bio_bundle.mutex);
  if (!meth) return NULL;
  BIO *bio = BIO_new(meth);
  if (!bio) {
    ctls_sock_bio_release();
    return NULL;
  }
  BIO_set_fd(bio, fd, BIO_NOCLOSE);
  return bio;
}

// Drops the method count of a BIO that SSL_free() already freed, and frees a
// retired method when this was its last BIO.
static void ctls_sock_bio_release(void) {
  ccol__mutex_lock(ctls_sock_bio_bundle.mutex);
  if (--ctls_sock_bio_bundle.live_bios == 0 && ctls_sock_bio_bundle.retired) {
    BIO_meth_free(ctls_sock_bio_bundle.meth);
    ctls_sock_bio_bundle.meth = NULL;
    ctls_sock_bio_bundle.retired = false;
  }
  ccol__mutex_unlock(ctls_sock_bio_bundle.mutex);
}

// Frees the BIO method at unload or exit when no BIO uses it, or marks it
// retired so the last BIO frees it.
__attribute__((destructor)) static void ctls_sock_bio_cleanup(void) {
  ccol__call_once(ctls_sock_bio_bundle.once, ctls_sock_bio_globals_init);
  ccol__mutex_lock(ctls_sock_bio_bundle.mutex);
  if (ctls_sock_bio_bundle.meth) {
    if (ctls_sock_bio_bundle.live_bios == 0) {
      BIO_meth_free(ctls_sock_bio_bundle.meth);
      ctls_sock_bio_bundle.meth = NULL;
    } else {
      ctls_sock_bio_bundle.retired = true;
    }
  }
  ccol__mutex_unlock(ctls_sock_bio_bundle.mutex);
}

#ifdef RUNNING_UNIT_TESTS
// Gives the number of live socket BIOs, so a suite can check that every
// connection gives its count back.
size_t ctls__live_sock_bio_count_for_tests(void) {
  ccol__call_once(ctls_sock_bio_bundle.once, ctls_sock_bio_globals_init);
  ccol__mutex_lock(ctls_sock_bio_bundle.mutex);
  size_t n = ctls_sock_bio_bundle.live_bios;
  ccol__mutex_unlock(ctls_sock_bio_bundle.mutex);
  return n;
}
#endif

// Internal types
//
// One ALPN registration: The protocol name, its selection callback with its
// udata, and the cleanup for that udata.
typedef struct ctls_alpn_entry {
  char *name;
  size_t name_len;
  ctls_alpn_selected_fn on_selected;
  void *udata;
  ctls_alpn_cleanup_fn on_cleanup;
} ctls_alpn_entry;

// One named certificate, dispatched by SNI: Its stored PEM data (or the
// self-signed subject) and its own fully built SSL_CTX, rebuilt whenever the
// trust store or the ALPN protocols of the context change.
typedef struct ctls_named_cert {
  char *cert_pem;
  size_t cert_len;
  char *key_pem;
  size_t key_len;
  char *pk_password;
  bool self_signed;
  char *self_signed_name;
  // NULL until the first successful rebuild.
  SSL_CTX *built_ctx;
} ctls_named_cert;

// A TLS configuration, shared by its connections. lock guards the refcount
// and every stored field; m_procs is an owned copy of the caller's procs, or
// NULL. The
// stored configuration (default certificate, named certificates, trust
// bundles, verify flags, ALPN protocols) is the source every rebuild makes
// the SSL_CTX objects from; ctx_default, the default SSL_CTX, is NULL
// before the first build.
struct ctls_ctx {
  size_t ref;
  ccol__mutex lock;
  ccol_memmgmt_procs *m_procs;

  // The default certificate, set by a server_name of NULL or "" in
  // ctls_ctx_cert_add().
  bool has_default_cert;
  char *default_cert_pem;
  size_t default_cert_len;
  char *default_key_pem;
  size_t default_key_len;
  char *default_pk_password;
  bool default_self_signed;
  char *default_self_signed_name;

  // Named certificates: A chmap from a lower-case hostname to a
  // ctls_named_cert *.
  chmap named_certs;
  bool sni_callback_installed;

  // The trusted CA bundles as PEM blobs, and the verify settings.
  char **trust_pems;
  size_t *trust_lens;
  size_t trust_count;
  size_t trust_cap;
  bool verify_default_store;
  bool verify_peer;
  // True when a server only asks for a client certificate; see
  // ctls_ctx_peer_cert_optional().
  bool peer_cert_optional;

  // The ALPN registrations, in registration order.
  ctls_alpn_entry *alpn;
  size_t alpn_count;
  size_t alpn_cap;

  SSL_CTX
  *ctx_default;
};

// One TLS connection. ctx is retained for the connection's life.
struct ctls_conn {
  SSL *ssl;
  ctls_ctx *ctx;
  bool is_server;
  bool handshake_done;
  void *udata;
  const char *alpn_selected_name;
  size_t alpn_selected_len;
  // The connection's own copy of the agreed ALPN name (1 to 255 bytes), which
  // alpn_selected_name points at. It's a copy because tls->alpn is mutable: A
  // concurrent ctls_ctx_alpn_add() can free or move the stored names.
  char alpn_selected_buf[256];
  // The direction the last read or write that gave EWOULDBLOCK needs; see
  // ctls_conn_wants_write().
  bool last_io_wants_write;
  // Our references on every SSL_CTX this connection can reach: The default
  // context it was created on (pinned in the same ctx->lock section as
  // SSL_new()) and the named context an SNI dispatch installed (NULL when there
  // was none). OpenSSL holds its own references, but these make keeping them
  // alive our guarantee. Both are released under ctx->lock, which orders the
  // connection's last use before a rebuild frees the object; ThreadSanitizer
  // can't see OpenSSL's atomic refcount, so without the lock it reports a
  // race.
  SSL_CTX *pinned_ctx_default;
  SSL_CTX *pinned_sni_ctx;
  // True while the SSL owns a BIO that holds a count on the socket BIO method.
  bool holds_sock_bio;
  // The record framing the socket BIO follows.
  ctls_rec_track rec_track;
  // The DER encoding and RFC 2253 subject of the verified peer certificate,
  // made on first request from the context's allocator. peer_info_cert
  // (referenced) names the certificate they describe, so a renegotiated
  // certificate makes them again.
  X509 *peer_info_cert;
  unsigned char *peer_der;
  size_t peer_der_len;
  char *peer_subject;
};

#ifdef RUNNING_UNIT_TESTS
// Counts the SSL_CTX pins held by live connections, so a suite can check that
// a connection pins exactly what it can reach and that the total comes back
// to its start. Several threads take and drop pins at once, so it's atomic;
// tests read it after joining their threads.
static _Atomic long ctls_live_conn_ctx_pins = 0;

// Adds delta to the pin count when a connection takes or drops pins. A build
// without tests compiles it to nothing.
static inline void ctls_account_ctx_pins(long delta) {
  atomic_fetch_add_explicit(&ctls_live_conn_ctx_pins, delta,
                            memory_order_relaxed);
}

// Gives the pin count, for the reference-balance tests.
long ctls__live_conn_ctx_pin_count_for_tests(void) {
  return atomic_load_explicit(&ctls_live_conn_ctx_pins, memory_order_relaxed);
}
#else
// The same accounting in a build without tests: It does nothing.
static inline void ctls_account_ctx_pins(long delta) { (void)delta; }
#endif

// Small helpers
//
// Copies string s into a new buffer from mp, to store a caller's names and
// passwords. NULL for a NULL s or a failed allocation.
static char *ctls_strdup(ccol_memmgmt_procs *mp, const char *s) {
  if (!s) return NULL;
  size_t len = strlen(s) + 1;
  char *d = (char *)ccol__mem_alloc(mp, len);
  if (!d) return NULL;
  memcpy(d, s, len);
  return d;
}

// Copies s in ASCII lower case, the form named certificates are stored and
// looked up in; DNS names ignore case (RFC 4343) whatever the locale. NULL
// for a NULL s or a failed allocation.
static char *ctls_strdup_lower(ccol_memmgmt_procs *mp, const char *s) {
  if (!s) return NULL;
  size_t len = strlen(s) + 1;
  char *d = (char *)ccol__mem_alloc(mp, len);
  if (!d) return NULL;
  for (size_t i = 0; i < len; ++i)
    d[i] = (char)ccol_ascii_tolower((unsigned char)s[i]);
  return d;
}

// The largest file ctls_read_file() reads. PEM files are a few KB (a full CA
// bundle a few hundred), so 16 MB refuses a path that names a log or a disk
// image fast instead of allocating gigabytes. It's an off_t like st_size.
#define CTLS_MAX_PEM_FILE_SIZE ((off_t)16 * 1024 * 1024)

// Reads the whole file at path into a new buffer from mp, for certificates,
// keys and CA bundles. It gives ccol_success with *out_data and *out_len set,
// ccol_not_enough_memory when the buffer can't be allocated, or
// ccol_http_tls_cert_load_failed for a missing or unreadable file, one that
// isn't a regular file, an empty or oversized one, or a short read; *why (when
// not NULL) gets the reason. The open uses O_NONBLOCK, so a FIFO or a device
// never blocks (the caller may hold a lock), and the type check runs on the
// descriptor, so a swapped path can't pass it. O_NOCTTY and O_CLOEXEC keep a
// terminal and a spawned child out of it.
static ccol_retval ctls_read_file(ccol_memmgmt_procs *mp, const char *path,
                                  char **out_data, size_t *out_len,
                                  char **why) {
  *out_data = NULL;
  *out_len = 0;
  if (why) *why = "file is missing or unreadable";
  if (!path) return ccol_http_tls_cert_load_failed;
  int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
  if (fd < 0) return ccol_http_tls_cert_load_failed;
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    if (why) *why = "path does not name a regular file";
    close(fd);
    return ccol_http_tls_cert_load_failed;
  }
  if (st.st_size <= 0) {
    close(fd);
    return ccol_http_tls_cert_load_failed;
  }
  if (st.st_size > CTLS_MAX_PEM_FILE_SIZE) {
    if (why) *why = "file is larger than the 16 MB limit for a PEM artefact";
    close(fd);
    return ccol_http_tls_cert_load_failed;
  }
  size_t sz = (size_t)st.st_size;
  char *buf = (char *)ccol__mem_alloc(mp, sz);
  if (!buf) {
    if (why) *why = "out of memory while reading the file";
    close(fd);
    return ccol_not_enough_memory;
  }
  size_t got = 0;
  while (got < sz) {
    ssize_t n = read(fd, buf + got, sz - got);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    got += (size_t)n;
  }
  close(fd);
  if (got != sz) {
    ccol__mem_free(mp, buf);
    return ccol_http_tls_cert_load_failed;
  }
  *out_data = buf;
  *out_len = sz;
  return ccol_success;
}

// Default CA file of the platform
//
// Gives getenv(name), or NULL in a setuid or setgid process, so a privileged
// program's caller can't pick its trust anchors (as OpenSSL does for
// SSL_CERT_FILE and SSL_CERT_DIR).
static const char *ctls_trusted_getenv(const char *name) {
#if defined(__linux__)
  if (getauxval(AT_SECURE) != 0) return NULL;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
  if (issetugid()) return NULL;
#else
  if (getuid() != geteuid() || getgid() != getegid()) return NULL;
#endif
  return getenv(name);
}

#ifdef RUNNING_UNIT_TESTS
// Counts the parses of the default CA file, for a white-box test.
static atomic_long g_ctls_system_ca_parses_for_tests;

// Gives the parse count, so a test can check the file is parsed once.
long ctls__system_ca_parses_for_tests(void) {
  return atomic_load(&g_ctls_system_ca_parses_for_tests);
}
#endif

// Parses the PEM buffer (pem, len) into its certificates and CRLs, for trust
// bundles and the default CA file. OpenSSL 3 can lose an allocation without
// ERR_R_MALLOC_FAILURE: It stops part way with an error queued, keeps a
// certificate whose key failed to decode, or gives NULL. A block it can't read
// looks the same, so in any of those cases the buffer is parsed again; the
// same result twice is kept, a different one counts as out of memory. It sets
// *oom and gives NULL when an allocation is known to have failed. *complete
// (may be NULL) tells whether the kept result had no error and every key
// decoded, so a caller knows not to cache a short one. The thread's error
// queue is empty on return.
static STACK_OF(X509_INFO) *
    ctls_parse_infos(const char *pem, size_t len, bool *oom, bool *complete) {
  STACK_OF(X509_INFO) *infos = NULL;
  bool incomplete = false;
  int first_count = 0, first_undecoded = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    sk_X509_INFO_pop_free(infos, X509_INFO_free);
    infos = NULL;
    BIO *bio = BIO_new_mem_buf(pem, (int)len);
    if (!bio) {
      *oom = true;
      break;
    }
    infos = PEM_X509_INFO_read_bio(bio, NULL, NULL, NULL);
    BIO_free(bio);
    incomplete = false;
    bool malloc_failure = false;
    unsigned long e;
    while ((e = ERR_get_error()) != 0) {
      incomplete = true;
      if (ERR_GET_REASON(e) == ERR_R_MALLOC_FAILURE) malloc_failure = true;
    }
    int count = infos ? sk_X509_INFO_num(infos) : -1;
    int undecoded = 0;
    for (int i = 0; i < count; ++i) {
      X509 *x = sk_X509_INFO_value(infos, i)->x509;
      if (x && !X509_get0_pubkey(x)) undecoded++;
    }
    ERR_clear_error();
    if (!infos || undecoded > 0) incomplete = true;
    if (!incomplete) break;
    if (attempt == 0) {
      first_count = count;
      first_undecoded = undecoded;
    } else if (malloc_failure || count != first_count ||
               undecoded != first_undecoded) {
      *oom = true;
    }
  }
  ERR_clear_error();
  if (*oom) {
    sk_X509_INFO_pop_free(infos, X509_INFO_free);
    return NULL;
  }
  if (complete) *complete = !incomplete;
  return infos;
}

// Reads the default CA file (SSL_CERT_FILE when the environment may be
// trusted, else the built-in path) through ctls_read_file(), so the
// descriptor is close-on-exec, and parses it for ctls_store_add_system_file().
// It gives NULL when the file is missing, unreadable, not regular or holds no
// certificate, and also when an allocation failed, which it marks in *oom so
// the caller doesn't fall back to the directory. *cacheable tells whether the
// result may be kept for the process. It counts the parse for tests and
// clears the error queue.
static STACK_OF(X509_INFO) * ctls_system_ca_load(bool *oom, bool *cacheable) {
  const char *path = ctls_trusted_getenv(X509_get_default_cert_file_env());
  if (!path || !*path) path = X509_get_default_cert_file();
  char *pem = NULL;
  size_t len = 0;
  ccol_retval read_rv = ctls_read_file(NULL, path, &pem, &len, NULL);
  if (read_rv != ccol_success) {
    if (read_rv == ccol_not_enough_memory) *oom = true;
    return NULL;
  }
  STACK_OF(X509_INFO) *infos = ctls_parse_infos(pem, len, oom, cacheable);
  ccol__default_mem_free(pem);
  if (*oom) {
    sk_X509_INFO_pop_free(infos, X509_INFO_free);
    return NULL;
  }
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_ctls_system_ca_parses_for_tests, 1);
#endif
  bool any_cert = false;
  for (int i = 0; infos && i < sk_X509_INFO_num(infos); ++i)
    if (sk_X509_INFO_value(infos, i)->x509) any_cert = true;
  if (!any_cert) {
    sk_X509_INFO_pop_free(infos, X509_INFO_free);
    infos = NULL;
  }
  ERR_clear_error();
  return infos;
}

// Adds the certificate and the CRL of info to store, each with its own
// reference, while a store is built. An object the store already holds counts
// as added (X509_R_CERT_ALREADY_IN_HASH_TABLE is popped off the queue), so a
// CA in two bundles never fails a rebuild. Any other failure is an allocation
// failure: It gives false and leaves the reason queued.
static bool ctls_store_add_info(X509_STORE *store, X509_INFO *info) {
  for (int pass = 0; pass < 2; ++pass) {
    if (pass == 0 ? !info->x509 : !info->crl) continue;
    ERR_set_mark();
    int added = pass == 0 ? X509_STORE_add_cert(store, info->x509)
                          : X509_STORE_add_crl(store, info->crl);
    if (added == 1) {
      ERR_pop_to_mark();
      continue;
    }
    unsigned long e = ERR_peek_last_error();
    if (ERR_GET_LIB(e) != ERR_LIB_X509 ||
        ERR_GET_REASON(e) != X509_R_CERT_ALREADY_IN_HASH_TABLE) {
      return false;
    }
    ERR_pop_to_mark();
  }
  return true;
}

// Adds every entry of infos to store with ctls_store_add_info(); false when an
// add fails.
static bool ctls_store_add_infos(X509_STORE *store,
                                 STACK_OF(X509_INFO) * infos) {
  for (int i = 0; i < sk_X509_INFO_num(infos); ++i)
    if (!ctls_store_add_info(store, sk_X509_INFO_value(infos, i))) return false;
  return true;
}

// Adds the default CA file to store, for a context that trusts the system
// store. The file is parsed once per process into ctls_system_ca_bundle
// (under its mutex); a failed or possibly short load isn't kept, so the next
// call reads the file again, and after the exit handler nothing is kept. It
// gives 1 when the certificates went in, 0 when there's no usable file, and
// -1 when an add failed or the load ran out of memory (then *oom is set).
static int ctls_store_add_system_file(X509_STORE *store, bool *oom) {
  ccol__call_once(ctls_system_ca_bundle.once, ctls_system_ca_globals_init);
  ccol__mutex_lock(ctls_system_ca_bundle.mutex);
  STACK_OF(X509_INFO) *infos = ctls_system_ca_bundle.infos;
  bool owned = false;
  if (!infos) {
    bool cacheable = false;
    infos = ctls_system_ca_load(oom, &cacheable);
    if (!infos && *oom) {
      ccol__mutex_unlock(ctls_system_ca_bundle.mutex);
      return -1;
    }
    if (infos && cacheable && !ctls_system_ca_bundle.finalized) {
      ctls_system_ca_bundle.infos = infos;
      ctls_process_state_exit_arm();
    } else {
      owned = true;
    }
  }
  int rv = 0;
  if (infos) rv = ctls_store_add_infos(store, infos) ? 1 : -1;
  if (owned) sk_X509_INFO_pop_free(infos, X509_INFO_free);
  ccol__mutex_unlock(ctls_system_ca_bundle.mutex);
  return rv;
}

#ifdef RUNNING_UNIT_TESTS
// Drops the cached default CA file, so the next system-trust context reads it
// again (after a test changed SSL_CERT_FILE, for example).
void ctls__system_ca_forget_for_tests(void) {
  ccol__call_once(ctls_system_ca_bundle.once, ctls_system_ca_globals_init);
  ccol__mutex_lock(ctls_system_ca_bundle.mutex);
  sk_X509_INFO_pop_free(ctls_system_ca_bundle.infos, X509_INFO_free);
  ctls_system_ca_bundle.infos = NULL;
  ccol__mutex_unlock(ctls_system_ca_bundle.mutex);
}

// Gives the certificates in the trust store of ctx's default SSL_CTX, or -1
// before the first build, so a test can check that a bundle went in whole.
// It reads under ctx->lock; OpenSSL 3.3 gives a copy of the store's objects,
// older versions the store's own stack.
int ctls__trusted_cert_count_for_tests(ctls_ctx *ctx) {
  if (!ctx) return -1;
  int n = -1;
  ccol__mutex_lock(ctx->lock);
  X509_STORE *store =
      ctx->ctx_default ? SSL_CTX_get_cert_store(ctx->ctx_default) : NULL;
  if (store) {
#if OPENSSL_VERSION_NUMBER >= 0x30300000L && !defined(LIBRESSL_VERSION_NUMBER)
    STACK_OF(X509_OBJECT) *objs = X509_STORE_get1_objects(store);
#else
    STACK_OF(X509_OBJECT) *objs = X509_STORE_get0_objects(store);
#endif
    if (objs) {
      n = 0;
      for (int i = 0; i < sk_X509_OBJECT_num(objs); ++i)
        if (X509_OBJECT_get_type(sk_X509_OBJECT_value(objs, i)) == X509_LU_X509)
          n++;
    }
#if OPENSSL_VERSION_NUMBER >= 0x30300000L && !defined(LIBRESSL_VERSION_NUMBER)
    sk_X509_OBJECT_pop_free(objs, X509_OBJECT_free);
#endif
  }
  ccol__mutex_unlock(ctx->lock);
  return n;
}
#endif

// Installs the platform trust anchors on store for ctls_apply_trust(): The
// default CA file when it exists and holds a certificate, otherwise OpenSSL's
// hashed-directory lookup (SSL_CERT_DIR when the environment may be trusted,
// else the built-in directory), whose files OpenSSL opens during handshakes.
// We read SSL_CERT_DIR ourselves because LibreSSL's default lookup ignores the
// environment. A missing directory isn't an error, as in
// SSL_CTX_set_default_verify_paths(). It gives false and may set *oom when the
// file or the lookup can't be added.
static bool ctls_store_add_system_trust(X509_STORE *store, bool *oom) {
  int from_file = ctls_store_add_system_file(store, oom);
  if (from_file < 0) return false;
  if (from_file > 0) return true;
  X509_LOOKUP *lookup = X509_STORE_add_lookup(store, X509_LOOKUP_hash_dir());
  if (!lookup) return false;
  const char *dir = ctls_trusted_getenv(X509_get_default_cert_dir_env());
  if (!dir || !*dir) dir = X509_get_default_cert_dir();
  (void)X509_LOOKUP_add_dir(lookup, dir, X509_FILETYPE_PEM);
  return true;
}

// The password a PEM key callback hands to OpenSSL.
typedef struct ctls_pw_ctx {
  const char *password;
} ctls_pw_ctx;

// The PEM password callback: Copies the password from u (a ctls_pw_ctx) into
// buf of size bytes, truncated to fit with its NUL, so an encrypted key can be
// read. It gives the password length, or 0 when there's none.
static int ctls_pem_passwd_cb(char *buf, int size, int rwflag, void *u) {
  (void)rwflag;
  const ctls_pw_ctx *p = (const ctls_pw_ctx *)u;
  if (!p || !p->password || !size) return 0;
  size_t plen = strlen(p->password);
  int len = (size <= (int)plen) ? (size - 1) : (int)plen;
  if (len < 0) len = 0;
  memcpy(buf, p->password, (size_t)len);
  buf[len] = 0;
  return len;
}

// Self-signed certificate generation
//
// Builds a certificate whose subject and issuer are O and CN = server_name,
// valid for 180 days, with a serial from a process-wide counter, holding the
// public half of root_key and signed with it (SHA-512). It serves development
// and tests that run without certificate files. NULL on failure.
static X509 *ctls_create_self_signed_with(const char *server_name,
                                          EVP_PKEY *root_key) {
  X509 *cert = X509_new();
  if (!cert) return NULL;
  static _Atomic uint32_t counter = 0;
  uint32_t serial = ++counter;
  if (!ASN1_INTEGER_set(X509_get_serialNumber(cert), (long)serial)) {
    X509_free(cert);
    return NULL;
  }
  if (!X509_gmtime_adj(X509_get_notBefore(cert), 0) ||
      !X509_gmtime_adj(X509_get_notAfter(cert), 15552000L)) {
    X509_free(cert);
    return NULL;
  }
  if (!X509_set_pubkey(cert, root_key)) {
    X509_free(cert);
    return NULL;
  }
  X509_NAME *s = X509_get_subject_name(cert);
  size_t name_len = strlen(server_name);
  if (!X509_NAME_add_entry_by_txt(s, "O", MBSTRING_ASC,
                                  (const unsigned char *)server_name,
                                  (int)name_len, -1, 0) ||
      !X509_NAME_add_entry_by_txt(s, "CN", MBSTRING_ASC,
                                  (const unsigned char *)server_name,
                                  (int)name_len, -1, 0)) {
    X509_free(cert);
    return NULL;
  }
  if (!X509_set_issuer_name(cert, s)) {
    X509_free(cert);
    return NULL;
  }
  if (!X509_sign(cert, root_key, EVP_sha512())) {
    X509_free(cert);
    return NULL;
  }
  return cert;
}

// Builds a self-signed certificate for server_name with the process root key,
// taking and dropping a reference on the key. NULL when there's no key or the
// build fails.
static X509 *ctls_create_self_signed(const char *server_name) {
  EVP_PKEY *root_key = ctls_get_root_key();
  if (!root_key) return NULL;
  X509 *cert = ctls_create_self_signed_with(server_name, root_key);
  EVP_PKEY_free(root_key);
  return cert;
}

// SSL_CTX (re)building
//
// The session id context of every SSL_CTX of this module. One constant for all
// of them keeps resumption working across an SNI dispatch: SSL_set_SSL_CTX()
// swaps the connection's session id context only when it equals the old
// context's, and here they're always equal. Sessions never cross ctls_ctx
// objects anyway, since the cache and ticket keys belong to the default
// SSL_CTX of one build.
static const unsigned char ctls_session_id_context[] = "ccol-ctls";

// Applies the settings every SSL_CTX of this module carries, and nothing
// else: Partial writes, moving write buffers, TLS 1.2 as the minimum, no
// compression and the session id context. Moving buffers let a server retry
// a write from a copy of the unsent tail (OpenSSL otherwise fails with "bad
// write retry"). Without the session id context, a server with
// SSL_VERIFY_PEER fails every resumed handshake instead of doing a full one,
// which would break every client's second connection. Gives false on
// failure.
static bool ctls_apply_base_settings(SSL_CTX *ctx) {
  if (!ctx) return false;
  SSL_CTX_set_mode(
      ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
  return SSL_CTX_set_session_id_context(
             ctx, ctls_session_id_context,
             (unsigned int)(sizeof(ctls_session_id_context) - 1)) == 1;
}

// Installs a certificate chain and its key on ctx while it's built: A
// self-signed certificate for self_signed_name with the root key, or the PEM
// cert_pem chain (leaf first; a chain certificate that fails to attach fails
// the whole call) with the PEM key_pem, decrypted with pk_password. No
// certificate configured is fine. It finishes with SSL_CTX_check_private_key(),
// because the install calls don't reliably report a mismatched pair, and a
// context built with one would fail every handshake without a word. Gives
// false on failure.
static bool ctls_apply_cert(SSL_CTX *ctx, const char *cert_pem, size_t cert_len,
                            const char *key_pem, size_t key_len,
                            const char *pk_password, bool self_signed,
                            const char *self_signed_name) {
  if (self_signed) {
    X509 *cert = ctls_create_self_signed(self_signed_name);
    if (!cert) return false;
    EVP_PKEY *root_key = ctls_get_root_key();
    bool ok = root_key && SSL_CTX_use_certificate(ctx, cert) == 1 &&
              SSL_CTX_use_PrivateKey(ctx, root_key) == 1;
    EVP_PKEY_free(root_key);
    X509_free(cert);
    return ok && SSL_CTX_check_private_key(ctx) == 1;
  }
  if (!cert_pem || !key_pem) return true;

  ctls_pw_ctx pw = {.password = pk_password};
  bool key_ok = false;
  BIO *kbio = BIO_new_mem_buf(key_pem, (int)key_len);
  if (kbio) {
    EVP_PKEY *pkey =
        PEM_read_bio_PrivateKey(kbio, NULL, ctls_pem_passwd_cb, &pw);
    if (pkey) {
      key_ok = SSL_CTX_use_PrivateKey(ctx, pkey) == 1;
      EVP_PKEY_free(pkey);
    }
    BIO_free(kbio);
  }
  if (!key_ok) return false;

  bool cert_ok = false;
  BIO *cbio = BIO_new_mem_buf(cert_pem, (int)cert_len);
  if (cbio) {
    STACK_OF(X509_INFO) *inf = PEM_X509_INFO_read_bio(cbio, NULL, NULL, NULL);
    if (inf) {
      for (int i = 0; i < sk_X509_INFO_num(inf); ++i) {
        X509_INFO *tmp = sk_X509_INFO_value(inf, i);
        if (tmp->x509) {
          if (i == 0) {
            cert_ok = SSL_CTX_use_certificate(ctx, tmp->x509) == 1;
          } else if (SSL_CTX_add1_chain_cert(ctx, tmp->x509) != 1) {
            cert_ok = false;
          }
        }
      }
      sk_X509_INFO_pop_free(inf, X509_INFO_free);
    }
    BIO_free(cbio);
  }
  return cert_ok && SSL_CTX_check_private_key(ctx) == 1;
}

// Installs tls's trust store and verify mode on ctx, for the default SSL_CTX
// and every named one alike, so an SNI dispatch never lands on a weaker mode.
// A server requires a client certificate (SSL_VERIFY_FAIL_IF_NO_PEER_CERT)
// unless peer_cert_optional is set; clients ignore that flag. When a bundle
// holds a CRL, the store gets X509_V_FLAG_CRL_CHECK and _CRL_CHECK_ALL, as
// nginx ssl_crl does. Every bundle was accepted earlier, so a parse that
// gives nothing or a failed add is an allocation failure that fails the
// build; a store missing anchors or a CRL would quietly weaken verification.
// It reads tls's stored trust fields under the caller's tls->lock and sets
// *oom when the default CA file couldn't be loaded for lack of memory.
static bool ctls_apply_trust(SSL_CTX *ctx, ctls_ctx *tls, bool *oom) {
  if (tls->trust_count == 0 && !tls->verify_default_store) return true;
  X509_STORE *store = X509_STORE_new();
  if (!store) return false;
  SSL_CTX_set_cert_store(ctx, store);
  SSL_CTX_set_verify(ctx,
                     tls->peer_cert_optional
                         ? SSL_VERIFY_PEER
                         : SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                     NULL);
  if (tls->verify_default_store && !ctls_store_add_system_trust(store, oom))
    return false;
  bool any_crl = false;
  for (size_t i = 0; i < tls->trust_count; ++i) {
    STACK_OF(X509_INFO) *inf =
        ctls_parse_infos(tls->trust_pems[i], tls->trust_lens[i], oom, NULL);
    if (!inf) return false;
    for (int j = 0; j < sk_X509_INFO_num(inf); ++j)
      if (sk_X509_INFO_value(inf, j)->crl) any_crl = true;
    bool added = ctls_store_add_infos(store, inf);
    sk_X509_INFO_pop_free(inf, X509_INFO_free);
    if (!added) return false;
  }
  if (any_crl &&
      X509_STORE_set_flags(
          store, X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL) != 1)
    return false;
  return true;
}

// The server ALPN selection callback, run inside SSL_accept(). It picks the
// first protocol of the client's offer (in, inlen) that tls registered and
// points *out at a copy in conn's own buffer, because OpenSSL reads *out after
// the callback and tls->alpn may be freed or moved by a concurrent
// ctls_ctx_alpn_add(). It reads tls->alpn under tls->lock, after resolving
// conn through the ex_data index. With no match it gives NOACK and records
// nothing, so a caller never believes the peer agreed to a protocol it never
// acknowledged. on_selected fires later from ctls_record_alpn(), after
// SSL_accept() returns, so a callback can destroy its own connection.
static int ctls_alpn_select_cb(SSL *ssl, const unsigned char **out,
                               unsigned char *outlen, const unsigned char *in,
                               unsigned int inlen, void *arg) {
  ctls_ctx *tls = (ctls_ctx *)arg;
  ctls_conn *conn = (ctls_conn *)SSL_get_ex_data(ssl, ctls_conn_ex_idx());
  if (!conn) return SSL_TLSEXT_ERR_NOACK;

  ccol__mutex_lock(tls->lock);
  if (tls->alpn_count == 0) {
    ccol__mutex_unlock(tls->lock);
    return SSL_TLSEXT_ERR_NOACK;
  }
  ctls_alpn_entry *matched = NULL;
  const unsigned char *p = in;
  const unsigned char *end = in + inlen;
  while (p < end && !matched) {
    uint8_t l = p[0];
    const unsigned char *name = p + 1;
    p += (size_t)l + 1;
    for (size_t i = 0; i < tls->alpn_count; ++i) {
      ctls_alpn_entry *e = &tls->alpn[i];
      if (e->name_len == l && memcmp(e->name, name, l) == 0) {
        matched = e;
        break;
      }
    }
  }
  if (!matched) {
    ccol__mutex_unlock(tls->lock);
    return SSL_TLSEXT_ERR_NOACK;
  }

  memcpy(conn->alpn_selected_buf, matched->name, matched->name_len);
  conn->alpn_selected_buf[matched->name_len] = '\0';
  conn->alpn_selected_name = conn->alpn_selected_buf;
  conn->alpn_selected_len = matched->name_len;
  ccol__mutex_unlock(tls->lock);

  *out = (const unsigned char *)conn->alpn_selected_buf;
  *outlen = (unsigned char)conn->alpn_selected_len;
  return SSL_TLSEXT_ERR_OK;
}

// Installs tls's ALPN protocols on ctx: The server selection callback, and the
// client offer list in wire format. It sets *oom when its own allocation
// fails (that leaves nothing on the error queue); the only other failure,
// inside SSL_CTX_set_alpn_protos(), is an allocation too. Gives false on
// failure.
static bool ctls_apply_alpn(SSL_CTX *ctx, ctls_ctx *tls, bool *oom) {
  if (tls->alpn_count == 0) return true;
  size_t wire_len = 0;
  for (size_t i = 0; i < tls->alpn_count; ++i)
    wire_len += tls->alpn[i].name_len + 1;
  unsigned char *wire =
      (unsigned char *)ccol__mem_alloc(tls->m_procs, wire_len);
  if (!wire) {
    *oom = true;
    return false;
  }
  size_t pos = 0;
  for (size_t i = 0; i < tls->alpn_count; ++i) {
    wire[pos++] = (unsigned char)tls->alpn[i].name_len;
    memcpy(wire + pos, tls->alpn[i].name, tls->alpn[i].name_len);
    pos += tls->alpn[i].name_len;
  }
  int failed = SSL_CTX_set_alpn_protos(ctx, wire, (unsigned int)wire_len);
  ccol__mem_free(tls->m_procs, wire);
  if (failed != 0) {
    *oom = true;
    return false;
  }
  SSL_CTX_set_alpn_select_cb(ctx, ctls_alpn_select_cb, tls);
  return true;
}

// Looks up one registered pattern (lower case) in tls->named_certs and gives
// its built SSL_CTX, or NULL when it isn't registered. The caller holds
// tls->lock.
static SSL_CTX *ctls_named_ctx_for_pattern(ctls_ctx *tls, char *pattern) {
  cmap_pair kp = {.ptr = pattern, .size = strlen(pattern) + 1};
  cmap_pair vp;
  if (chashmap_get_elem_ref(tls->named_certs, &kp, &vp) != ccol_success)
    return NULL;
  ctls_named_cert *nc;
  memcpy(&nc, vp.ptr, sizeof(nc));
  return nc->built_ctx;
}

// Resolves a ClientHello's SNI name to the SSL_CTX of a named certificate, or
// NULL. An exact match always wins over the one-label "*." wildcard, so a
// host with its own certificate never gets the wildcard's whatever the order
// of registration. Each half is one probe: The only wildcard candidate is
// "*." plus everything after the first dot, written in place over the copy.
// The caller holds tls->lock.
static SSL_CTX *ctls_find_named_ctx(ctls_ctx *tls, const char *sni_name) {
  if (!tls->named_certs || !sni_name || !*sni_name) return NULL;
  char *lower = ctls_strdup_lower(tls->m_procs, sni_name);
  if (!lower) return NULL;
  SSL_CTX *found = ctls_named_ctx_for_pattern(tls, lower);
  if (!found) {
    char *dot = strchr(lower, '.');
    if (dot && dot > lower && dot[1] != '\0') {
      char *pattern = dot - 1;
      pattern[0] = '*';
      pattern[1] = '.';
      found = ctls_named_ctx_for_pattern(tls, pattern);
    }
  }
  ccol__mem_free(tls->m_procs, lower);
  return found;
}

// Records our reference on installed, the SSL_CTX an SNI dispatch put on
// conn, dropping the reference an earlier dispatch left (a TLS 1.3
// HelloRetryRequest runs the callback twice). The caller holds ctx->lock in
// the same section as the lookup and SSL_set_SSL_CTX(), so no rebuild can
// free the object in between. The pin makes "every context a live connection
// can reach is one we hold" a property of ours, not of OpenSSL's internals.
static void ctls_conn_pin_sni_ctx(ctls_conn *conn, SSL_CTX *installed) {
  SSL_CTX_up_ref(installed);
  if (conn->pinned_sni_ctx) {
    SSL_CTX_free(conn->pinned_sni_ctx);
    ctls_account_ctx_pins(-1);
  }
  conn->pinned_sni_ctx = installed;
  ctls_account_ctx_pins(1);
}

// The SNI callback, run during SSL_accept(): Moves the connection onto the
// SSL_CTX of the named certificate that matches the ClientHello's name, so
// each host gets its own certificate. conn is resolved before taking
// tls->lock, which keeps the ex_data mutex and that lock independent. The
// lookup, SSL_set_SSL_CTX() and the pin happen in one tls->lock section,
// because a concurrent rebuild frees replaced contexts under that lock. A
// failed install leaves the connection on its old context and pins nothing.
// An unknown name keeps the default certificate.
static int ctls_servername_cb(SSL *ssl, int *ad, void *arg) {
  (void)ad;
  ctls_ctx *tls = (ctls_ctx *)arg;
  ctls_conn *conn = (ctls_conn *)SSL_get_ex_data(ssl, ctls_conn_ex_idx());
  const char *name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
  if (!name) return SSL_TLSEXT_ERR_OK;
  ccol__mutex_lock(tls->lock);
  SSL_CTX *matched = ctls_find_named_ctx(tls, name);
  if (matched && SSL_set_SSL_CTX(ssl, matched) && conn)
    ctls_conn_pin_sni_ctx(conn, matched);
  ccol__mutex_unlock(tls->lock);
  return SSL_TLSEXT_ERR_OK;
}

// Builds one fully configured SSL_CTX from scratch (base settings, an optional
// certificate, tls's trust store and ALPN protocols), for the default context
// and for each named certificate, so SSL_set_SSL_CTX() can switch to any of
// them as a whole. It gives NULL on failure, and the caller keeps the old
// object. A failed SSL_CTX_new() or trust store counts as out of memory
// (*oom), since nothing from the caller can make them fail and OpenSSL 3
// often reports no ERR_R_MALLOC_FAILURE.
static SSL_CTX *ctls_build_one_ctx(ctls_ctx *tls, const char *cert_pem,
                                   size_t cert_len, const char *key_pem,
                                   size_t key_len, const char *pk_password,
                                   bool self_signed,
                                   const char *self_signed_name, bool *oom) {
  SSL_CTX *ctx = SSL_CTX_new(TLS_method());
  if (!ctx) {
    *oom = true;
    return NULL;
  }
  if (!ctls_apply_base_settings(ctx) ||
      !ctls_apply_cert(ctx, cert_pem, cert_len, key_pem, key_len, pk_password,
                       self_signed, self_signed_name)) {
    SSL_CTX_free(ctx);
    return NULL;
  }
  if (!ctls_apply_trust(ctx, tls, oom)) {
    *oom = true;
    SSL_CTX_free(ctx);
    return NULL;
  }
  if (!ctls_apply_alpn(ctx, tls, oom)) {
    SSL_CTX_free(ctx);
    return NULL;
  }
  return ctx;
}

// Gives the result of a failed rebuild and empties the thread's error queue:
// ccol_not_enough_memory when oom is set or the queue holds
// ERR_R_MALLOC_FAILURE, ccol_http_tls_cert_load_failed otherwise.
static ccol_retval ctls_rebuild_failure(bool oom) {
  unsigned long e;
  while ((e = ERR_get_error()) != 0) {
    if (ERR_GET_REASON(e) == ERR_R_MALLOC_FAILURE) oom = true;
  }
  return oom ? ccol_not_enough_memory : ccol_http_tls_cert_load_failed;
}

// Rebuilds the default SSL_CTX and every named certificate's SSL_CTX from the
// configuration tls stores; every call that changes a context runs it under
// tls->lock. It never patches an SSL_CTX in place: Everything is built first
// and committed together (old contexts freed, new ones installed), so a
// failure leaves every old object serving as before. A caller that changed
// the stored configuration first must restore it on failure, or the next
// successful rebuild would commit what this one refused. It gives
// ccol_success, ccol_not_enough_memory or ccol_http_tls_cert_load_failed, and
// the error queue is empty on return.
static ccol_retval ctls_ctx_rebuild_locked(ctls_ctx *tls) {
  ERR_clear_error();
  bool oom = false;
  SSL_CTX *new_default = ctls_build_one_ctx(
      tls, tls->has_default_cert ? tls->default_cert_pem : NULL,
      tls->default_cert_len,
      tls->has_default_cert ? tls->default_key_pem : NULL, tls->default_key_len,
      tls->default_pk_password, tls->default_self_signed,
      tls->default_self_signed_name, &oom);
  if (!new_default) return ctls_rebuild_failure(oom);

  if (tls->named_certs && chashmap_elem_count(tls->named_certs) > 0) {
    SSL_CTX_set_tlsext_servername_callback(new_default, ctls_servername_cb);
    SSL_CTX_set_tlsext_servername_arg(new_default, tls);
  }

  size_t named_count =
      tls->named_certs ? chashmap_elem_count(tls->named_certs) : 0;
  SSL_CTX **new_named_ctxs = NULL;
  ctls_named_cert **new_named_entries = NULL;
  if (named_count > 0) {
    new_named_ctxs = (SSL_CTX **)ccol__mem_alloc(
        tls->m_procs, named_count * sizeof(SSL_CTX *));
    new_named_entries = (ctls_named_cert **)ccol__mem_alloc(
        tls->m_procs, named_count * sizeof(ctls_named_cert *));
    if (!new_named_ctxs || !new_named_entries) {
      ccol__mem_free(tls->m_procs, new_named_ctxs);
      ccol__mem_free(tls->m_procs, new_named_entries);
      SSL_CTX_free(new_default);
      return ctls_rebuild_failure(true);
    }
    ccol_chmap_walk walk = {0};
    const void *key_bytes;
    const void *val_bytes;
    size_t i = 0;
    bool ok = true;
    while (i < named_count && ccol_chmap_walk_next(tls->named_certs, &walk,
                                                   &key_bytes, &val_bytes)) {
      ctls_named_cert *nc;
      memcpy(&nc, val_bytes, sizeof(nc));
      SSL_CTX *built = ctls_build_one_ctx(
          tls, nc->cert_pem, nc->cert_len, nc->key_pem, nc->key_len,
          nc->pk_password, nc->self_signed, nc->self_signed_name, &oom);
      if (!built) {
        ok = false;
        break;
      }
      new_named_ctxs[i] = built;
      new_named_entries[i] = nc;
      ++i;
    }
    if (i != named_count) ok = false;
    if (!ok) {
      for (size_t j = 0; j < i; ++j) SSL_CTX_free(new_named_ctxs[j]);
      ccol__mem_free(tls->m_procs, new_named_ctxs);
      ccol__mem_free(tls->m_procs, new_named_entries);
      SSL_CTX_free(new_default);
      return ctls_rebuild_failure(oom);
    }
  }

  if (tls->ctx_default) SSL_CTX_free(tls->ctx_default);
  tls->ctx_default = new_default;
  for (size_t i = 0; i < named_count; ++i) {
    if (new_named_entries[i]->built_ctx)
      SSL_CTX_free(new_named_entries[i]->built_ctx);
    new_named_entries[i]->built_ctx = new_named_ctxs[i];
  }
  ccol__mem_free(tls->m_procs, new_named_ctxs);
  ccol__mem_free(tls->m_procs, new_named_entries);
  ERR_clear_error();
  return ccol_success;
}

// ctls_ctx construction and mutation
//
// Frees a named certificate entry: Its PEM data, password, self-signed name
// and built SSL_CTX, through tls's allocator.
static void ctls_named_cert_destroy(ctls_ctx *tls, ctls_named_cert *nc) {
  if (!nc) return;
  ccol__mem_free(tls->m_procs, nc->cert_pem);
  ccol__mem_free(tls->m_procs, nc->key_pem);
  ccol__mem_free(tls->m_procs, nc->pk_password);
  ccol__mem_free(tls->m_procs, nc->self_signed_name);
  if (nc->built_ctx) SSL_CTX_free(nc->built_ctx);
  ccol__mem_free(tls->m_procs, nc);
}

// Tells whether the PEM key (decrypted with pk_password) matches the leaf
// certificate at the start of the PEM chain, when ctls_ctx_cert_add() accepts
// the input. The check happens before anything is stored, because a bad pair
// that got stored would fail every later rebuild of the context. A key it
// can't read leaves its reason on the error queue, so the queue is cleared,
// or a later SSL_get_error() on an unrelated connection would read it.
static bool ctls_cert_key_pair_matches(const char *cert_pem, size_t cert_len,
                                       const char *key_pem, size_t key_len,
                                       const char *pk_password) {
  ctls_pw_ctx pw = {.password = pk_password};
  BIO *kbio = BIO_new_mem_buf(key_pem, (int)key_len);
  if (!kbio) return false;
  EVP_PKEY *pkey = PEM_read_bio_PrivateKey(kbio, NULL, ctls_pem_passwd_cb, &pw);
  BIO_free(kbio);
  if (!pkey) {
    ERR_clear_error();
    return false;
  }
  bool matches = false;
  BIO *cbio = BIO_new_mem_buf(cert_pem, (int)cert_len);
  if (cbio) {
    STACK_OF(X509_INFO) *inf = PEM_X509_INFO_read_bio(cbio, NULL, NULL, NULL);
    if (inf) {
      X509_INFO *leaf =
          sk_X509_INFO_num(inf) > 0 ? sk_X509_INFO_value(inf, 0) : NULL;
      if (leaf && leaf->x509)
        matches = X509_check_private_key(leaf->x509, pkey) == 1;
      sk_X509_INFO_pop_free(inf, X509_INFO_free);
    }
    BIO_free(cbio);
  }
  EVP_PKEY_free(pkey);
  if (!matches) ERR_clear_error();
  return matches;
}

// Creates a context with a refcount of 1, a copy of mp (NULL for the default
// allocator), its mutex, an empty named-certificate map and a first default
// SSL_CTX, so it can serve clients at once. It gives NULL with *err_str set
// when any step fails, freeing what was made.
ctls_ctx *ctls_ctx_new_mp(ccol_memmgmt_procs *mp, char **err_str) {
  ctls_ctx *tls = (ctls_ctx *)ccol__mem_calloc(mp, 1, sizeof(*tls));
  if (!tls) {
    if (err_str) *err_str = "ctls_ctx_new: allocation failure";
    return NULL;
  }
  if (mp) {
    tls->m_procs = (ccol_memmgmt_procs *)ccol__mem_alloc(mp, sizeof(*mp));
    if (!tls->m_procs) {
      ccol__mem_free(mp, tls);
      if (err_str) *err_str = "ctls_ctx_new: allocation failure";
      return NULL;
    }
    *tls->m_procs = *mp;
  }
  tls->ref = 1;
  if (ccol__mutex_init(tls->lock) != 0) {
    ccol__mem_free(mp, tls->m_procs);
    ccol__mem_free(mp, tls);
    if (err_str) *err_str = "ctls_ctx_new: failed to initialize mutex";
    return NULL;
  }
  char *err = NULL;
  tls->named_certs =
      chashmap_create_mp(16, ccol_string, ccol_pointer, tls->m_procs, &err);
  if (!tls->named_certs) {
    ccol__mutex_destroy(tls->lock);
    ccol__mem_free(mp, tls->m_procs);
    ccol__mem_free(mp, tls);
    if (err_str) *err_str = "ctls_ctx_new: chmap allocation failure";
    return NULL;
  }
  if (ctls_ctx_rebuild_locked(tls) != ccol_success) {
    chashmap__destroy(tls->named_certs);
    ccol__mutex_destroy(tls->lock);
    ccol__mem_free(mp, tls->m_procs);
    ccol__mem_free(mp, tls);
    if (err_str) *err_str = "ctls_ctx_new: SSL_CTX_new failure";
    return NULL;
  }
  return tls;
}

// Adds or replaces the default or a named certificate of ctx. It reads and
// checks the files first (readable, matching pair, a pair OpenSSL accepts in
// a throwaway SSL_CTX), so nothing is stored for a refused input; NULL paths
// make a self-signed certificate. Under ctx->lock it then stores the new
// fields and rebuilds. Only a successful rebuild frees the replaced fields or
// entry; a failed one puts them back (or deletes a name this call added),
// which can't fail, so every name keeps the certificate it served. A replaced
// entry's built context lives until the rebuild succeeded, and a connection
// still using it holds its own reference. Gives the codes of ctls.h.
ccol_retval ctls_ctx_cert_add(ctls_ctx *ctx, const char *server_name,
                              const char *cert_path, const char *key_path,
                              const char *pk_password, char **err_str) {
  if (!ctx) return ccol_invalid_args;
  bool have_name = server_name && *server_name;
  bool have_pair = cert_path && key_path;
  bool self_signed = !cert_path && !key_path;
  if (!have_pair && !self_signed) {
    if (err_str)
      *err_str =
          "ctls_ctx_cert_add: need both cert_path+key_path, or neither (for "
          "a self-signed certificate)";
    return ccol_invalid_args;
  }

  char *cert_pem = NULL, *key_pem = NULL, *pw_copy = NULL;
  size_t cert_len = 0, key_len = 0;
  if (have_pair) {
    char *why = NULL;
    ccol_retval read_rv =
        ctls_read_file(ctx->m_procs, cert_path, &cert_pem, &cert_len, &why);
    if (read_rv == ccol_success)
      read_rv =
          ctls_read_file(ctx->m_procs, key_path, &key_pem, &key_len, &why);
    if (read_rv != ccol_success) {
      ccol__mem_free(ctx->m_procs, cert_pem);
      ccol__mem_free(ctx->m_procs, key_pem);
      if (err_str) {
        *err_str = why ? why : "ctls_ctx_cert_add: cert/key file unreadable";
      }
      return read_rv;
    }
    if (!ctls_cert_key_pair_matches(cert_pem, cert_len, key_pem, key_len,
                                    pk_password)) {
      ccol__mem_free(ctx->m_procs, cert_pem);
      ccol__mem_free(ctx->m_procs, key_pem);
      if (err_str) {
        *err_str =
            "ctls_ctx_cert_add: certificate and private key do not match, or "
            "either one is unreadable";
      }
      return ccol_http_tls_cert_load_failed;
    }
    SSL_CTX *probe = SSL_CTX_new(TLS_method());
    bool usable = probe && ctls_apply_base_settings(probe) &&
                  ctls_apply_cert(probe, cert_pem, cert_len, key_pem, key_len,
                                  pk_password, false, NULL);
    if (probe) SSL_CTX_free(probe);
    ERR_clear_error();
    if (!usable) {
      ccol__mem_free(ctx->m_procs, cert_pem);
      ccol__mem_free(ctx->m_procs, key_pem);
      if (err_str) {
        *err_str =
            "ctls_ctx_cert_add: the TLS library refuses this certificate "
            "(for example a key below its security level, or a weak "
            "signature digest)";
      }
      return ccol_http_tls_cert_load_failed;
    }
    if (pk_password) {
      pw_copy = ctls_strdup(ctx->m_procs, pk_password);
      if (!pw_copy) {
        ccol__mem_free(ctx->m_procs, cert_pem);
        ccol__mem_free(ctx->m_procs, key_pem);
        return ccol_not_enough_memory;
      }
    }
  }

  ccol__mutex_lock(ctx->lock);
  ccol_retval rv = ccol_success;
  if (!have_name) {
    char *self_signed_name = NULL;
    if (self_signed) {
      self_signed_name = ctls_strdup(ctx->m_procs, "ctls-default");
      if (!self_signed_name) {
        ccol__mem_free(ctx->m_procs, cert_pem);
        ccol__mem_free(ctx->m_procs, key_pem);
        ccol__mem_free(ctx->m_procs, pw_copy);
        ccol__mutex_unlock(ctx->lock);
        return ccol_not_enough_memory;
      }
    }
    char *prev_cert_pem = ctx->default_cert_pem;
    size_t prev_cert_len = ctx->default_cert_len;
    char *prev_key_pem = ctx->default_key_pem;
    size_t prev_key_len = ctx->default_key_len;
    char *prev_pw = ctx->default_pk_password;
    bool prev_self_signed = ctx->default_self_signed;
    char *prev_self_signed_name = ctx->default_self_signed_name;
    bool prev_has_default = ctx->has_default_cert;
    ctx->default_cert_pem = cert_pem;
    ctx->default_cert_len = cert_len;
    ctx->default_key_pem = key_pem;
    ctx->default_key_len = key_len;
    ctx->default_pk_password = pw_copy;
    ctx->default_self_signed = self_signed;
    ctx->default_self_signed_name = self_signed_name;
    ctx->has_default_cert = true;
    rv = ctls_ctx_rebuild_locked(ctx);
    if (rv != ccol_success) {
      ctx->default_cert_pem = prev_cert_pem;
      ctx->default_cert_len = prev_cert_len;
      ctx->default_key_pem = prev_key_pem;
      ctx->default_key_len = prev_key_len;
      ctx->default_pk_password = prev_pw;
      ctx->default_self_signed = prev_self_signed;
      ctx->default_self_signed_name = prev_self_signed_name;
      ctx->has_default_cert = prev_has_default;
      prev_cert_pem = cert_pem;
      prev_key_pem = key_pem;
      prev_pw = pw_copy;
      prev_self_signed_name = self_signed_name;
    }
    ccol__mem_free(ctx->m_procs, prev_cert_pem);
    ccol__mem_free(ctx->m_procs, prev_key_pem);
    ccol__mem_free(ctx->m_procs, prev_pw);
    ccol__mem_free(ctx->m_procs, prev_self_signed_name);
  } else {
    char *lower_name = ctls_strdup_lower(ctx->m_procs, server_name);
    if (!lower_name) {
      ccol__mem_free(ctx->m_procs, cert_pem);
      ccol__mem_free(ctx->m_procs, key_pem);
      ccol__mem_free(ctx->m_procs, pw_copy);
      ccol__mutex_unlock(ctx->lock);
      return ccol_not_enough_memory;
    }
    ctls_named_cert *nc =
        (ctls_named_cert *)ccol__mem_calloc(ctx->m_procs, 1, sizeof(*nc));
    if (!nc) {
      ccol__mem_free(ctx->m_procs, lower_name);
      ccol__mem_free(ctx->m_procs, cert_pem);
      ccol__mem_free(ctx->m_procs, key_pem);
      ccol__mem_free(ctx->m_procs, pw_copy);
      ccol__mutex_unlock(ctx->lock);
      return ccol_not_enough_memory;
    }
    nc->cert_pem = cert_pem;
    nc->cert_len = cert_len;
    nc->key_pem = key_pem;
    nc->key_len = key_len;
    nc->pk_password = pw_copy;
    nc->self_signed = self_signed;
    if (self_signed) {
      nc->self_signed_name = ctls_strdup(ctx->m_procs, lower_name);
      if (!nc->self_signed_name) {
        ctls_named_cert_destroy(ctx, nc);
        ccol__mem_free(ctx->m_procs, lower_name);
        ccol__mutex_unlock(ctx->lock);
        return ccol_not_enough_memory;
      }
    }

    cmap_pair kp = {.ptr = lower_name, .size = strlen(lower_name) + 1};
    cmap_pair old_vp;
    ctls_named_cert *old_nc = NULL;
    if (chashmap_get_elem_ref(ctx->named_certs, &kp, &old_vp) == ccol_success)
      memcpy(&old_nc, old_vp.ptr, sizeof(old_nc));
    cmap_pair vp = {.ptr = &nc, .size = sizeof(nc)};
    rv = chashmap_insert_elem(ctx->named_certs, &kp, &vp);
    if (rv != ccol_success && rv != ccol_key_already_present) {
      ccol__mem_free(ctx->m_procs, lower_name);
      ctls_named_cert_destroy(ctx, nc);
      ccol__mutex_unlock(ctx->lock);
      return rv;
    }
    rv = ctls_ctx_rebuild_locked(ctx);
    if (rv != ccol_success) {
      if (old_nc) {
        cmap_pair old_vp_pair = {.ptr = &old_nc, .size = sizeof(old_nc)};
        ccol_retval back =
            chashmap_insert_elem(ctx->named_certs, &kp, &old_vp_pair);
        ccol__assert(back == ccol_key_already_present);
        (void)back;
      } else {
        ccol_retval gone = chashmap_delete_elem(ctx->named_certs, &kp);
        ccol__assert(gone == ccol_success);
        (void)gone;
      }
      ctls_named_cert_destroy(ctx, nc);
    } else if (old_nc) {
      ctls_named_cert_destroy(ctx, old_nc);
    }
    ccol__mem_free(ctx->m_procs, lower_name);
  }
  ccol__mutex_unlock(ctx->lock);
  if (rv != ccol_success && err_str)
    *err_str = "ctls_ctx_cert_add: SSL_CTX rebuild failure";
  return rv;
}

// Tells whether the PEM buffer (pem, len) holds at least one certificate,
// when ctls_ctx_trust() accepts a bundle. OpenSSL parses an empty file, a
// lone key or non-PEM text into an empty set with no error, and such a bundle
// would turn verification on with a store that trusts nothing (a server would
// then silently accept every anonymous client). A CRL alone names no issuer,
// so it doesn't count. It gives ccol_success, ccol_not_enough_memory when the
// parse ran out of memory, or ccol_http_tls_cert_load_failed.
static ccol_retval ctls_pem_has_certificate(const char *pem, size_t len) {
  bool oom = false;
  STACK_OF(X509_INFO) *inf = ctls_parse_infos(pem, len, &oom, NULL);
  if (oom) return ccol_not_enough_memory;
  bool found = false;
  for (int i = 0; inf && i < sk_X509_INFO_num(inf) && !found; ++i) {
    X509_INFO *entry = sk_X509_INFO_value(inf, i);
    if (entry && entry->x509) found = true;
  }
  sk_X509_INFO_pop_free(inf, X509_INFO_free);
  return found ? ccol_success : ccol_http_tls_cert_load_failed;
}

// Adds the CA bundle at ca_bundle_path to ctx's trust store and turns on peer
// verification. The file is read and checked for a certificate before
// anything is stored, since a stored bad bundle would fail every later
// rebuild. Under ctx->lock it grows trust_pems and trust_lens (each new
// pointer is stored at once, so a second failed realloc can't leave the first
// dangling), appends the bundle, sets verify_peer and rebuilds. A failed
// rebuild takes the bundle and the flag back out; the bigger arrays stay.
// Gives the codes of ctls.h, with *err_str set on failure.
ccol_retval ctls_ctx_trust(ctls_ctx *ctx, const char *ca_bundle_path,
                           char **err_str) {
  if (!ctx || !ca_bundle_path) return ccol_invalid_args;
  char *pem = NULL;
  size_t len = 0;
  char *why = NULL;
  ccol_retval read_rv =
      ctls_read_file(ctx->m_procs, ca_bundle_path, &pem, &len, &why);
  if (read_rv != ccol_success) {
    if (err_str) {
      *err_str = why ? why : "ctls_ctx_trust: CA bundle file unreadable";
    }
    return read_rv;
  }
  ccol_retval has_rv = ctls_pem_has_certificate(pem, len);
  if (has_rv != ccol_success) {
    ccol__mem_free(ctx->m_procs, pem);
    if (err_str) {
      *err_str = has_rv == ccol_not_enough_memory
                     ? "ctls_ctx_trust: out of memory while parsing the CA "
                       "bundle"
                     : "ctls_ctx_trust: CA bundle contains no certificates";
    }
    return has_rv;
  }

  ccol__mutex_lock(ctx->lock);
  if (ctx->trust_count == ctx->trust_cap) {
    size_t new_cap = ctx->trust_cap ? ctx->trust_cap * 2 : 4;
    char **new_pems = (char **)ccol__mem_realloc(ctx->m_procs, ctx->trust_pems,
                                                 new_cap * sizeof(char *));
    if (!new_pems) {
      ccol__mem_free(ctx->m_procs, pem);
      ccol__mutex_unlock(ctx->lock);
      return ccol_not_enough_memory;
    }
    ctx->trust_pems = new_pems;
    size_t *new_lens = (size_t *)ccol__mem_realloc(
        ctx->m_procs, ctx->trust_lens, new_cap * sizeof(size_t));
    if (!new_lens) {
      ccol__mem_free(ctx->m_procs, pem);
      ccol__mutex_unlock(ctx->lock);
      return ccol_not_enough_memory;
    }
    ctx->trust_lens = new_lens;
    ctx->trust_cap = new_cap;
  }
  bool prev_verify_peer = ctx->verify_peer;
  ctx->trust_pems[ctx->trust_count] = pem;
  ctx->trust_lens[ctx->trust_count] = len;
  ctx->trust_count++;
  ctx->verify_peer = true;
  ccol_retval rv = ctls_ctx_rebuild_locked(ctx);
  if (rv != ccol_success) {
    ctx->trust_count--;
    ctx->verify_peer = prev_verify_peer;
    ccol__mem_free(ctx->m_procs, pem);
  }
  ccol__mutex_unlock(ctx->lock);
  if (rv != ccol_success && err_str)
    *err_str = "ctls_ctx_trust: SSL_CTX rebuild failure";
  return rv;
}

// Makes ctx trust the system CA store and turns on peer verification: Under
// ctx->lock it sets verify_default_store and verify_peer and rebuilds,
// restoring both flags when the rebuild fails.
ccol_retval ctls_ctx_trust_system(ctls_ctx *ctx) {
  if (!ctx) return ccol_invalid_args;
  ccol__mutex_lock(ctx->lock);
  bool prev_default_store = ctx->verify_default_store;
  bool prev_verify_peer = ctx->verify_peer;
  ctx->verify_default_store = true;
  ctx->verify_peer = true;
  ccol_retval rv = ctls_ctx_rebuild_locked(ctx);
  if (rv != ccol_success) {
    ctx->verify_default_store = prev_default_store;
    ctx->verify_peer = prev_verify_peer;
  }
  ccol__mutex_unlock(ctx->lock);
  return rv;
}

// Sets whether a server only asks for a client certificate. Under ctx->lock
// it stores the flag and, when a trust store exists, rebuilds, restoring the
// old flag on failure; without a trust store the first trust call builds with
// it.
ccol_retval ctls_ctx_peer_cert_optional(ctls_ctx *ctx, bool optional) {
  if (!ctx) return ccol_invalid_args;
  ccol__mutex_lock(ctx->lock);
  if (ctx->peer_cert_optional == optional) {
    ccol__mutex_unlock(ctx->lock);
    return ccol_success;
  }
  bool previous = ctx->peer_cert_optional;
  ctx->peer_cert_optional = optional;
  ccol_retval rv = (ctx->trust_count == 0 && !ctx->verify_default_store)
                       ? ccol_success
                       : ctls_ctx_rebuild_locked(ctx);
  if (rv != ccol_success) ctx->peer_cert_optional = previous;
  ccol__mutex_unlock(ctx->lock);
  return rv;
}

// Registers or replaces an ALPN protocol on ctx. Under ctx->lock it stores the
// registration (replacing one of the same name in place, or appending) and
// rebuilds; a failed rebuild restores the old registration or removes the new
// one, so nothing of the caller's is kept and its on_cleanup never runs. The
// replaced registration's cleanup runs after the lock is released, because it
// may call back into this context. Gives the codes of ctls.h, with *err_str
// set on failure.
ccol_retval ctls_ctx_alpn_add(ctls_ctx *ctx, const char *protocol_name,
                              ctls_alpn_selected_fn on_selected,
                              void *alpn_udata, ctls_alpn_cleanup_fn on_cleanup,
                              char **err_str) {
  if (!ctx || !protocol_name) return ccol_invalid_args;
  size_t name_len = strlen(protocol_name);
  if (name_len == 0 || name_len > 255) {
    if (err_str)
      *err_str = "ctls_ctx_alpn_add: protocol name must be 1-255 bytes";
    return ccol_invalid_args;
  }
  char *name_copy = ctls_strdup(ctx->m_procs, protocol_name);
  if (!name_copy) return ccol_not_enough_memory;

  ccol__mutex_lock(ctx->lock);
  for (size_t i = 0; i < ctx->alpn_count; ++i) {
    if (ctx->alpn[i].name_len == name_len &&
        memcmp(ctx->alpn[i].name, name_copy, name_len) == 0) {
      ctls_alpn_entry old = ctx->alpn[i];
      ctx->alpn[i] = (ctls_alpn_entry){.name = name_copy,
                                       .name_len = name_len,
                                       .on_selected = on_selected,
                                       .udata = alpn_udata,
                                       .on_cleanup = on_cleanup};
      ccol_retval rv = ctls_ctx_rebuild_locked(ctx);
      if (rv != ccol_success) {
        ctx->alpn[i] = old;
        ccol__mutex_unlock(ctx->lock);
        ccol__mem_free(ctx->m_procs, name_copy);
        if (err_str) *err_str = "ctls_ctx_alpn_add: SSL_CTX rebuild failure";
        return rv;
      }
      ccol__mutex_unlock(ctx->lock);
      ccol__mem_free(ctx->m_procs, old.name);
      if (old.on_cleanup) old.on_cleanup(old.udata);
      return ccol_success;
    }
  }
  if (ctx->alpn_count == ctx->alpn_cap) {
    size_t new_cap = ctx->alpn_cap ? ctx->alpn_cap * 2 : 4;
    ctls_alpn_entry *new_alpn = (ctls_alpn_entry *)ccol__mem_realloc(
        ctx->m_procs, ctx->alpn, new_cap * sizeof(ctls_alpn_entry));
    if (!new_alpn) {
      ccol__mem_free(ctx->m_procs, name_copy);
      ccol__mutex_unlock(ctx->lock);
      return ccol_not_enough_memory;
    }
    ctx->alpn = new_alpn;
    ctx->alpn_cap = new_cap;
  }
  ctx->alpn[ctx->alpn_count++] = (ctls_alpn_entry){.name = name_copy,
                                                   .name_len = name_len,
                                                   .on_selected = on_selected,
                                                   .udata = alpn_udata,
                                                   .on_cleanup = on_cleanup};
  ccol_retval rv = ctls_ctx_rebuild_locked(ctx);
  if (rv != ccol_success) ctx->alpn_count--;
  ccol__mutex_unlock(ctx->lock);
  if (rv != ccol_success) {
    ccol__mem_free(ctx->m_procs, name_copy);
    if (err_str) *err_str = "ctls_ctx_alpn_add: SSL_CTX rebuild failure";
  }
  return rv;
}

// Gives ctx->alpn_count, read without the lock, or 0 for NULL.
size_t ctls_ctx_alpn_count(const ctls_ctx *ctx) {
  return ctx ? ctx->alpn_count : 0;
}

// Adds a reference to ctx under its lock, for a connection or a request that
// keeps using it. NULL is ignored.
void ctls_ctx_retain(ctls_ctx *ctx) {
  if (!ctx) return;
  ccol__mutex_lock(ctx->lock);
  ctx->ref++;
  ccol__mutex_unlock(ctx->lock);
}

// Drops a reference to ctx and, at zero, frees everything it owns: The built
// contexts, the stored certificates and bundles, every named certificate
// (walked without allocating, so a release under memory pressure still frees
// them all), and the ALPN registrations after calling their cleanups. ctx is
// freed before its procs copy, because that copy holds the free function the
// first free needs. NULL is fine.
void ctls_ctx_release(ctls_ctx *ctx) {
  if (!ctx) return;
  ccol__mutex_lock(ctx->lock);
  size_t remaining = --ctx->ref;
  ccol__mutex_unlock(ctx->lock);
  if (remaining > 0) return;

  if (ctx->ctx_default) SSL_CTX_free(ctx->ctx_default);
  ccol__mem_free(ctx->m_procs, ctx->default_cert_pem);
  ccol__mem_free(ctx->m_procs, ctx->default_key_pem);
  ccol__mem_free(ctx->m_procs, ctx->default_pk_password);
  ccol__mem_free(ctx->m_procs, ctx->default_self_signed_name);

  if (ctx->named_certs) {
    ccol_chmap_walk walk = {0};
    const void *key_bytes;
    const void *val_bytes;
    while (
        ccol_chmap_walk_next(ctx->named_certs, &walk, &key_bytes, &val_bytes)) {
      ctls_named_cert *nc;
      memcpy(&nc, val_bytes, sizeof(nc));
      ctls_named_cert_destroy(ctx, nc);
    }
    chashmap__destroy(ctx->named_certs);
  }

  for (size_t i = 0; i < ctx->trust_count; ++i)
    ccol__mem_free(ctx->m_procs, ctx->trust_pems[i]);
  ccol__mem_free(ctx->m_procs, ctx->trust_pems);
  ccol__mem_free(ctx->m_procs, ctx->trust_lens);

  for (size_t i = 0; i < ctx->alpn_count; ++i) {
    if (ctx->alpn[i].on_cleanup) ctx->alpn[i].on_cleanup(ctx->alpn[i].udata);
    ccol__mem_free(ctx->m_procs, ctx->alpn[i].name);
  }
  ccol__mem_free(ctx->m_procs, ctx->alpn);

  ccol__mutex_destroy(ctx->lock);
  ccol_memmgmt_procs *mp = ctx->m_procs;
  ccol__mem_free(mp, ctx);
  ccol__mem_free(mp, mp);
}

// Connections
//
// Frees conn's SSL (and with it the BIO) and drops both SSL_CTX pins in one
// ctx->lock section, then releases the BIO method count after the lock. The
// lock orders the connection's last use of a shared SSL_CTX before a rebuild,
// which frees replaced contexts under the same lock, can destroy it. It
// doesn't call SSL_shutdown(): Holding the lock across a write to one peer
// would stall every create, destroy and configuration change of the context.
// It updates the pin accounting only for pins really taken.
static void ctls_conn_release_ssl(ctls_conn *conn) {
  ctls_ctx *ctx = conn->ctx;
  ccol__mutex_lock(ctx->lock);
  if (conn->ssl) {
    SSL_free(conn->ssl);
    conn->ssl = NULL;
  }
  bool release_sock_bio = conn->holds_sock_bio;
  conn->holds_sock_bio = false;
  if (conn->pinned_sni_ctx) {
    SSL_CTX_free(conn->pinned_sni_ctx);
    conn->pinned_sni_ctx = NULL;
    ctls_account_ctx_pins(-1);
  }
  if (conn->pinned_ctx_default) {
    SSL_CTX_free(conn->pinned_ctx_default);
    conn->pinned_ctx_default = NULL;
    ctls_account_ctx_pins(-1);
  }
  ccol__mutex_unlock(ctx->lock);
  if (release_sock_bio) ctls_sock_bio_release();
}

// Undoes a connection that failed during its own construction: Releases its
// SSL state and pins, drops the context reference ctls_conn_alloc() took, and
// frees the record. The procs are copied by value first, because that release
// may free the context and the procs copy it holds.
static void ctls_conn_create_unwind(ctls_conn *conn) {
  ctls_conn_release_ssl(conn);
  ctls_ctx *ctx = conn->ctx;
  ccol_memmgmt_procs procs;
  ccol_memmgmt_procs *mp = NULL;
  if (ctx->m_procs) {
    procs = *ctx->m_procs;
    mp = &procs;
  }
  ctls_ctx_release(ctx);
  ccol__mem_free(mp, conn);
}

// Allocates a zeroed connection record from ctx's allocator for a create
// function, with its mode set and a reference on ctx. NULL when the
// allocation fails.
static ctls_conn *ctls_conn_alloc(ctls_ctx *ctx, bool is_server) {
  ctls_conn *conn =
      (ctls_conn *)ccol__mem_calloc(ctx->m_procs, 1, sizeof(*conn));
  if (!conn) return NULL;
  conn->ctx = ctx;
  conn->is_server = is_server;
  ctls_ctx_retain(ctx);
  return conn;
}

// Creates a client connection on fd for hostname. verify_host with no
// hostname is refused, so it never connects without the name check it was
// asked for. Under ctx->lock it calls SSL_new() on the default SSL_CTX and
// pins that object in the same section, and publishes conn->ssl at once so
// every later failure unwinds through ctls_conn_create_unwind(). It then sets
// the ex_data, SNI (not for an IP literal), the host or IP check, the client
// mode and one socket BIO for both directions. Gives NULL with *err_str set on
// failure.
ctls_conn *ctls_conn_create_client(ctls_ctx *ctx, int fd, const char *hostname,
                                   bool verify_host, char **err_str) {
  if (!ctx || fd < 0) {
    if (err_str) *err_str = "ctls_conn_create_client: invalid arguments";
    return NULL;
  }
  if (verify_host && (!hostname || !*hostname)) {
    if (err_str)
      *err_str =
          "ctls_conn_create_client: hostname verification requested with no "
          "hostname";
    return NULL;
  }
  ctls_conn *conn = ctls_conn_alloc(ctx, false);
  if (!conn) {
    if (err_str) *err_str = "ctls_conn_create_client: allocation failure";
    return NULL;
  }

  ccol__mutex_lock(ctx->lock);
  SSL *ssl = SSL_new(ctx->ctx_default);
  if (ssl) {
    SSL_CTX_up_ref(ctx->ctx_default);
    conn->pinned_ctx_default = ctx->ctx_default;
    ctls_account_ctx_pins(1);
    conn->ssl = ssl;
  }
  ccol__mutex_unlock(ctx->lock);
  if (!ssl) {
    ctls_conn_create_unwind(conn);
    if (err_str) *err_str = "ctls_conn_create_client: SSL_new failure";
    return NULL;
  }
  if (!SSL_set_ex_data(ssl, ctls_conn_ex_idx(), conn)) {
    ctls_conn_create_unwind(conn);
    if (err_str) *err_str = "ctls_conn_create_client: SSL_set_ex_data failure";
    return NULL;
  }

  BIO *bio = ctls_sock_bio_new(fd);
  if (!bio) {
    ctls_conn_create_unwind(conn);
    if (err_str) *err_str = "ctls_conn_create_client: socket BIO failure";
    return NULL;
  }
  SSL_set_bio(ssl, bio, bio);
  conn->holds_sock_bio = true;
  BIO_set_callback_arg(bio, (char *)&conn->rec_track);

  if (hostname && *hostname) {
    struct in_addr v4;
    struct in6_addr v6;
    bool is_ip = inet_pton(AF_INET, hostname, &v4) == 1 ||
                 inet_pton(AF_INET6, hostname, &v6) == 1;
    if (!is_ip) SSL_set_tlsext_host_name(ssl, hostname);
    if (verify_host) {
      X509_VERIFY_PARAM *param = SSL_get0_param(ssl);
      bool ok;
      if (is_ip) {
        ok = X509_VERIFY_PARAM_set1_ip_asc(param, hostname) == 1;
      } else {
        X509_VERIFY_PARAM_set_hostflags(param,
                                        X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        ok = X509_VERIFY_PARAM_set1_host(param, hostname, 0) == 1;
      }
      if (!ok) {
        ctls_conn_create_unwind(conn);
        if (err_str)
          *err_str =
              "ctls_conn_create_client: could not configure hostname "
              "verification target";
        return NULL;
      }
    }
  }
  SSL_set_connect_state(ssl);
  return conn;
}

// Creates a server connection on the accepted fd, keeping udata for
// ctls_conn_udata(). Like the client create, SSL_new() and the pin of the
// default SSL_CTX happen under ctx->lock, and failures unwind through
// ctls_conn_create_unwind(). It sets the ex_data the SNI and ALPN callbacks
// read, the server mode and one socket BIO. Gives NULL with *err_str set on
// failure.
ctls_conn *ctls_conn_create_server(ctls_ctx *ctx, int fd, void *udata,
                                   char **err_str) {
  if (!ctx || fd < 0) {
    if (err_str) *err_str = "ctls_conn_create_server: invalid arguments";
    return NULL;
  }
  ctls_conn *conn = ctls_conn_alloc(ctx, true);
  if (!conn) {
    if (err_str) *err_str = "ctls_conn_create_server: allocation failure";
    return NULL;
  }
  conn->udata = udata;

  ccol__mutex_lock(ctx->lock);
  SSL *ssl = SSL_new(ctx->ctx_default);
  if (ssl) {
    SSL_CTX_up_ref(ctx->ctx_default);
    conn->pinned_ctx_default = ctx->ctx_default;
    ctls_account_ctx_pins(1);
    conn->ssl = ssl;
  }
  ccol__mutex_unlock(ctx->lock);
  if (!ssl) {
    ctls_conn_create_unwind(conn);
    if (err_str) *err_str = "ctls_conn_create_server: SSL_new failure";
    return NULL;
  }
  if (!SSL_set_ex_data(ssl, ctls_conn_ex_idx(), conn)) {
    ctls_conn_create_unwind(conn);
    if (err_str) *err_str = "ctls_conn_create_server: SSL_set_ex_data failure";
    return NULL;
  }

  BIO *bio = ctls_sock_bio_new(fd);
  if (!bio) {
    ctls_conn_create_unwind(conn);
    if (err_str) *err_str = "ctls_conn_create_server: socket BIO failure";
    return NULL;
  }
  SSL_set_bio(ssl, bio, bio);
  conn->holds_sock_bio = true;
  BIO_set_callback_arg(bio, (char *)&conn->rec_track);
  SSL_set_accept_state(ssl);
  return conn;
}

// Records the protocol conn's handshake agreed on, by copying its name into
// conn's own buffer under tls->lock, and then fires the registration's
// on_selected with no lock held, since it may call back into the same
// context. ctls_conn_handshake_step() calls it in both modes as its last use
// of conn, so the callback may destroy the connection. When nothing was
// agreed, nothing is recorded and no callback fires; reporting a registered
// name would make a client speak a protocol the server never accepted.
static void ctls_record_alpn(ctls_conn *conn) {
  ctls_ctx *tls = conn->ctx;
  const unsigned char *proto = NULL;
  unsigned int proto_len = 0;
  SSL_get0_alpn_selected(conn->ssl, &proto, &proto_len);
  ccol__mutex_lock(tls->lock);
  if (tls->alpn_count == 0) {
    ccol__mutex_unlock(tls->lock);
    return;
  }
  ctls_alpn_entry *matched = NULL;
  if (proto_len > 0) {
    for (size_t i = 0; i < tls->alpn_count; ++i) {
      if (tls->alpn[i].name_len == proto_len &&
          memcmp(tls->alpn[i].name, proto, proto_len) == 0) {
        matched = &tls->alpn[i];
        break;
      }
    }
  }
  if (!matched) {
    ccol__mutex_unlock(tls->lock);
    return;
  }
  memcpy(conn->alpn_selected_buf, matched->name, matched->name_len);
  conn->alpn_selected_buf[matched->name_len] = '\0';
  conn->alpn_selected_name = conn->alpn_selected_buf;
  conn->alpn_selected_len = matched->name_len;
  ctls_alpn_selected_fn cb = matched->on_selected;
  size_t name_len = matched->name_len;
  void *udata = matched->udata;
  ccol__mutex_unlock(tls->lock);
  if (cb) cb(conn, conn->alpn_selected_buf, name_len, udata);
}

// Runs one SSL_accept() or SSL_connect() step on conn, with a clean error
// queue, and maps the result to DONE, WANT_READ, WANT_WRITE or ERROR. On DONE
// it sets handshake_done and records the ALPN result, which may fire the
// callback. See ctls.h for the ThreadSanitizer report inside OpenSSL that
// this call can show.
ctls_handshake_result ctls_conn_handshake_step(ctls_conn *conn) {
  if (!conn || !conn->ssl) return CTLS_HANDSHAKE_ERROR;
  ERR_clear_error();
  int ri = conn->is_server ? SSL_accept(conn->ssl) : SSL_connect(conn->ssl);
  if (ri == 1) {
    conn->handshake_done = true;
    ctls_record_alpn(conn);
    return CTLS_HANDSHAKE_DONE;
  }
  switch (SSL_get_error(conn->ssl, ri)) {
    case SSL_ERROR_WANT_READ:
      return CTLS_HANDSHAKE_WANT_READ;
    case SSL_ERROR_WANT_WRITE:
      return CTLS_HANDSHAKE_WANT_WRITE;
    default:
      return CTLS_HANDSHAKE_ERROR;
  }
}

// Turns an SSL_read() or SSL_write() result ret into the read(2)-like result
// of ctls_conn_read() and ctls_conn_write(): Bytes, 0 for close_notify, or -1
// with errno. SSL_ERROR_SSL and SSL_ERROR_SYSCALL are ECONNRESET (a syscall
// error never becomes a retry that could spin), and the "want" results are
// EWOULDBLOCK with the direction stored in conn->last_io_wants_write. The
// callers clear the error queue before the I/O, because many connections
// share a few threads.
static ssize_t ctls_classify_io_result(ctls_conn *conn, int ret) {
  if (ret > 0) return ret;
  int err = SSL_get_error(conn->ssl, ret);
  switch (err) {
    case SSL_ERROR_ZERO_RETURN:
      return 0;
    case SSL_ERROR_SSL:
      errno = ECONNRESET;
      return -1;
    case SSL_ERROR_SYSCALL:
      if (!errno) errno = ECONNRESET;
      return -1;
    case SSL_ERROR_WANT_WRITE:
      conn->last_io_wants_write = true;
      errno = EWOULDBLOCK;
      return -1;
    default:
      conn->last_io_wants_write = false;
      errno = EWOULDBLOCK;
      return -1;
  }
}

// Clamps a size_t length to the int SSL_read() and SSL_write() take. A plain
// cast would truncate or go negative above INT_MAX; the clamp turns that into
// a short transfer, which the callers already loop on (a streaming handler
// can pass a buffer that large).
static inline int ctls_clamp_io_len(size_t len) {
  return len > (size_t)INT_MAX ? INT_MAX : (int)len;
}

// Tells whether a failed SSL_read() (ret) is the peer closing the socket
// without close_notify on a record boundary, which ctls_conn_read() reports as
// end of data, as curl and Go do. It needs the OpenSSL 3 unexpected-EOF reason
// or a SSL_ERROR_SYSCALL (OpenSSL 1.1, LibreSSL), the BIO's EOF flag, and a
// record tracker that isn't inside a record. A close inside a record stays an
// error, as in Go.
static bool ctls_eof_on_record_boundary(ctls_conn *conn, int ret) {
  int err = SSL_get_error(conn->ssl, ret);
  if (err != SSL_ERROR_SYSCALL && err != SSL_ERROR_SSL) return false;
  BIO *rb = SSL_get_rbio(conn->ssl);
  if (!rb || !BIO_test_flags(rb, CTLS_SOCK_BIO_EOF)) return false;
  if (conn->rec_track.hdr_got != 0 || conn->rec_track.lost) return false;
  if (err == SSL_ERROR_SYSCALL) return true;
#ifdef SSL_R_UNEXPECTED_EOF_WHILE_READING
  unsigned long e = ERR_peek_error();
  return ERR_GET_LIB(e) == ERR_LIB_SSL &&
         ERR_GET_REASON(e) == SSL_R_UNEXPECTED_EOF_WHILE_READING;
#else
  return false;
#endif
}

// Reads up to len decrypted bytes into buf with SSL_read(), clamping the
// length and clearing the error queue first. A len of 0 gives 0 at once (the
// classification would read SSL_read()'s 0 as a reset). A close on a record
// boundary without close_notify gives 0. Otherwise it gives what
// ctls_classify_io_result() makes of the result, and that may set
// last_io_wants_write.
ssize_t ctls_conn_read(ctls_conn *conn, void *buf, size_t len) {
  if (!conn || !conn->ssl) {
    errno = EINVAL;
    return -1;
  }
  if (len == 0) return 0;
  ERR_clear_error();
  int ret = SSL_read(conn->ssl, buf, ctls_clamp_io_len(len));
  if (ret <= 0 && ctls_eof_on_record_boundary(conn, ret)) {
    ERR_clear_error();
    return 0;
  }
  return ctls_classify_io_result(conn, ret);
}

// Writes up to len bytes of buf with SSL_write(), clamping the length and
// clearing the error queue first. A len of 0 gives 0 at once. It gives what
// ctls_classify_io_result() makes of the result, which may set
// last_io_wants_write.
ssize_t ctls_conn_write(ctls_conn *conn, const void *buf, size_t len) {
  if (!conn || !conn->ssl) {
    errno = EINVAL;
    return -1;
  }
  if (len == 0) return 0;
  ERR_clear_error();
  int ret = SSL_write(conn->ssl, buf, ctls_clamp_io_len(len));
  return ctls_classify_io_result(conn, ret);
}

// Gives conn->last_io_wants_write, or false for NULL.
bool ctls_conn_wants_write(const ctls_conn *conn) {
  return conn && conn->last_io_wants_write;
}

// Tells whether conn holds input a read can take without the socket:
// SSL_pending() plus SSL_has_pending() on OpenSSL, SSL_pending() plus the
// record tracker on LibreSSL, which has no SSL_has_pending(). No buffer of
// ours sits between the BIO and OpenSSL, so that's the whole answer. NULL
// gives false.
bool ctls_conn_has_pending_input(const ctls_conn *conn) {
  if (!conn || !conn->ssl) return false;
#if defined(LIBRESSL_VERSION_NUMBER)
  return SSL_pending(conn->ssl) > 0 || conn->rec_track.hdr_got != 0 ||
         conn->rec_track.lost;
#else
  return SSL_pending(conn->ssl) > 0 || SSL_has_pending(conn->ssl) == 1;
#endif
}

// Gives the peer certificate of ssl without a new reference (the session keeps
// one). OpenSSL 1.1.1 and LibreSSL have no SSL_get0_peer_certificate(), so
// their referenced result is dropped at once.
static X509 *ctls_peer_cert0(const SSL *ssl) {
#if defined(LIBRESSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER < 0x30000000L
  X509 *cert = SSL_get_peer_certificate(ssl);
  X509_free(cert);
  return cert;
#else
  return SSL_get0_peer_certificate(ssl);
#endif
}

// Gives SSL_get_verify_result() of conn, so a caller can tell a certificate
// failure from another, or -1 for NULL or a connection without an SSL.
long ctls_conn_verify_result(const ctls_conn *conn) {
  if (!conn || !conn->ssl) return -1;
  return SSL_get_verify_result(conn->ssl);
}

// Tells whether conn's peer presented a certificate that verified: It needs
// a finished handshake, SSL_VERIFY_PEER in the mode, a peer certificate and
// X509_V_OK, because OpenSSL reports X509_V_OK for a peer that sent nothing.
// A resumed session carries the answer of its original handshake.
bool ctls_conn_peer_cert_verified(const ctls_conn *conn) {
  if (!conn || !conn->ssl || !conn->handshake_done) return false;
  return (SSL_get_verify_mode(conn->ssl) & SSL_VERIFY_PEER) != 0 &&
         ctls_peer_cert0(conn->ssl) != NULL &&
         SSL_get_verify_result(conn->ssl) == X509_V_OK;
}

// Makes, once per certificate, the DER encoding and the RFC 2253 subject of
// the verified peer certificate in conn's peer_der and peer_subject, from the
// context's allocator, for the three peer-certificate accessors. It holds a
// reference on the certificate in peer_info_cert, so a renegotiated one makes
// them again. XN_FLAG_RFC2253 escapes every byte above 0x7F and every control
// byte, so the subject is printable ASCII. It gives the certificate, or NULL
// when there's no verified one or an allocation fails (what failed stays
// NULL).
static const X509 *ctls_conn_peer_info(ctls_conn *conn) {
  if (!ctls_conn_peer_cert_verified(conn)) return NULL;
  X509 *cert = ctls_peer_cert0(conn->ssl);
  if (!cert) return NULL;
  if (conn->peer_info_cert == cert) return cert;
  ccol_memmgmt_procs *mp = conn->ctx->m_procs;
  ccol__mem_free(mp, conn->peer_der);
  ccol__mem_free(mp, conn->peer_subject);
  conn->peer_der = NULL;
  conn->peer_der_len = 0;
  conn->peer_subject = NULL;
  X509_free(conn->peer_info_cert);
  conn->peer_info_cert = NULL;

  int der_len = i2d_X509(cert, NULL);
  if (der_len <= 0) {
    ERR_clear_error();
    return NULL;
  }
  unsigned char *der = (unsigned char *)ccol__mem_alloc(mp, (size_t)der_len);
  if (!der) return NULL;
  unsigned char *w = der;
  if (i2d_X509(cert, &w) != der_len) {
    ERR_clear_error();
    ccol__mem_free(mp, der);
    return NULL;
  }

  char *subject = NULL;
  BIO *bio = BIO_new(BIO_s_mem());
  if (bio && X509_NAME_print_ex(bio, X509_get_subject_name(cert), 0,
                                XN_FLAG_RFC2253) >= 0) {
    char *text = NULL;
    long text_len = BIO_get_mem_data(bio, &text);
    if (text_len >= 0) {
      subject = (char *)ccol__mem_alloc(mp, (size_t)text_len + 1);
      if (subject) {
        if (text_len) memcpy(subject, text, (size_t)text_len);
        subject[text_len] = '\0';
      }
    }
  }
  BIO_free(bio);
  ERR_clear_error();
  if (!subject || X509_up_ref(cert) != 1) {
    ccol__mem_free(mp, subject);
    ccol__mem_free(mp, der);
    return NULL;
  }
  conn->peer_der = der;
  conn->peer_der_len = (size_t)der_len;
  conn->peer_subject = subject;
  conn->peer_info_cert = cert;
  return cert;
}

// Gives conn's verified peer certificate in DER, owned by conn, with its
// length in *len_out (may be NULL); NULL and 0 when there's none.
const unsigned char *ctls_conn_peer_cert_der(ctls_conn *conn, size_t *len_out) {
  if (len_out) *len_out = 0;
  if (!ctls_conn_peer_info(conn)) return NULL;
  if (len_out) *len_out = conn->peer_der_len;
  return conn->peer_der;
}

// Writes the SHA-256 of the peer certificate's DER encoding to out. It zeroes
// out first and again on a failed digest, and gives false for a NULL out, no
// verified certificate or a failed digest.
bool ctls_conn_peer_cert_sha256(ctls_conn *conn,
                                unsigned char out[CTLS_SHA256_LEN]) {
  if (!out) return false;
  memset(out, 0, CTLS_SHA256_LEN);
  if (!ctls_conn_peer_info(conn)) return false;
  unsigned int md_len = 0;
  if (EVP_Digest(conn->peer_der, conn->peer_der_len, out, &md_len, EVP_sha256(),
                 NULL) != 1 ||
      md_len != CTLS_SHA256_LEN) {
    ERR_clear_error();
    memset(out, 0, CTLS_SHA256_LEN);
    return false;
  }
  return true;
}

// Gives conn's verified peer certificate subject, owned by conn, or NULL.
const char *ctls_conn_peer_cert_subject(ctls_conn *conn) {
  if (!ctls_conn_peer_info(conn)) return NULL;
  return conn->peer_subject;
}

// Gives the protocol name recorded on conn after the handshake, with its
// length in *len_out (may be NULL), or NULL when none was agreed.
const char *ctls_conn_alpn_selected(const ctls_conn *conn, size_t *len_out) {
  if (!conn || !conn->alpn_selected_name) {
    if (len_out) *len_out = 0;
    return NULL;
  }
  if (len_out) *len_out = conn->alpn_selected_len;
  return conn->alpn_selected_name;
}

// Gives the udata conn was created with, or NULL for NULL.
void *ctls_conn_udata(const ctls_conn *conn) {
  return conn ? conn->udata : NULL;
}

// Destroys conn when its connection ends. After a finished handshake it sends
// close_notify with no lock held (holding ctx->lock across a peer's socket
// would queue every other connection behind it) and clears the thread's error
// queue whatever happened. It then releases the SSL and both pins under
// ctx->lock (see ctls_conn_release_ssl()), frees the peer-certificate cache,
// drops its context reference and frees the record with a by-value copy of
// the procs, since that release may free the context and its procs. NULL is
// fine.
void ctls_conn_destroy(ctls_conn *conn) {
  if (!conn) return;
  if (conn->ssl && conn->handshake_done) SSL_shutdown(conn->ssl);
  ERR_clear_error();
  ctls_conn_release_ssl(conn);
  ccol_memmgmt_procs procs;
  ccol_memmgmt_procs *mp = NULL;
  if (conn->ctx->m_procs) {
    procs = *conn->ctx->m_procs;
    mp = &procs;
  }
  ccol__mem_free(mp, conn->peer_der);
  ccol__mem_free(mp, conn->peer_subject);
  X509_free(conn->peer_info_cert);
  ctls_ctx_release(conn->ctx);
  ccol__mem_free(mp, conn);
}

#ifdef RUNNING_UNIT_TESTS
// Gives conn's SSL, so a test can inspect the agreed peer certificate and
// check the SNI dispatch.
SSL *ctls__conn_ssl_for_tests(ctls_conn *conn) {
  return conn ? conn->ssl : NULL;
}

// Gives the SSL_CTX conn pinned for an SNI dispatch, or NULL, so a test can
// check that the context a connection serves from is one we hold.
SSL_CTX *ctls__conn_pinned_sni_ctx_for_tests(ctls_conn *conn) {
  return conn ? conn->pinned_sni_ctx : NULL;
}

// Gives the SSL_CTX a ClientHello naming server_name would be served from
// (the matching named context, found as the SNI callback finds it, or the
// default), read under tls->lock, so a test can check every name after a
// configuration change without a handshake.
SSL_CTX *ctls__ctx_served_ctx_for_tests(ctls_ctx *tls,
                                        const char *server_name) {
  if (!tls) return NULL;
  ccol__mutex_lock(tls->lock);
  SSL_CTX *found = ctls_find_named_ctx(tls, server_name);
  if (!found) found = tls->ctx_default;
  ccol__mutex_unlock(tls->lock);
  return found;
}
#endif
