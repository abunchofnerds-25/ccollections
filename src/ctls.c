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

/* ========================================================================== */
/*                       PROCESS-WIDE SELF-SIGNED ROOT KEY                    */
/* ========================================================================== */

/* Generated once per process, the first time it's needed, and shared by
 * every self-signed certificate this module generates. The code has the same
 * shape as fio_tls_make_root_key. This state belongs to the whole process
 * and not to one ctls_ctx_t, so a _ccol_once_flag_t guards it instead of a
 * per-instance mutex. */
static struct {
  EVP_PKEY *key;
  _ccol_mutex_t mutex;
  _ccol_once_flag_t once;
  bool finalized;
} ctls_root_key_bundle = {.once = _CCOL_ONCE_INIT};

static void _ctls_root_key_globals_init(void) {
  if (_ccol_mutex_init(ctls_root_key_bundle.mutex) != 0)
    _ccol_fatal_err("ctls root key: failed to initialize mutex");
}

/* The certificates and CRLs of the platform's default CA file, parsed once
 * per process at the first ctls_ctx_trust_system() and shared by every
 * context that trusts the system store. The list never changes once stored:
 * a rebuild adds each certificate to the store of its own SSL_CTX, and that
 * store holds its own reference, so freeing the list at exit leaves every
 * live context intact. The mutex serializes the first load, so concurrent
 * first users parse the file only once, and it orders every read of the list
 * against the free at exit. */
static struct {
  STACK_OF(X509_INFO) * infos;
  _ccol_mutex_t mutex;
  _ccol_once_flag_t once;
  bool finalized;
} ctls_system_ca_bundle = {.once = _CCOL_ONCE_INIT};

static void _ctls_system_ca_globals_init(void) {
  if (_ccol_mutex_init(ctls_system_ca_bundle.mutex) != 0)
    _ccol_fatal_err("ctls system CA bundle: failed to initialize mutex");
}

/* Frees the root key and the parsed default CA file at exit. The handler is
 * registered after the first of the two exists, so after OpenSSL registered
 * its own cleanup, which therefore runs later; a free from a destructor
 * would come after that cleanup. A user that took a reference keeps it.
 * After this handler has run a call gets no root key, and a context that
 * trusts the system store reads and parses the default CA file itself.
 */
static void _ctls_process_state_at_exit(void) {
  _ccol_call_once(ctls_root_key_bundle.once, _ctls_root_key_globals_init);
  _ccol_mutex_lock(ctls_root_key_bundle.mutex);
  EVP_PKEY_free(ctls_root_key_bundle.key);
  ctls_root_key_bundle.key = NULL;
  ctls_root_key_bundle.finalized = true;
  _ccol_mutex_unlock(ctls_root_key_bundle.mutex);

  _ccol_call_once(ctls_system_ca_bundle.once, _ctls_system_ca_globals_init);
  _ccol_mutex_lock(ctls_system_ca_bundle.mutex);
  sk_X509_INFO_pop_free(ctls_system_ca_bundle.infos, X509_INFO_free);
  ctls_system_ca_bundle.infos = NULL;
  ctls_system_ca_bundle.finalized = true;
  _ccol_mutex_unlock(ctls_system_ca_bundle.mutex);
}

static _ccol_once_flag_t ctls_process_state_exit_once = _CCOL_ONCE_INIT;

static void _ctls_process_state_exit_register(void) {
  (void)atexit(_ctls_process_state_at_exit);
}

/* Registers _ctls_process_state_at_exit once. Only call it after some
 * OpenSSL call has run, which initialized OpenSSL and registered its cleanup
 * first. */
static void _ctls_process_state_exit_arm(void) {
  _ccol_call_once(ctls_process_state_exit_once,
                  _ctls_process_state_exit_register);
}

#ifdef RUNNING_UNIT_TESTS
/* Whether the process still holds the root key. */
bool _ctls_root_key_held_for_tests(void) {
  return __atomic_load_n(&ctls_root_key_bundle.key, __ATOMIC_SEQ_CST) != NULL;
}
#endif

/* Gives a reference to the key, which the caller frees with EVP_PKEY_free,
 * or NULL. */
static EVP_PKEY *_ctls_get_root_key(void) {
  _ccol_call_once(ctls_root_key_bundle.once, _ctls_root_key_globals_init);
  _ccol_mutex_lock(ctls_root_key_bundle.mutex);
  if (!ctls_root_key_bundle.key && !ctls_root_key_bundle.finalized) {
    /* EVP_PKEY_CTX key generation exists in OpenSSL 3 and in LibreSSL, and
     * neither marks it deprecated. OpenSSL 3 deprecates the RSA_new, BN_new,
     * RSA_generate_key_ex, EVP_PKEY_assign_RSA sequence, which would fire
     * -Wdeprecated-declarations in the -Werror build, and LibreSSL has no
     * EVP_RSA_gen. A failure leaves the key NULL and the next call tries
     * again. */
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
    if (key) _ctls_process_state_exit_arm();
  }
  EVP_PKEY *key = ctls_root_key_bundle.key;
  if (key && EVP_PKEY_up_ref(key) != 1) key = NULL;
  _ccol_mutex_unlock(ctls_root_key_bundle.mutex);
  return key;
}

/* ========================================================================== */
/*                       ALPN EX-DATA INDEX (SSL* -> ctls_conn_t*) */
/* ========================================================================== */

/* idx starts at -1, the "not registered yet" sentinel _ctls_conn_ex_idx()
 * tests for. A start value of 0 would look exactly like a successful
 * registration, the registration below would never be reached, and this
 * module would silently use ex_data index 0. OpenSSL reserves index 0 for
 * the SSL_set_app_data() and SSL_get_app_data() macros, so any other code in
 * the process that gets an SSL* and uses those macros would read this
 * module's ctls_conn_t* through them, and overwrite it. SSL_get_ex_new_index()
 * also reports its own failure as -1, so a failed registration leaves the
 * sentinel in place and the next call tries again. The two SSL_set_ex_data()
 * call sites already report a failure to their own caller. */
static struct {
  int idx;
  _ccol_mutex_t mutex;
  _ccol_once_flag_t once;
} ctls_conn_ex_idx_bundle = {.idx = -1};

#ifdef RUNNING_UNIT_TESTS
/* Counts the _ctls_conn_ex_idx() calls that take its mutex. */
static long g_ctls_ex_idx_locked_calls_for_tests = 0;
long _ctls_ex_idx_locked_calls_for_tests(void) {
  return __atomic_load_n(&g_ctls_ex_idx_locked_calls_for_tests,
                         __ATOMIC_RELAXED);
}
#endif

static void _ctls_ex_idx_globals_init(void) {
  if (_ccol_mutex_init(ctls_conn_ex_idx_bundle.mutex) != 0)
    _ccol_fatal_err("ctls conn ex_data index: failed to initialize mutex");
}

/* Every connection create and every SNI and ALPN callback asks for the
 * index, so a registered index is read with one acquire load and no lock.
 * Only a call that finds the sentinel takes the mutex, registers, and
 * publishes the index with a release store. */
static int _ctls_conn_ex_idx(void) {
  int idx = __atomic_load_n(&ctls_conn_ex_idx_bundle.idx, __ATOMIC_ACQUIRE);
  if (__builtin_expect(idx >= 0, 1)) return idx;
#ifdef RUNNING_UNIT_TESTS
  __atomic_fetch_add(&g_ctls_ex_idx_locked_calls_for_tests, 1,
                     __ATOMIC_RELAXED);
#endif
  _ccol_call_once(ctls_conn_ex_idx_bundle.once, _ctls_ex_idx_globals_init);
  _ccol_mutex_lock(ctls_conn_ex_idx_bundle.mutex);
  idx = __atomic_load_n(&ctls_conn_ex_idx_bundle.idx, __ATOMIC_RELAXED);
  if (idx < 0) {
    idx = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
    __atomic_store_n(&ctls_conn_ex_idx_bundle.idx, idx, __ATOMIC_RELEASE);
  }
  _ccol_mutex_unlock(ctls_conn_ex_idx_bundle.mutex);
  return idx;
}

/* ========================================================================== */
/*                  SOCKET BIO THAT NEVER RAISES SIGPIPE                      */
/* ========================================================================== */

/* Every connection of this module reads and writes through this BIO_METHOD,
 * a socket BIO that behaves like OpenSSL's socket BIO except that it writes
 * with send(2) and MSG_NOSIGNAL, where OpenSSL's writes with write(2). A
 * write(2) to a stream socket whose peer is gone raises SIGPIPE, and the
 * default SIGPIPE disposition ends the whole process, while a peer resetting
 * a keep-alive connection is ordinary network behavior, and the close_notify
 * ctls_conn_destroy() sends goes to exactly such a socket. With MSG_NOSIGNAL
 * that write fails with EPIPE instead and the caller sees an ordinary I/O
 * error, whatever SIGPIPE disposition the application picked. We fix this
 * per connection on purpose; a process-wide SIG_IGN would change a setting
 * that belongs to the application.
 *
 * The read side, the retry flags, the end-of-file flag and the control
 * queries match OpenSSL's socket BIO, because OpenSSL's record layer reads
 * them: BIO_eof() is what tells an abrupt close by the peer from a retryable
 * read, and the poll descriptors are what OpenSSL reports through
 * SSL_get_rpoll_descriptor(). The type carries BIO_TYPE_DESCRIPTOR, so
 * BIO_find_type() and SSL_get_fd() find it. The file descriptor lives in the
 * BIO's data pointer, so a write reads it without a control call.
 *
 * The method outlives every BIO built from it: each BIO this module creates
 * holds one count on it, and the count drops once SSL_free() has freed that
 * BIO. The destructor at process exit frees the method only when no BIO
 * holds it; if a connection is live then, the destructor marks the method
 * retired and the release of the last BIO frees it. The order of a process's
 * destructors isn't ours to control, and a later destructor can tear down a
 * TLS connection and write its close_notify through this method. A
 * connection created after the destructor ran builds a fresh method under
 * the same rule. The mutex is never destroyed, because a release can come
 * after the destructor. */
static struct {
  BIO_METHOD *meth;
  size_t live_bios;
  bool retired;
  _ccol_mutex_t mutex;
  _ccol_once_flag_t once;
} ctls_sock_bio_bundle = {.once = _CCOL_ONCE_INIT};

static void _ctls_sock_bio_globals_init(void) {
  if (_ccol_mutex_init(ctls_sock_bio_bundle.mutex) != 0)
    _ccol_fatal_err("ctls socket BIO: failed to initialize mutex");
}

/* The socket BIO's end-of-file state, kept in the BIO's flags and read back
 * through BIO_CTRL_EOF. OpenSSL names this bit; LibreSSL doesn't, and there
 * it's a bit that no LibreSSL BIO flag uses. */
#ifdef BIO_FLAGS_IN_EOF
#define _CTLS_SOCK_BIO_EOF BIO_FLAGS_IN_EOF
#else
#define _CTLS_SOCK_BIO_EOF 0x800
#endif

/* The socket BIO follows the record framing of every byte it hands to the
 * TLS layer, to answer two questions. ctls_conn_read() asks whether a peer
 * that closed the socket without close_notify did it on a record boundary
 * (see _ctls_eof_on_record_boundary()). And on LibreSSL, which has no
 * SSL_has_pending(), ctls_conn_has_pending_input() asks whether the TLS
 * layer holds part of a record it can't decrypt yet. A record is a 5-byte
 * header, whose last two bytes give the body length, followed by the body.
 * With read-ahead off, the default and something this module never changes,
 * the TLS layer only asks for the bytes of one record, and it processes a
 * record in the same call that reads its last byte. So the TLS layer holds
 * part of a record exactly while the BIO is inside a record, that is, while
 * hdr_got isn't 0.
 *
 * A record header starts with a content type from 20 to 24. Any other first
 * byte, like the high bit of an SSLv2-format client hello, means the framing
 * isn't the one we follow, so the tracker gives up on that connection and
 * reports pending input from then on. The caller then reads until the
 * socket has nothing left, which is always safe. */
typedef struct {
  uint32_t body_left; /* body bytes the BIO hasn't given yet */
  uint8_t hdr_got;    /* header bytes given, 0 to 5; 5 while in the body */
  uint8_t len_hi;     /* high byte of the body length */
  bool lost;          /* the framing couldn't be followed */
} _ctls_rec_track_t;

static void _ctls_rec_track_feed(_ctls_rec_track_t *rt, const char *p, int n) {
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
/* Feeds the given chunks in order to a fresh tracker and gives what
 * ctls_conn_has_pending_input() would answer from the framing alone. */
bool _ctls_rec_track_pending_after_for_tests(const char *const *chunks,
                                             const int *lens, int count) {
  _ctls_rec_track_t rt = {0};
  for (int i = 0; i < count; i++) _ctls_rec_track_feed(&rt, chunks[i], lens[i]);
  return rt.hdr_got != 0 || rt.lost;
}
#endif

static inline int _ctls_sock_bio_fd(BIO *b) {
  return (int)(intptr_t)BIO_get_data(b);
}

static int _ctls_sock_bio_write(BIO *b, const char *in, int inl) {
  errno = 0;
  ssize_t n = send(_ctls_sock_bio_fd(b), in, (size_t)inl, CCOL_MSG_NOSIGNAL);
  int ret = n < 0 ? -1 : (int)n;
  BIO_clear_retry_flags(b);
  if (ret <= 0 && BIO_sock_should_retry(ret)) BIO_set_retry_write(b);
  return ret;
}

static int _ctls_sock_bio_read(BIO *b, char *out, int outl) {
  if (!out) return 0;
  errno = 0;
  ssize_t n = recv(_ctls_sock_bio_fd(b), out, (size_t)outl, 0);
  int ret = n < 0 ? -1 : (int)n;
  BIO_clear_retry_flags(b);
  if (ret <= 0) {
    if (BIO_sock_should_retry(ret))
      BIO_set_retry_read(b);
    else if (ret == 0)
      BIO_set_flags(b, _CTLS_SOCK_BIO_EOF);
  }
  /* The connection hands the BIO its framing state through the callback
   * argument, a plain field that only a BIO callback reads, and this BIO
   * has no callback. */
  _ctls_rec_track_t *rt = (_ctls_rec_track_t *)BIO_get_callback_arg(b);
  if (ret > 0 && rt) _ctls_rec_track_feed(rt, out, ret);
  return ret;
}

static int _ctls_sock_bio_puts(BIO *b, const char *str) {
  return _ctls_sock_bio_write(b, str, (int)strlen(str));
}

static long _ctls_sock_bio_ctrl(BIO *b, int cmd, long num, void *ptr) {
  switch (cmd) {
    case BIO_C_SET_FD:
      if (BIO_get_shutdown(b) && BIO_get_init(b)) close(_ctls_sock_bio_fd(b));
      BIO_clear_flags(b, ~0);
      BIO_set_data(b, (void *)(intptr_t)*(int *)ptr);
      BIO_set_shutdown(b, (int)num);
      BIO_set_init(b, 1);
      return 1;
    case BIO_C_GET_FD:
      if (!BIO_get_init(b)) return -1;
      if (ptr) *(int *)ptr = _ctls_sock_bio_fd(b);
      return _ctls_sock_bio_fd(b);
    case BIO_CTRL_GET_CLOSE:
      return BIO_get_shutdown(b);
    case BIO_CTRL_SET_CLOSE:
      BIO_set_shutdown(b, (int)num);
      return 1;
    case BIO_CTRL_DUP:
    case BIO_CTRL_FLUSH:
      return 1;
    case BIO_CTRL_EOF:
      return BIO_test_flags(b, _CTLS_SOCK_BIO_EOF) != 0;
#ifdef BIO_CTRL_GET_RPOLL_DESCRIPTOR
    case BIO_CTRL_GET_RPOLL_DESCRIPTOR:
    case BIO_CTRL_GET_WPOLL_DESCRIPTOR: {
      if (!BIO_get_init(b)) return 0;
      BIO_POLL_DESCRIPTOR *pd = (BIO_POLL_DESCRIPTOR *)ptr;
      pd->type = BIO_POLL_DESCRIPTOR_TYPE_SOCK_FD;
      pd->value.fd = _ctls_sock_bio_fd(b);
      return 1;
    }
#endif
    default:
      return 0;
  }
}

static int _ctls_sock_bio_create(BIO *b) {
  BIO_set_init(b, 0);
  BIO_set_data(b, NULL);
  BIO_clear_flags(b, ~0);
  return 1;
}

static int _ctls_sock_bio_destroy(BIO *b) {
  if (!b) return 0;
  if (BIO_get_shutdown(b) && BIO_get_init(b)) close(_ctls_sock_bio_fd(b));
  BIO_set_init(b, 0);
  BIO_clear_flags(b, ~0);
  return 1;
}

static BIO_METHOD *_ctls_sock_bio_method_build(void) {
  int type = BIO_get_new_index();
  if (type == -1) return NULL;
  BIO_METHOD *m = BIO_meth_new(
      type | BIO_TYPE_SOURCE_SINK | BIO_TYPE_DESCRIPTOR, "ccol socket");
  if (!m) return NULL;
  if (!BIO_meth_set_write(m, _ctls_sock_bio_write) ||
      !BIO_meth_set_read(m, _ctls_sock_bio_read) ||
      !BIO_meth_set_puts(m, _ctls_sock_bio_puts) ||
      !BIO_meth_set_ctrl(m, _ctls_sock_bio_ctrl) ||
      !BIO_meth_set_create(m, _ctls_sock_bio_create) ||
      !BIO_meth_set_destroy(m, _ctls_sock_bio_destroy)) {
    BIO_meth_free(m);
    return NULL;
  }
  return m;
}

static void _ctls_sock_bio_release(void);

/* Builds a socket BIO over fd that doesn't close fd, and charges one count
 * on the method. Gives NULL on failure, and then charges nothing. */
static BIO *_ctls_sock_bio_new(int fd) {
  /* The socket comes from the caller, so where SIGPIPE is suppressed per
   * socket rather than per send, the BIO arranges that itself, and refuses
   * a socket where it can't. */
  if (ccol_sock_nosigpipe(fd) != 0) return NULL;
  _ccol_call_once(ctls_sock_bio_bundle.once, _ctls_sock_bio_globals_init);
  _ccol_mutex_lock(ctls_sock_bio_bundle.mutex);
  if (!ctls_sock_bio_bundle.meth) {
    ctls_sock_bio_bundle.meth = _ctls_sock_bio_method_build();
    ctls_sock_bio_bundle.retired = false;
  }
  /* The count is charged before unlocking, so the method can't be freed
   * while BIO_new() below uses it outside the lock. */
  BIO_METHOD *meth = ctls_sock_bio_bundle.meth;
  if (meth) ctls_sock_bio_bundle.live_bios++;
  _ccol_mutex_unlock(ctls_sock_bio_bundle.mutex);
  if (!meth) return NULL;
  BIO *bio = BIO_new(meth);
  if (!bio) {
    _ctls_sock_bio_release();
    return NULL;
  }
  BIO_set_fd(bio, fd, BIO_NOCLOSE);
  return bio;
}

/* Drops the count of one BIO that's already freed, and frees the method if
 * the destructor has retired it and this was the last BIO. */
static void _ctls_sock_bio_release(void) {
  _ccol_mutex_lock(ctls_sock_bio_bundle.mutex);
  if (--ctls_sock_bio_bundle.live_bios == 0 && ctls_sock_bio_bundle.retired) {
    BIO_meth_free(ctls_sock_bio_bundle.meth);
    ctls_sock_bio_bundle.meth = NULL;
    ctls_sock_bio_bundle.retired = false;
  }
  _ccol_mutex_unlock(ctls_sock_bio_bundle.mutex);
}

__attribute__((destructor)) static void _ctls_sock_bio_cleanup(void) {
  _ccol_call_once(ctls_sock_bio_bundle.once, _ctls_sock_bio_globals_init);
  _ccol_mutex_lock(ctls_sock_bio_bundle.mutex);
  if (ctls_sock_bio_bundle.meth) {
    if (ctls_sock_bio_bundle.live_bios == 0) {
      BIO_meth_free(ctls_sock_bio_bundle.meth);
      ctls_sock_bio_bundle.meth = NULL;
    } else {
      ctls_sock_bio_bundle.retired = true;
    }
  }
  _ccol_mutex_unlock(ctls_sock_bio_bundle.mutex);
}

#ifdef RUNNING_UNIT_TESTS
/* White-box accessor giving the number of this module's BIOs that are
 * alive, so a suite can assert that every connection gives its count
 * back. */
size_t _ctls_live_sock_bio_count_for_tests(void) {
  _ccol_call_once(ctls_sock_bio_bundle.once, _ctls_sock_bio_globals_init);
  _ccol_mutex_lock(ctls_sock_bio_bundle.mutex);
  size_t n = ctls_sock_bio_bundle.live_bios;
  _ccol_mutex_unlock(ctls_sock_bio_bundle.mutex);
  return n;
}
#endif

/* ========================================================================== */
/*                              INTERNAL TYPES                                */
/* ========================================================================== */

typedef struct ctls_alpn_entry {
  char *name;
  size_t name_len;
  ctls_alpn_selected_fn on_selected;
  void *udata;
  ctls_alpn_cleanup_fn on_cleanup;
} ctls_alpn_entry;

/* One named certificate, dispatched by SNI. It holds the stored
 * configuration and its own fully built SSL_CTX, which is rebuilt every time
 * the state of the whole ctx (the trust store or the ALPN protocols)
 * changes. */
typedef struct ctls_named_cert {
  char *cert_pem;
  size_t cert_len;
  char *key_pem;
  size_t key_len;
  char *pk_password;
  bool self_signed;
  char *self_signed_name; /* subject name of a self_signed entry */
  SSL_CTX *built_ctx;     /* NULL until the first successful rebuild */
} ctls_named_cert;

struct ctls_ctx {
  size_t ref;
  _ccol_mutex_t lock;
  ccol_memmgmt_procs_t *m_procs; /* owned copy, or NULL for default alloc */

  /* The default certificate, selected by a server_name of NULL or "" in
   * ctls_ctx_cert_add. */
  bool has_default_cert;
  char *default_cert_pem;
  size_t default_cert_len;
  char *default_key_pem;
  size_t default_key_len;
  char *default_pk_password;
  bool default_self_signed;
  char *default_self_signed_name;

  /* The named certificates, dispatched by SNI. A chmap from a char*
   * hostname to a ctls_named_cert*. */
  chmap named_certs;
  bool sni_callback_installed;

  /* The trust store: an array of the loaded CA-bundle PEM blobs. */
  char **trust_pems;
  size_t *trust_lens;
  size_t trust_count;
  size_t trust_cap;
  bool verify_default_store;
  bool verify_peer;
  /* With false, the default, a server-mode connection with a trust store
   * refuses a peer that presents no certificate; with true it only requests
   * one. See ctls_ctx_peer_cert_optional(). */
  bool peer_cert_optional;

  /* The ALPN protocol registrations in registration order, the default one
   * at index 0. */
  ctls_alpn_entry *alpn;
  size_t alpn_count;
  size_t alpn_cap;

  SSL_CTX
  *ctx_default; /* built from the fields above. NULL until the first build */
};

struct ctls_conn {
  SSL *ssl;
  ctls_ctx_t *ctx; /* retained */
  bool is_server;
  bool handshake_done;
  void *udata;
  const char *alpn_selected_name; /* points into alpn_selected_buf below
                                      after a selection, or NULL */
  size_t alpn_selected_len;
  /* The connection's own copy of the selected ALPN protocol name. The
   * documented limit of ctls_ctx_alpn_add is 1 to 255 bytes, and the buffer
   * holds that plus one byte of headroom. _ctls_alpn_select_cb fills it in
   * server mode and _ctls_record_alpn in both modes, and neither of them
   * points alpn_selected_name into the tls->alpn[] storage of the owning
   * ctls_ctx_t, because that storage is mutable: a concurrent
   * ctls_ctx_alpn_add() that replaces or adds a protocol can
   * _ccol_mem_free() an existing entry's name buffer, or _ccol_mem_realloc()
   * the whole array. The doc comment of ctls_ctx_retain() documents this
   * kind of concurrent configuration change under live traffic as a
   * supported use case, not a hypothetical one, so a connection's selected
   * protocol name can't stay a raw pointer into that storage for the life of
   * the connection. */
  char alpn_selected_buf[256];
  /* _ctls_classify_io_result sets this every time ctls_conn_read() or
   * ctls_conn_write() returns -1 with EWOULDBLOCK, to the direction OpenSSL
   * reported for that one call (SSL_ERROR_WANT_READ or
   * SSL_ERROR_WANT_WRITE). The doc comment of ctls_conn_wants_write() in
   * ctls.h explains why it exists: after the handshake SSL_read() and
   * SSL_write() can each need the OPPOSITE direction from what the call's
   * name suggests, like flushing a deferred TLS 1.3 session ticket inside
   * SSL_read(), or a read driven by a TLS 1.2 renegotiation inside
   * SSL_write(). A plain read(2) or write(2) is different, since its own
   * EWOULDBLOCK always means "wait for this same direction". The field keeps
   * the value of the last classified call and only means something right
   * after a read or write call that returned -1 with EWOULDBLOCK. */
  bool last_io_wants_write;
  /* These two fields name every SSL_CTX this connection can still reach. We
   * take our own reference on each with SSL_CTX_up_ref, and
   * _ctls_conn_release_ssl() releases both.
   *
   *   pinned_ctx_default: the SSL_CTX* that ctx->ctx_default held when
   *     SSL_new() was called for this connection. The pin is taken in the
   *     same ctx->lock critical section as that call, so it always names
   *     the object the connection's SSL* was created against. It is NULL if
   *     SSL_new() failed before the pin could be taken. OpenSSL reads this
   *     object for the connection's whole life through its own separate
   *     session-context field, even after an SNI dispatch has moved the
   *     connection's active context to another object.
   *
   *   pinned_sni_ctx: the SSL_CTX of the SNI-dispatched named certificate,
   *     which _ctls_servername_cb installs with SSL_set_SSL_CTX(). The pin
   *     is taken in the same ctx->lock critical section as the lookup that
   *     found the object and the install that put it on the connection. The
   *     field is NULL for a connection that never reaches the servername
   *     callback (every client connection, and every server connection
   *     whose peer sends no SNI extension), and also for one whose SNI name
   *     matches no named certificate. All of those keep serving from
   *     pinned_ctx_default.
   *
   * OpenSSL takes references of its own on both objects: SSL_new takes one
   * for the connection's active context and one for its session context,
   * and SSL_set_SSL_CTX takes one on the context it installs and drops the
   * one on the context it replaces. So these pins aren't the only thing
   * keeping either object alive. What they do is make the guarantee our own:
   * a context a live connection can still reach stays alive because this
   * module holds a reference on it, not because some third party's internal
   * field bookkeeping happens to.
   *
   * Both pins are released under ctx->lock, because the commit step of
   * _ctls_ctx_rebuild_locked frees every replaced context under that same
   * lock, and SSL_free() is called under it for the same reason. Released
   * outside that lock, the ordering between "this connection stopped reading
   * a shared context" and "another thread may now destroy that context"
   * would be left to OpenSSL alone, carried only by its internal reference
   * counter, an atomic inside a library nothing instruments. ThreadSanitizer
   * can't see that atomic, so it reports the configuration-changing thread's
   * destroy of a context as a race against this connection's earlier reads
   * through the same context. Taking ctx->lock for the release provides the
   * missing ordering from a primitive this module owns. */
  SSL_CTX *pinned_ctx_default;
  SSL_CTX *pinned_sni_ctx;
  /* True while this connection's SSL* owns a BIO that charged a count on
   * the socket BIO method. See ctls_sock_bio_bundle. */
  bool holds_sock_bio;
  /* The record framing the socket BIO follows; see _ctls_rec_track_t. The
   * SSL* frees the BIO before the connection record is freed. */
  _ctls_rec_track_t rec_track;
  /* The DER encoding and the RFC 2253 subject of the peer's verified
   * certificate, made the first time a caller asks and kept for the life of
   * the connection; see ctls_conn_peer_cert_der(). peer_info_cert names the
   * certificate they describe, so a different certificate, which a
   * renegotiation can bring, makes them again. The connection holds a
   * reference on it, so its address can never name some other certificate
   * while the cache describes it. Both buffers belong to the context's
   * allocator. */
  X509 *peer_info_cert;
  unsigned char *peer_der;
  size_t peer_der_len;
  char *peer_subject;
};

#ifdef RUNNING_UNIT_TESTS
/* How many SSL_CTX references this module holds for live connections; see
 * the pin fields of struct ctls_conn. Every pin taken adds one and every pin
 * released subtracts one, so a suite can assert two things: a connection
 * pins exactly the contexts it can reach, and the total comes back to its
 * starting value once every connection is destroyed. The second one turns
 * "a context rebuilt many times doesn't collect references" from a claim
 * into a measurement.
 *
 * The counter is relaxed. A pin is taken and released on the thread that
 * drives that connection, so several threads write the counter at once and
 * it must be atomic; otherwise the counter itself would be a data race in
 * code every sanitizer run compiles. Nothing orders it beyond the ctx->lock
 * around it, and a test only reads it after joining the threads under test.
 */
static _Atomic long ctls_live_conn_ctx_pins = 0;

static inline void _ctls_account_ctx_pins(long delta) {
  atomic_fetch_add_explicit(&ctls_live_conn_ctx_pins, delta,
                            memory_order_relaxed);
}

long _ctls_live_conn_ctx_pin_count_for_tests(void) {
  return atomic_load_explicit(&ctls_live_conn_ctx_pins, memory_order_relaxed);
}
#else
static inline void _ctls_account_ctx_pins(long delta) { (void)delta; }
#endif

/* ========================================================================== */
/*                             SMALL HELPERS                                  */
/* ========================================================================== */

static char *_ctls_strdup(ccol_memmgmt_procs_t *mp, const char *s) {
  if (!s) return NULL;
  size_t len = strlen(s) + 1;
  char *d = (char *)_ccol_mem_alloc(mp, len);
  if (!d) return NULL;
  memcpy(d, s, len);
  return d;
}

static char *_ctls_strdup_lower(ccol_memmgmt_procs_t *mp, const char *s) {
  if (!s) return NULL;
  size_t len = strlen(s) + 1;
  char *d = (char *)_ccol_mem_alloc(mp, len);
  if (!d) return NULL;
  /* DNS names compare without regard to ASCII case only (RFC 4343), so the
   * fold must not follow the process's locale. */
  for (size_t i = 0; i < len; ++i)
    d[i] = (char)ccol_ascii_tolower((unsigned char)s[i]);
  return d;
}

/* This ceiling bounds the allocation a caller's path can cause in this
 * function. PEM artefacts are small: a certificate or a private key is a few
 * kilobytes, and a full system CA bundle (the Mozilla set) a few hundred
 * kilobytes. 16 MB is far above any legitimate input, and it keeps a path
 * pointing at something else entirely (a log, a disk image or a core dump) a
 * fast, clean error instead of a multi-gigabyte allocation. The type is
 * off_t, matching the size fstat(2) reports and that we compare it with, so
 * the comparison involves no signedness conversion. */
#define CTLS_MAX_PEM_FILE_SIZE ((off_t)16 * 1024 * 1024)

/* Reads a whole file into a new buffer. Returns false on every failure (a
 * missing file, a path that doesn't name a regular file, a read error, or a
 * zero-length file, which is never a meaningful certificate, key or CA
 * bundle) and leaves no partial allocation behind.
 *
 * The open never waits. A path naming a FIFO without a writer, a terminal or
 * a device blocks a plain open(2) or its first read forever, and the caller
 * may hold a client's or a server's lock while it reads. With O_NONBLOCK the
 * open of such a path returns at once, the type check on the opened
 * descriptor then refuses it before a byte is read, and O_NONBLOCK has no
 * effect on reads of a regular file. The check runs on the descriptor and
 * not on the path, so a path that something swaps between a check and the
 * open can't sneak past it. O_NOCTTY keeps a terminal from becoming the
 * process's controlling terminal, and O_CLOEXEC keeps a process another
 * thread spawns in the meantime from inheriting the descriptor. */
static bool _ctls_read_file(ccol_memmgmt_procs_t *mp, const char *path,
                            char **out_data, size_t *out_len, char **why) {
  *out_data = NULL;
  *out_len = 0;
  /* Every failure below keeps this string when it has nothing more exact to
   * say, so a caller never reports a bare "unreadable" for a file it could
   * read just fine. */
  if (why) *why = "file is missing or unreadable";
  if (!path) return false;
  int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
  if (fd < 0) return false;
  /* Only a regular file has a meaningful size to read, and a certificate or
   * CA-bundle option pointing at a directory, a FIFO or a device is an
   * ordinary configuration slip; OpenSSL itself keeps CAfile apart from
   * CApath for this reason. */
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    if (why) *why = "path does not name a regular file";
    close(fd);
    return false;
  }
  if (st.st_size <= 0) {
    close(fd);
    return false;
  }
  if (st.st_size > CTLS_MAX_PEM_FILE_SIZE) {
    if (why) *why = "file is larger than the 16 MB limit for a PEM artefact";
    close(fd);
    return false;
  }
  size_t sz = (size_t)st.st_size;
  char *buf = (char *)_ccol_mem_alloc(mp, sz);
  if (!buf) {
    close(fd);
    return false;
  }
  size_t got = 0;
  while (got < sz) {
    ssize_t n = read(fd, buf + got, sz - got);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    got += (size_t)n;
  }
  close(fd);
  /* A file that shrank since the fstat reads short, and we refuse it
   * instead of parsing part of it. */
  if (got != sz) {
    _ccol_mem_free(mp, buf);
    return false;
  }
  *out_data = buf;
  *out_len = sz;
  return true;
}

/* ========================================================================== */
/*                       DEFAULT CA FILE OF THE PLATFORM                      */
/* ========================================================================== */

/* Reads an environment variable only for a process running with the
 * privileges of the user who started it, as OpenSSL does for SSL_CERT_FILE
 * and SSL_CERT_DIR: a set-user-ID program must not let its caller pick the
 * trust anchors. */
static const char *_ctls_trusted_getenv(const char *name) {
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
static atomic_long g_ctls_system_ca_parses_for_tests;

/* How many times the process parsed the default CA file. */
long _ctls_system_ca_parses_for_tests(void) {
  return atomic_load(&g_ctls_system_ca_parses_for_tests);
}
#endif

/* Reads the default CA file (SSL_CERT_FILE when the process may trust its
 * environment, otherwise the file OpenSSL was built with) through
 * _ctls_read_file, so the descriptor is close-on-exec, and parses it. Gives
 * NULL when the file is missing, unreadable, not a regular file, or holds no
 * certificate. */
static STACK_OF(X509_INFO) * _ctls_system_ca_load(void) {
  const char *path = _ctls_trusted_getenv(X509_get_default_cert_file_env());
  if (!path || !*path) path = X509_get_default_cert_file();
  char *pem = NULL;
  size_t len = 0;
  if (!_ctls_read_file(NULL, path, &pem, &len, NULL)) return NULL;
  STACK_OF(X509_INFO) *infos = NULL;
  /* _ctls_read_file caps the size at 16 MB, so the length fits in an int. */
  BIO *bio = BIO_new_mem_buf(pem, (int)len);
  if (bio) {
    infos = PEM_X509_INFO_read_bio(bio, NULL, NULL, NULL);
    BIO_free(bio);
  }
  _ccol_default_mem_free(pem);
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
  /* A file that doesn't parse leaves errors on the queue, and they must not
   * decide how the rebuild around this call reports some later failure. */
  ERR_clear_error();
  return infos;
}

/* Adds the certificate and the CRL of one X509_INFO to store, each with its
 * own reference. An object the store already holds counts as added: OpenSSL
 * 1.1.1 and later accept such an add, and a TLS library that refuses it
 * reports X509_R_CERT_ALREADY_IN_HASH_TABLE, which we take off the error
 * queue again, so one CA appearing in two bundles never fails a rebuild.
 * Gives false for any other failure, which only an allocation failure can
 * cause, and leaves its reason on the error queue. */
static bool _ctls_store_add_info(X509_STORE *store, X509_INFO *info) {
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

/* Adds every certificate and CRL of infos to store. Gives false when an add
 * fails; see _ctls_store_add_info. */
static bool _ctls_store_add_infos(X509_STORE *store,
                                  STACK_OF(X509_INFO) * infos) {
  for (int i = 0; i < sk_X509_INFO_num(infos); ++i)
    if (!_ctls_store_add_info(store, sk_X509_INFO_value(infos, i)))
      return false;
  return true;
}

/* Adds the platform's default CA file to store. The file is read and parsed
 * once per process, and a failed load isn't remembered, so the next call
 * reads the file again. Gives 1 when the file's certificates went into
 * store, 0 when there's no usable default file, and -1 when an add failed. */
static int _ctls_store_add_system_file(X509_STORE *store) {
  _ccol_call_once(ctls_system_ca_bundle.once, _ctls_system_ca_globals_init);
  _ccol_mutex_lock(ctls_system_ca_bundle.mutex);
  STACK_OF(X509_INFO) *infos = ctls_system_ca_bundle.infos;
  bool owned = false;
  if (!infos) {
    infos = _ctls_system_ca_load();
    if (infos && !ctls_system_ca_bundle.finalized) {
      ctls_system_ca_bundle.infos = infos;
      _ctls_process_state_exit_arm();
    } else {
      /* After the exit handler nothing would ever free a stored list. */
      owned = true;
    }
  }
  int rv = 0;
  if (infos) rv = _ctls_store_add_infos(store, infos) ? 1 : -1;
  if (owned) sk_X509_INFO_pop_free(infos, X509_INFO_free);
  _ccol_mutex_unlock(ctls_system_ca_bundle.mutex);
  return rv;
}

#ifdef RUNNING_UNIT_TESTS
/* Drops the parsed default CA file so the next context that trusts the
 * system store reads the file again, for example after a test changed
 * SSL_CERT_FILE. */
void _ctls_system_ca_forget_for_tests(void) {
  _ccol_call_once(ctls_system_ca_bundle.once, _ctls_system_ca_globals_init);
  _ccol_mutex_lock(ctls_system_ca_bundle.mutex);
  sk_X509_INFO_pop_free(ctls_system_ca_bundle.infos, X509_INFO_free);
  ctls_system_ca_bundle.infos = NULL;
  _ccol_mutex_unlock(ctls_system_ca_bundle.mutex);
}
#endif

/* Installs the platform's trust anchors on store. The default CA file is the
 * source when it exists and holds a certificate. Only when it doesn't does
 * the store get OpenSSL's hashed-directory lookup instead (SSL_CERT_DIR when
 * the process may trust its environment, otherwise the directory OpenSSL was
 * built with), which then opens that directory's files during each
 * handshake that needs them. */
static bool _ctls_store_add_system_trust(X509_STORE *store) {
  int from_file = _ctls_store_add_system_file(store);
  if (from_file < 0) return false;
  if (from_file > 0) return true;
  X509_LOOKUP *lookup = X509_STORE_add_lookup(store, X509_LOOKUP_hash_dir());
  if (!lookup) return false;
  /* A missing directory isn't an error here, exactly as in
   * SSL_CTX_set_default_verify_paths: the store then trusts nothing from it
   * and every verification fails. */
  (void)X509_LOOKUP_add_dir(lookup, NULL, X509_FILETYPE_DEFAULT);
  return true;
}

typedef struct ctls_pw_ctx {
  const char *password;
} ctls_pw_ctx;

static int _ctls_pem_passwd_cb(char *buf, int size, int rwflag, void *u) {
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

/* ========================================================================== */
/*                       X509 SELF-SIGNED CERT GENERATION                     */
/* ========================================================================== */

static X509 *_ctls_create_self_signed_with(const char *server_name,
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
      !X509_gmtime_adj(X509_get_notAfter(cert), 15552000L) /* 180 days */) {
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

static X509 *_ctls_create_self_signed(const char *server_name) {
  EVP_PKEY *root_key = _ctls_get_root_key();
  if (!root_key) return NULL;
  X509 *cert = _ctls_create_self_signed_with(server_name, root_key);
  EVP_PKEY_free(root_key);
  return cert;
}

/* ========================================================================== */
/*                          SSL_CTX (RE)BUILDING                              */
/* ========================================================================== */

/* The session id context every SSL_CTX of this module carries; see
 * _ctls_apply_base_settings(). Using one constant for every context is what
 * keeps resumption working across an SNI dispatch: SSL_set_SSL_CTX() only
 * swaps a connection's session id context for the new context's one when
 * the connection still carries the old context's one, and then the two are
 * byte for byte the same. The value isn't meant to tell one ctls_ctx_t from
 * another. The session cache and the ticket keys belong to the SSL_CTX a
 * connection starts on, which is the default context of one ctls_ctx_t and
 * of one build of it, so a session never crosses from one ctls_ctx_t to
 * another in the first place. */
static const unsigned char _ctls_session_id_context[] = "ccol-ctls";

/* Applies the five explicit settings every SSL_CTX of this module carries,
 * and nothing else; OpenSSL's defaults for the session cache and tickets are
 * left alone.
 *
 * SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER lets a caller retry a write that
 * reported "want write" from a different address with the same bytes. A
 * server that parks a slow reader copies the unsent tail of its response
 * into storage of its own and retries from there on another thread, and
 * without this mode OpenSSL refuses that retry with "bad write retry",
 * because by default it insists on the very same buffer pointer.
 *
 * The session id context is the one setting resumption depends on. A server
 * context with SSL_VERIFY_PEER, which is every context with a trust store,
 * refuses to resume a session when no session id context is set. OpenSSL
 * then fails the whole handshake with "session id context uninitialized"
 * instead of falling back to a full handshake, so every client that offers a
 * cached session (every browser, curl, and any proxy that reuses sessions)
 * would fail on its second connection. That failure is far too common to
 * leave to chance, which is why no context of this module goes without
 * one. */
static bool _ctls_apply_base_settings(SSL_CTX *ctx) {
  if (!ctx) return false;
  SSL_CTX_set_mode(
      ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
  return SSL_CTX_set_session_id_context(
             ctx, _ctls_session_id_context,
             (unsigned int)(sizeof(_ctls_session_id_context) - 1)) == 1;
}

static bool _ctls_apply_cert(SSL_CTX *ctx, const char *cert_pem,
                             size_t cert_len, const char *key_pem,
                             size_t key_len, const char *pk_password,
                             bool self_signed, const char *self_signed_name) {
  if (self_signed) {
    X509 *cert = _ctls_create_self_signed(self_signed_name);
    if (!cert) return false;
    EVP_PKEY *root_key = _ctls_get_root_key();
    bool ok = root_key && SSL_CTX_use_certificate(ctx, cert) == 1 &&
              SSL_CTX_use_PrivateKey(ctx, root_key) == 1;
    EVP_PKEY_free(root_key);
    X509_free(cert);
    return ok && SSL_CTX_check_private_key(ctx) == 1;
  }
  if (!cert_pem || !key_pem) return true; /* no cert configured; not an error */

  ctls_pw_ctx pw = {.password = pk_password};
  bool key_ok = false;
  BIO *kbio = BIO_new_mem_buf(key_pem, (int)key_len);
  if (kbio) {
    EVP_PKEY *pkey =
        PEM_read_bio_PrivateKey(kbio, NULL, _ctls_pem_passwd_cb, &pw);
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
            /* An intermediate certificate that fails to attach would quietly
             * give every client an incomplete chain, so any failure here
             * fails applying the whole certificate. */
            cert_ok = false;
          }
        }
      }
      sk_X509_INFO_pop_free(inf, X509_INFO_free);
    }
    BIO_free(cbio);
  }
  /* Neither install call answers "do these two belong together" by itself.
   * SSL_CTX_use_PrivateKey can only cross-check against a certificate that
   * is already installed, and SSL_CTX_use_certificate answers a mismatch in
   * another way, by throwing away the installed key, clearing the error
   * queue and reporting success. Without this check we would build and
   * commit a context carrying a pair that doesn't match, and every handshake
   * it served would fail with no certificate and no diagnostic.
   * ctls_ctx_cert_add already rejects such a pair when it accepts the
   * input, so no current path gets here with one; this check keeps any built
   * SSL_CTX free of such a pair however the pair arrives. */
  return cert_ok && SSL_CTX_check_private_key(ctx) == 1;
}

/* Installs the trust store and the verify mode on one SSL_CTX. Every SSL_CTX
 * built for one ctls_ctx_t, the default one and each named certificate's,
 * goes through here, so an SNI dispatch never moves a connection onto a
 * context with a weaker mode.
 *
 * SSL_VERIFY_FAIL_IF_NO_PEER_CERT makes a server refuse a client that sends
 * no certificate, whereas plain SSL_VERIFY_PEER only asks for one and lets a
 * client that sends nothing through, since there's nothing to verify.
 * OpenSSL ignores the flag in client mode, where a server always presents a
 * certificate that the chain check then verifies.
 *
 * A certificate revocation list in a trusted bundle is enforced. When any
 * bundle holds one, the store gets X509_V_FLAG_CRL_CHECK and
 * X509_V_FLAG_CRL_CHECK_ALL, like nginx's ssl_crl directive does: every
 * certificate of the peer's chain, its trust anchor included, must then have
 * a CRL of its issuer in the store, a revoked certificate fails the
 * handshake, and so does a chain left unchecked by a missing or expired CRL.
 *
 * Every bundle read here was accepted by ctls_ctx_trust, so a parse that
 * gives nothing, or an add that fails, is an allocation failure, and it
 * fails the whole build. A context built anyway would verify against a store
 * missing some of the anchors and CRLs the caller configured, and a lost CRL
 * would switch revocation checking off. */
static bool _ctls_apply_trust(SSL_CTX *ctx, ctls_ctx_t *tls) {
  if (tls->trust_count == 0 && !tls->verify_default_store) return true;
  X509_STORE *store = X509_STORE_new();
  if (!store) return false;
  SSL_CTX_set_cert_store(ctx, store);
  SSL_CTX_set_verify(ctx,
                     tls->peer_cert_optional
                         ? SSL_VERIFY_PEER
                         : SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                     NULL);
  if (tls->verify_default_store && !_ctls_store_add_system_trust(store))
    return false;
  bool any_crl = false;
  for (size_t i = 0; i < tls->trust_count; ++i) {
    BIO *bio = BIO_new_mem_buf(tls->trust_pems[i], (int)tls->trust_lens[i]);
    if (!bio) return false;
    STACK_OF(X509_INFO) *inf = PEM_X509_INFO_read_bio(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (!inf) return false;
    for (int j = 0; j < sk_X509_INFO_num(inf); ++j)
      if (sk_X509_INFO_value(inf, j)->crl) any_crl = true;
    bool added = _ctls_store_add_infos(store, inf);
    sk_X509_INFO_pop_free(inf, X509_INFO_free);
    if (!added) return false;
  }
  if (any_crl &&
      X509_STORE_set_flags(
          store, X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL) != 1)
    return false;
  return true;
}

/* The server-mode ALPN selection callback. It runs inside the SSL_accept()
 * call of ctls_conn_handshake_step() and only records the selection.
 * on_selected fires from _ctls_record_alpn once SSL_accept() has returned,
 * because OpenSSL reads *out, which points into conn, after this callback
 * returns, and an on_selected that destroyed its own connection here would
 * leave OpenSSL reading freed memory. */
static int _ctls_alpn_select_cb(SSL *ssl, const unsigned char **out,
                                unsigned char *outlen, const unsigned char *in,
                                unsigned int inlen, void *arg) {
  ctls_ctx_t *tls = (ctls_ctx_t *)arg;
  ctls_conn_t *conn = (ctls_conn_t *)SSL_get_ex_data(ssl, _ctls_conn_ex_idx());
  /* In practice conn is never NULL: this callback is only installed on a
   * ctx we built ourselves, and ctls_conn_create_server() always sets this
   * ex_data before any handshake that can get here. The NULL check below is
   * a defence because without conn there's no safe place to copy a selected
   * protocol name to (see below), not because some real code path leaves
   * conn unset. */
  if (!conn) return SSL_TLSEXT_ERR_NOACK;

  /* tls->lock guards the read of tls->alpn[] and tls->alpn_count below, and
   * _ctls_record_alpn, which runs after the handshake, locks around its own
   * read of the same fields for the same reason: tls->alpn is mutable
   * storage. A concurrent ctls_ctx_alpn_add() can _ccol_mem_free() it when
   * it replaces an existing entry or _ccol_mem_realloc() it when it grows
   * the array, and either can happen under a handshake in progress. That's a
   * documented, supported use case and not a hypothetical one; see the doc
   * comment of ctls_ctx_retain().
   *
   * We copy the matched entry's name bytes into conn->alpn_selected_buf
   * before unlocking, because a pointer to those bytes isn't enough for the
   * two readers that depend on them. OpenSSL reads *out from inside this
   * same SSL_accept() call after this callback returns, and a caller can
   * read conn->alpn_selected_name at any later point in conn's life, long
   * after this handshake and after any number of further
   * ctls_ctx_alpn_add() calls. The copies keep both readers away from memory
   * that tls->lock has stopped protecting. */
  _ccol_mutex_lock(tls->lock);
  if (tls->alpn_count == 0) {
    _ccol_mutex_unlock(tls->lock);
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
    /* This ctx holds none of the protocols the peer offered, so the two sides
     * agree on no protocol: the negotiation on the wire reports NOACK,
     * nothing is recorded on conn and no on_selected callback fires.
     * Reporting a registered protocol name here would tell a caller the two
     * sides settled on a protocol the peer never acknowledged, and a caller
     * that picks its wire framing from that report would end up speaking a
     * protocol the peer doesn't speak. */
    _ccol_mutex_unlock(tls->lock);
    return SSL_TLSEXT_ERR_NOACK;
  }

  memcpy(conn->alpn_selected_buf, matched->name, matched->name_len);
  conn->alpn_selected_buf[matched->name_len] = '\0';
  conn->alpn_selected_name = conn->alpn_selected_buf;
  conn->alpn_selected_len = matched->name_len;
  _ccol_mutex_unlock(tls->lock);

  *out = (const unsigned char *)conn->alpn_selected_buf;
  *outlen = (unsigned char)conn->alpn_selected_len;
  return SSL_TLSEXT_ERR_OK;
}

/* Sets *oom when its own allocation fails, since that failure leaves no
 * reason on the OpenSSL error queue for the caller to read. */
static bool _ctls_apply_alpn(SSL_CTX *ctx, ctls_ctx_t *tls, bool *oom) {
  if (tls->alpn_count == 0) return true;
  size_t wire_len = 0;
  for (size_t i = 0; i < tls->alpn_count; ++i)
    wire_len += tls->alpn[i].name_len + 1;
  unsigned char *wire =
      (unsigned char *)_ccol_mem_alloc(tls->m_procs, wire_len);
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
  /* SSL_CTX_set_alpn_protos builds the client-mode offer list and copies its
   * input internally, so wire doesn't have to outlive this call. */
  /* It returns 0 on success, and the only way it can fail here is an
   * allocation failure, since every stored name is a valid protocol name. */
  int failed = SSL_CTX_set_alpn_protos(ctx, wire, (unsigned int)wire_len);
  _ccol_mem_free(tls->m_procs, wire);
  if (failed != 0) {
    *oom = true;
    return false;
  }
  SSL_CTX_set_alpn_select_cb(ctx, _ctls_alpn_select_cb, tls);
  return true;
}

/* Looks up one registered pattern by its exact spelling, which the caller
 * passes in lower case. Returns that entry's built SSL_CTX, or NULL if the
 * pattern isn't registered. */
static SSL_CTX *_ctls_named_ctx_for_pattern(ctls_ctx_t *tls, char *pattern) {
  cmap_pair kp = {.ptr = pattern, .size = strlen(pattern) + 1};
  cmap_pair vp;
  if (chmap_get_elem_ref(tls->named_certs, &kp, &vp) != ccol_success)
    return NULL;
  ctls_named_cert *nc;
  memcpy(&nc, vp.ptr, sizeof(nc));
  return nc->built_ctx;
}

/* Resolves a ClientHello's SNI name to the SSL_CTX of a named certificate.
 * An exact match is tried first, and a one-label "*." wildcard only when no
 * exact entry exists. That precedence makes the answer a property of the
 * name; without it the answer would depend on the order of the
 * ctls_ctx_cert_add calls. "foo.example.com" and "*.example.com" both
 * describe a request for "foo.example.com", and without a rank between them
 * a host asking for a specific name could get the wildcard certificate,
 * throwing away everything that makes the specific one special: its own
 * key, its own chain, and a leaf that a client pins. An operator who adds a
 * specific certificate next to a wildcard means it to win, so it always
 * does.
 *
 * Each half is a single direct probe, not a scan. At most one registered
 * pattern can match a given name as a wildcard, because the "*." form
 * covers exactly one label, so the only candidate is "*." plus everything
 * after the name's first dot, and there's nothing to rank among wildcards
 * either. */
static SSL_CTX *_ctls_find_named_ctx(ctls_ctx_t *tls, const char *sni_name) {
  if (!tls->named_certs || !sni_name || !*sni_name) return NULL;
  char *lower = _ctls_strdup_lower(tls->m_procs, sni_name);
  if (!lower) return NULL;
  SSL_CTX *found = _ctls_named_ctx_for_pattern(tls, lower);
  if (!found) {
    char *dot = strchr(lower, '.');
    /* The two bytes right before the suffix are the last character of the
     * name's first label and the dot itself, which the exact probe above has
     * already used, so we write the wildcard pattern over them in place
     * without a second allocation. A name whose first label is empty has no
     * wildcard form to build, and neither does a name that ends in its only
     * dot. */
    if (dot && dot > lower && dot[1] != '\0') {
      char *pattern = dot - 1;
      pattern[0] = '*';
      pattern[1] = '.';
      found = _ctls_named_ctx_for_pattern(tls, pattern);
    }
  }
  _ccol_mem_free(tls->m_procs, lower);
  return found;
}

/* Records our reference on the SSL_CTX a servername dispatch installed on
 * conn, replacing a reference an earlier dispatch on the same connection
 * left behind: OpenSSL runs the servername callback once per ClientHello,
 * so a TLS 1.3 HelloRetryRequest exchange gets here twice. The caller holds
 * ctx->lock, and this call is in the same critical section as the lookup and
 * the SSL_set_SSL_CTX() that produced installed, so nothing can rebuild that
 * object away in between.
 *
 * Without this pin the only thing keeping a dispatched named context alive
 * would be the reference SSL_set_SSL_CTX() takes internally, which is a
 * third party's bookkeeping and not ours. The property that matters is that
 * every context one of our live connections can still reach is a context
 * this module holds a reference on, and without the pin we could neither
 * state that property nor check it. */
static void _ctls_conn_pin_sni_ctx(ctls_conn_t *conn, SSL_CTX *installed) {
  SSL_CTX_up_ref(installed);
  if (conn->pinned_sni_ctx) {
    SSL_CTX_free(conn->pinned_sni_ctx);
    _ctls_account_ctx_pins(-1);
  }
  conn->pinned_sni_ctx = installed;
  _ctls_account_ctx_pins(1);
}

static int _ctls_servername_cb(SSL *ssl, int *ad, void *arg) {
  (void)ad;
  ctls_ctx_t *tls = (ctls_ctx_t *)arg;
  /* We resolve conn before taking tls->lock, in the same order as
   * _ctls_alpn_select_cb. _ctls_conn_ex_idx() may take a mutex of its own,
   * and we never call it while holding tls->lock, which keeps the two locks
   * independent of each other. */
  ctls_conn_t *conn = (ctls_conn_t *)SSL_get_ex_data(ssl, _ctls_conn_ex_idx());
  const char *name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
  if (!name) return SSL_TLSEXT_ERR_OK;
  /* SSL_set_SSL_CTX() must run in the SAME critical section as the lookup
   * that produced matched, not after tls->lock is released, because matched
   * is a raw SSL_CTX* into a named certificate's built_ctx field. The doc
   * comment of ctls_ctx_retain() says that pinning a context against a
   * concurrent configuration change from another thread is an explicitly
   * supported use case; such a change comes from ctls_ctx_cert_add(),
   * ctls_ctx_trust() and the other functions that change a context, a
   * certificate rotation under live traffic for example, and it isn't a
   * hypothetical one. Rotating a named certificate frees that name's OLD
   * built_ctx once the rebuild has succeeded, and any rebuild frees every
   * context it replaces, always while holding tls->lock, so unlocking here
   * before using matched lets that free happen between the lookup and the
   * use. SSL_set_SSL_CTX() would then get a dangling pointer, a real
   * use-after-free on a live handshake and not a theoretical one.
   * ctls_conn_create_client and ctls_conn_create_server follow the same
   * discipline and call SSL_new() on the default context while holding
   * tls->lock, for the same reason. */
  _ccol_mutex_lock(tls->lock);
  SSL_CTX *matched = _ctls_find_named_ctx(tls, name);
  /* SSL_set_SSL_CTX() reports failure by returning NULL, since it duplicates
   * the new context's certificate and that allocation can fail. After a
   * failure the connection stays on the context it already had, so pinning
   * matched then would hold an object the connection doesn't use. After a
   * success the call returns the context it installed. */
  if (matched && SSL_set_SSL_CTX(ssl, matched) && conn)
    _ctls_conn_pin_sni_ctx(conn, matched);
  _ccol_mutex_unlock(tls->lock);
  return SSL_TLSEXT_ERR_OK;
}

/* Builds one SSL_CTX from scratch with the base settings, an optional
 * certificate, the shared trust store and the shared ALPN configuration.
 * It's used for ctx_default and for the SSL_CTX of every SNI-dispatched
 * named certificate, and each of those contexts is fully independent and
 * fully configured, so SSL_set_SSL_CTX() can move onto it as a whole from
 * the servername callback. Returns NULL on failure, so the caller must
 * keep the ctx_t's previous, valid SSL_CTX pointer until this succeeds. Sets
 * *oom when one of this module's allocations fails. */
static SSL_CTX *_ctls_build_one_ctx(ctls_ctx_t *tls, const char *cert_pem,
                                    size_t cert_len, const char *key_pem,
                                    size_t key_len, const char *pk_password,
                                    bool self_signed,
                                    const char *self_signed_name, bool *oom) {
  SSL_CTX *ctx = SSL_CTX_new(TLS_method());
  if (!ctx) return NULL;
  if (!_ctls_apply_base_settings(ctx) ||
      !_ctls_apply_cert(ctx, cert_pem, cert_len, key_pem, key_len, pk_password,
                        self_signed, self_signed_name) ||
      !_ctls_apply_trust(ctx, tls) || !_ctls_apply_alpn(ctx, tls, oom)) {
    SSL_CTX_free(ctx);
    return NULL;
  }
  return ctx;
}

/* Gives the result of a failed rebuild and empties the thread's error queue:
 * ccol_not_enough_memory when oom is set or the queue holds
 * ERR_R_MALLOC_FAILURE, which OpenSSL records for every failed allocation of
 * its own, and ccol_http_tls_cert_load_failed otherwise. */
static ccol_retval_t _ctls_rebuild_failure(bool oom) {
  unsigned long e;
  while ((e = ERR_get_error()) != 0) {
    if (ERR_GET_REASON(e) == ERR_R_MALLOC_FAILURE) oom = true;
  }
  return oom ? ccol_not_enough_memory : ccol_http_tls_cert_load_failed;
}

/* Rebuilds ctx_default and the SSL_CTX of every named certificate from the
 * configuration the context stores at the time of the call. It's called
 * under tls->lock after every call that changes the context: cert_add,
 * trust, trust_system and alpn_add. It always rebuilds from scratch and
 * never patches the existing SSL_CTX objects in place. That's more work per
 * change than a patch would be, and it's worth it, because a fresh build is
 * what lets a failure leave the old objects untouched. On failure the
 * previous, valid SSL_CTX objects stay in place and nothing is torn down, so
 * a failed configuration change never leaves ctx in a worse, unusable state
 * than before the call. A caller that changed the stored configuration
 * before this call must restore it on failure, because the built contexts
 * still serve the previous configuration, and otherwise the next successful
 * rebuild would commit the change this call refused.
 *
 * Returns ccol_success, ccol_not_enough_memory when an allocation failed
 * (ours, or one of OpenSSL's, which leaves ERR_R_MALLOC_FAILURE on the error
 * queue), or ccol_http_tls_cert_load_failed for any other failure. The
 * thread's error queue is empty on return. */
static ccol_retval_t _ctls_ctx_rebuild_locked(ctls_ctx_t *tls) {
  /* Only this rebuild's errors may decide how a failure is reported. */
  ERR_clear_error();
  bool oom = false;
  SSL_CTX *new_default = _ctls_build_one_ctx(
      tls, tls->has_default_cert ? tls->default_cert_pem : NULL,
      tls->default_cert_len,
      tls->has_default_cert ? tls->default_key_pem : NULL, tls->default_key_len,
      tls->default_pk_password, tls->default_self_signed,
      tls->default_self_signed_name, &oom);
  if (!new_default) return _ctls_rebuild_failure(oom);

  if (tls->named_certs && chmap_elem_count(tls->named_certs) > 0) {
    SSL_CTX_set_tlsext_servername_callback(new_default, _ctls_servername_cb);
    SSL_CTX_set_tlsext_servername_arg(new_default, tls);
  }

  /* Every named certificate's SSL_CTX is rebuilt before new_default is
   * committed, so a failure halfway leaves everything consistent, including
   * the entries whose new SSL_CTX was already built: either every context
   * rebuilds successfully and they're all swapped in together, or none of
   * them is swapped in. */
  size_t named_count =
      tls->named_certs ? chmap_elem_count(tls->named_certs) : 0;
  SSL_CTX **new_named_ctxs = NULL;
  ctls_named_cert **new_named_entries = NULL;
  if (named_count > 0) {
    new_named_ctxs = (SSL_CTX **)_ccol_mem_alloc(
        tls->m_procs, named_count * sizeof(SSL_CTX *));
    new_named_entries = (ctls_named_cert **)_ccol_mem_alloc(
        tls->m_procs, named_count * sizeof(ctls_named_cert *));
    if (!new_named_ctxs || !new_named_entries) {
      _ccol_mem_free(tls->m_procs, new_named_ctxs);
      _ccol_mem_free(tls->m_procs, new_named_entries);
      SSL_CTX_free(new_default);
      return _ctls_rebuild_failure(true);
    }
    /* The walk allocates nothing so it can't fail, and with named_count
     * above 0 and ctx->lock held it gives exactly named_count entries. */
    ccol_chmap_walk walk = {0};
    const void *key_bytes;
    const void *val_bytes;
    size_t i = 0;
    bool ok = true;
    while (i < named_count && ccol_chmap_walk_next(tls->named_certs, &walk,
                                                   &key_bytes, &val_bytes)) {
      ctls_named_cert *nc;
      memcpy(&nc, val_bytes, sizeof(nc));
      SSL_CTX *built = _ctls_build_one_ctx(
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
      _ccol_mem_free(tls->m_procs, new_named_ctxs);
      _ccol_mem_free(tls->m_procs, new_named_entries);
      SSL_CTX_free(new_default);
      return _ctls_rebuild_failure(oom);
    }
  }

  /* Commit: free the old contexts and install the new ones. */
  if (tls->ctx_default) SSL_CTX_free(tls->ctx_default);
  tls->ctx_default = new_default;
  for (size_t i = 0; i < named_count; ++i) {
    if (new_named_entries[i]->built_ctx)
      SSL_CTX_free(new_named_entries[i]->built_ctx);
    new_named_entries[i]->built_ctx = new_named_ctxs[i];
  }
  _ccol_mem_free(tls->m_procs, new_named_ctxs);
  _ccol_mem_free(tls->m_procs, new_named_entries);
  /* A build can succeed with errors on the queue that some step pushed and
   * then recovered from, and they must not leak to a later reader of the
   * queue. */
  ERR_clear_error();
  return ccol_success;
}

/* ========================================================================== */
/*                       ctls_ctx_t CONSTRUCTION / MUTATION                   */
/* ========================================================================== */

static void _ctls_named_cert_destroy(ctls_ctx_t *tls, ctls_named_cert *nc) {
  if (!nc) return;
  _ccol_mem_free(tls->m_procs, nc->cert_pem);
  _ccol_mem_free(tls->m_procs, nc->key_pem);
  _ccol_mem_free(tls->m_procs, nc->pk_password);
  _ccol_mem_free(tls->m_procs, nc->self_signed_name);
  if (nc->built_ctx) SSL_CTX_free(nc->built_ctx);
  _ccol_mem_free(tls->m_procs, nc);
}

/* Reports whether a PEM certificate and a PEM private key belong together.
 * The pair is only usable if the key matches the public key of the leaf
 * certificate that _ctls_apply_cert installs later, which is the first entry
 * of the certificate stream; the other entries are chain certificates and
 * have no private key of their own.
 *
 * The check is made when the input is accepted, before the pair reaches the
 * stored configuration, and not in the rebuild step, because a pair that's
 * committed and only then found bad stays registered, and every later
 * rebuild this context runs for some unrelated reason (another certificate,
 * another trust bundle or another ALPN protocol) would fail on it again. */
static bool _ctls_cert_key_pair_matches(const char *cert_pem, size_t cert_len,
                                        const char *key_pem, size_t key_len,
                                        const char *pk_password) {
  ctls_pw_ctx pw = {.password = pk_password};
  BIO *kbio = BIO_new_mem_buf(key_pem, (int)key_len);
  if (!kbio) return false;
  EVP_PKEY *pkey =
      PEM_read_bio_PrivateKey(kbio, NULL, _ctls_pem_passwd_cb, &pw);
  BIO_free(kbio);
  if (!pkey) {
    /* A key we can't read (malformed PEM, or an encrypted key with a wrong
     * password or no password) leaves its reason on the thread's error
     * queue, so we clear the queue. Otherwise a later SSL_get_error
     * classification of some unrelated connection would report this
     * configuration failure. */
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

ctls_ctx_t *ctls_ctx_new_mp(ccol_memmgmt_procs_t *mp, char **err_str) {
  ctls_ctx_t *tls = (ctls_ctx_t *)_ccol_mem_calloc(mp, 1, sizeof(*tls));
  if (!tls) {
    if (err_str) *err_str = "ctls_ctx_new: allocation failure";
    return NULL;
  }
  if (mp) {
    tls->m_procs = (ccol_memmgmt_procs_t *)_ccol_mem_alloc(mp, sizeof(*mp));
    if (!tls->m_procs) {
      _ccol_mem_free(mp, tls);
      if (err_str) *err_str = "ctls_ctx_new: allocation failure";
      return NULL;
    }
    *tls->m_procs = *mp;
  }
  tls->ref = 1;
  if (_ccol_mutex_init(tls->lock) != 0) {
    _ccol_mem_free(mp, tls->m_procs);
    _ccol_mem_free(mp, tls);
    if (err_str) *err_str = "ctls_ctx_new: failed to initialize mutex";
    return NULL;
  }
  char *err = NULL;
  tls->named_certs =
      chmap_create_mp(16, ccol_string, ccol_pointer, tls->m_procs, &err);
  if (!tls->named_certs) {
    _ccol_mutex_destroy(tls->lock);
    _ccol_mem_free(mp, tls->m_procs);
    _ccol_mem_free(mp, tls);
    if (err_str) *err_str = "ctls_ctx_new: chmap allocation failure";
    return NULL;
  }
  if (_ctls_ctx_rebuild_locked(tls) != ccol_success) {
    __chmap_destroy(tls->named_certs);
    _ccol_mutex_destroy(tls->lock);
    _ccol_mem_free(mp, tls->m_procs);
    _ccol_mem_free(mp, tls);
    if (err_str) *err_str = "ctls_ctx_new: SSL_CTX_new failure";
    return NULL;
  }
  return tls;
}

ccol_retval_t ctls_ctx_cert_add(ctls_ctx_t *ctx, const char *server_name,
                                const char *cert_path, const char *key_path,
                                const char *pk_password, char **err_str) {
  if (!ctx) return ccol_invalid_args;
  bool have_name = server_name && *server_name;
  bool have_pair = cert_path && key_path;
  /* Neither file is needed when a self-signed certificate is generated,
   * which happens in two cases: a named self-signed certificate dispatched by
   * SNI, where server_name gives the subject CN, and a self-signed DEFAULT
   * certificate, where server_name is NULL or "" and the code below falls
   * back to a generic subject name, since a self-signed certificate always
   * needs a subject and the default slot has no name of its own to take one
   * from. */
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
    if (!_ctls_read_file(ctx->m_procs, cert_path, &cert_pem, &cert_len, &why) ||
        !_ctls_read_file(ctx->m_procs, key_path, &key_pem, &key_len, &why)) {
      _ccol_mem_free(ctx->m_procs, cert_pem);
      _ccol_mem_free(ctx->m_procs, key_pem);
      if (err_str) {
        *err_str = why ? why : "ctls_ctx_cert_add: cert/key file unreadable";
      }
      return ccol_http_tls_cert_load_failed;
    }
    /* A certificate and a key that don't belong together are a load failure,
     * not a usable configuration. We reject the pair here before storing
     * anything, so ctx keeps serving what it served before this call.
     * Without this check the pair would be accepted, the rebuild would
     * commit it, and every handshake after that would fail because there's
     * no certificate to present; for a named SNI entry only that one hostname
     * would break while the context looks healthy. Updating one of the two
     * paths without the other is the ordinary way a rotation goes wrong. */
    if (!_ctls_cert_key_pair_matches(cert_pem, cert_len, key_pem, key_len,
                                     pk_password)) {
      _ccol_mem_free(ctx->m_procs, cert_pem);
      _ccol_mem_free(ctx->m_procs, key_pem);
      if (err_str) {
        *err_str =
            "ctls_ctx_cert_add: certificate and private key do not match, or "
            "either one is unreadable";
      }
      return ccol_http_tls_cert_load_failed;
    }
    /* A matching pair can still be one OpenSSL refuses to serve, like a key
     * below the security level or a SHA-1 signature. The probe installs it
     * into a throwaway SSL_CTX with the base settings before anything is
     * stored; without it the refused pair would be stored, the rebuild would
     * fail, and every later change to this context would fail on it. */
    SSL_CTX *probe = SSL_CTX_new(TLS_method());
    bool usable = probe && _ctls_apply_base_settings(probe) &&
                  _ctls_apply_cert(probe, cert_pem, cert_len, key_pem, key_len,
                                   pk_password, false, NULL);
    if (probe) SSL_CTX_free(probe);
    ERR_clear_error();
    if (!usable) {
      _ccol_mem_free(ctx->m_procs, cert_pem);
      _ccol_mem_free(ctx->m_procs, key_pem);
      if (err_str) {
        *err_str =
            "ctls_ctx_cert_add: the TLS library refuses this certificate "
            "(for example a key below its security level, or a weak "
            "signature digest)";
      }
      return ccol_http_tls_cert_load_failed;
    }
    if (pk_password) {
      pw_copy = _ctls_strdup(ctx->m_procs, pk_password);
      if (!pw_copy) {
        _ccol_mem_free(ctx->m_procs, cert_pem);
        _ccol_mem_free(ctx->m_procs, key_pem);
        return ccol_not_enough_memory;
      }
    }
  }

  _ccol_mutex_lock(ctx->lock);
  ccol_retval_t rv = ccol_success;
  if (!have_name) {
    /* Replace the default certificate. */
    char *self_signed_name = NULL;
    if (self_signed) {
      /* A generic fixed subject name is used when the caller gives none,
       * because the default slot has no name of its own, unlike a named
       * entry, whose server_name also becomes the self-signed certificate's
       * subject. That name is good enough for the common case of a server
       * running with no certificate configuration at all; a caller that
       * relies on hostname verification against this certificate has to
       * configure a real name anyway. */
      self_signed_name = _ctls_strdup(ctx->m_procs, "ctls-default");
      if (!self_signed_name) {
        _ccol_mem_free(ctx->m_procs, cert_pem);
        _ccol_mem_free(ctx->m_procs, key_pem);
        _ccol_mem_free(ctx->m_procs, pw_copy);
        _ccol_mutex_unlock(ctx->lock);
        return ccol_not_enough_memory;
      }
    }
    /* The new fields go in and the rebuild runs, and only a successful
     * rebuild frees the previous fields; a failed one puts them back, so the
     * stored configuration keeps matching the contexts that are serving. */
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
    rv = _ctls_ctx_rebuild_locked(ctx);
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
    _ccol_mem_free(ctx->m_procs, prev_cert_pem);
    _ccol_mem_free(ctx->m_procs, prev_key_pem);
    _ccol_mem_free(ctx->m_procs, prev_pw);
    _ccol_mem_free(ctx->m_procs, prev_self_signed_name);
  } else {
    char *lower_name = _ctls_strdup_lower(ctx->m_procs, server_name);
    if (!lower_name) {
      _ccol_mem_free(ctx->m_procs, cert_pem);
      _ccol_mem_free(ctx->m_procs, key_pem);
      _ccol_mem_free(ctx->m_procs, pw_copy);
      _ccol_mutex_unlock(ctx->lock);
      return ccol_not_enough_memory;
    }
    ctls_named_cert *nc =
        (ctls_named_cert *)_ccol_mem_calloc(ctx->m_procs, 1, sizeof(*nc));
    if (!nc) {
      _ccol_mem_free(ctx->m_procs, lower_name);
      _ccol_mem_free(ctx->m_procs, cert_pem);
      _ccol_mem_free(ctx->m_procs, key_pem);
      _ccol_mem_free(ctx->m_procs, pw_copy);
      _ccol_mutex_unlock(ctx->lock);
      return ccol_not_enough_memory;
    }
    nc->cert_pem = cert_pem;
    nc->cert_len = cert_len;
    nc->key_pem = key_pem;
    nc->key_len = key_len;
    nc->pk_password = pw_copy;
    nc->self_signed = self_signed;
    if (self_signed) {
      nc->self_signed_name = _ctls_strdup(ctx->m_procs, lower_name);
      if (!nc->self_signed_name) {
        /* This failure needs checking like every other allocation in this
         * function. Without the check nc->self_signed_name would stay NULL
         * while nc->self_signed stays true, and nothing below tests
         * self_signed_name for NULL before using it: _ctls_ctx_rebuild_locked
         * calls _ctls_build_one_ctx, which calls
         * _ctls_create_self_signed(NULL), which always calls
         * strlen(server_name), so we'd get a real crash under sustained
         * memory pressure. The documented, graceful answer is
         * ccol_not_enough_memory, which every other allocation failure here
         * already reports. At this point nothing can reach nc from
         * ctx->named_certs, since the map insert happens further down, so
         * _ctls_named_cert_destroy(ctx, nc) is a clean and complete unwind,
         * and it also frees cert_pem, key_pem and pw_copy, which nc owns from
         * the assignments above. */
        _ctls_named_cert_destroy(ctx, nc);
        _ccol_mem_free(ctx->m_procs, lower_name);
        _ccol_mutex_unlock(ctx->lock);
        return ccol_not_enough_memory;
      }
    }

    cmap_pair kp = {.ptr = lower_name, .size = strlen(lower_name) + 1};
    cmap_pair old_vp;
    ctls_named_cert *old_nc = NULL;
    /* Only the pointer VALUE is taken here, because old_vp.ptr points into
     * the map's storage and the insert below may move that storage. */
    if (chmap_get_elem_ref(ctx->named_certs, &kp, &old_vp) == ccol_success)
      memcpy(&old_nc, old_vp.ptr, sizeof(old_nc));
    cmap_pair vp = {.ptr = &nc, .size = sizeof(nc)};
    rv = chmap_insert_elem(ctx->named_certs, &kp, &vp);
    /* ccol_key_already_present means the map updated this hostname's value
     * slot to point at nc because the entry already existed, for example
     * because this call rotates the certificate of a name an earlier call
     * added. That isn't a failure, and nc must not be destroyed here, since
     * that would leave the map with a dangling pointer to it. */
    if (rv != ccol_success && rv != ccol_key_already_present) {
      _ccol_mem_free(ctx->m_procs, lower_name);
      _ctls_named_cert_destroy(ctx, nc);
      _ccol_mutex_unlock(ctx->lock);
      return rv;
    }
    /* The entry the map held before this call stays alive, with its built
     * context, until the rebuild succeeds. A failed rebuild puts it back in
     * the map (or removes a name this call added), so the name keeps the
     * certificate it served before the call. Updating the value of a key
     * that's present allocates nothing for a pointer-sized value and a
     * delete allocates nothing, so neither step of the rollback can fail.
     * Only after a successful rebuild is the replaced entry dropped; its
     * context is out of every map and every built configuration by then,
     * and a connection still using it holds its own reference. */
    rv = _ctls_ctx_rebuild_locked(ctx);
    if (rv != ccol_success) {
      if (old_nc) {
        cmap_pair old_vp_pair = {.ptr = &old_nc, .size = sizeof(old_nc)};
        ccol_retval_t back =
            chmap_insert_elem(ctx->named_certs, &kp, &old_vp_pair);
        _ccol_assert(back == ccol_key_already_present);
        (void)back;
      } else {
        ccol_retval_t gone = chmap_delete_elem(ctx->named_certs, &kp);
        _ccol_assert(gone == ccol_success);
        (void)gone;
      }
      _ctls_named_cert_destroy(ctx, nc);
    } else if (old_nc) {
      _ctls_named_cert_destroy(ctx, old_nc);
    }
    _ccol_mem_free(ctx->m_procs, lower_name);
  }
  _ccol_mutex_unlock(ctx->lock);
  if (rv != ccol_success && err_str)
    *err_str = "ctls_ctx_cert_add: SSL_CTX rebuild failure";
  return rv;
}

/* Reports whether a PEM stream holds at least one certificate. Some streams
 * hold none (an empty file, a file with just a private key, or a text file that
 * isn't PEM at all), and OpenSSL parses such a stream into an empty set
 * without reporting an error, so we check what an operator's bundle
 * contains, not only whether it parses. A bundle that contributes nothing
 * still switches peer verification on, and leaves a store that trusts no
 * issuer. A client then rejects every peer it talks to, while a server
 * keeps accepting every client that presents no certificate at all, because
 * its peer verification asks for a certificate but doesn't need one, so the
 * mutual TLS the operator configured the bundle for is silently missing.
 * A loud refusal when the bundle is given is much better than a server
 * that quietly runs without the protection it was set up for. Only
 * certificates count here. A CRL carries no trust anchor, so a stream
 * of nothing but CRLs names no issuer a peer could chain to, and it's
 * refused like an empty one. A bundle holding both certificates and CRLs is
 * accepted and its CRLs are enforced; see _ctls_apply_trust. */
static bool _ctls_pem_has_certificate(const char *pem, size_t len) {
  BIO *bio = BIO_new_mem_buf(pem, (int)len);
  if (!bio) return false;
  STACK_OF(X509_INFO) *inf = PEM_X509_INFO_read_bio(bio, NULL, NULL, NULL);
  bool found = false;
  if (inf) {
    for (int i = 0; i < sk_X509_INFO_num(inf) && !found; ++i) {
      X509_INFO *entry = sk_X509_INFO_value(inf, i);
      if (entry && entry->x509) found = true;
    }
    sk_X509_INFO_pop_free(inf, X509_INFO_free);
  }
  BIO_free(bio);
  return found;
}

ccol_retval_t ctls_ctx_trust(ctls_ctx_t *ctx, const char *ca_bundle_path,
                             char **err_str) {
  if (!ctx || !ca_bundle_path) return ccol_invalid_args;
  char *pem = NULL;
  size_t len = 0;
  char *why = NULL;
  if (!_ctls_read_file(ctx->m_procs, ca_bundle_path, &pem, &len, &why)) {
    if (err_str) {
      *err_str = why ? why : "ctls_ctx_trust: CA bundle file unreadable";
    }
    return ccol_http_tls_cert_load_failed;
  }
  /* This check runs here, before the entry below is committed, and not in
   * the rebuild step, because a bundle that reaches ctx->trust_pems and only
   * then fails stays registered, and every later rebuild this context runs
   * for some unrelated reason (another certificate or another ALPN protocol)
   * would fail on that bundle again. */
  if (!_ctls_pem_has_certificate(pem, len)) {
    _ccol_mem_free(ctx->m_procs, pem);
    if (err_str) {
      *err_str = "ctls_ctx_trust: CA bundle contains no certificates";
    }
    return ccol_http_tls_cert_load_failed;
  }

  _ccol_mutex_lock(ctx->lock);
  if (ctx->trust_count == ctx->trust_cap) {
    size_t new_cap = ctx->trust_cap ? ctx->trust_cap * 2 : 4;
    /* The new pointer goes into ctx->trust_pems immediately, without waiting
     * for the second realloc below to be checked. A successful realloc can
     * move the block, freeing the old one, or extend the original
     * allocation, and either way the copy of that pointer in ctx->trust_pems
     * becomes invalid. Committing only once both reallocs succeeded would
     * leave ctx->trust_pems dangling every time this first call succeeds and
     * moves the block while the second one fails, a real use-after-free on
     * this ctx's next access to trust_pems; it would also leak new_pems
     * itself, which clang's static analyzer flags. */
    char **new_pems = (char **)_ccol_mem_realloc(ctx->m_procs, ctx->trust_pems,
                                                 new_cap * sizeof(char *));
    if (!new_pems) {
      _ccol_mem_free(ctx->m_procs, pem);
      _ccol_mutex_unlock(ctx->lock);
      return ccol_not_enough_memory;
    }
    ctx->trust_pems = new_pems;
    size_t *new_lens = (size_t *)_ccol_mem_realloc(
        ctx->m_procs, ctx->trust_lens, new_cap * sizeof(size_t));
    if (!new_lens) {
      _ccol_mem_free(ctx->m_procs, pem);
      _ccol_mutex_unlock(ctx->lock);
      return ccol_not_enough_memory;
    }
    ctx->trust_lens = new_lens;
    ctx->trust_cap = new_cap;
  }
  /* A failed rebuild takes the bundle and the verify flag back out, so the
   * stored configuration keeps matching the contexts that are serving, and a
   * later rebuild for some other reason doesn't switch on what this call
   * refused. The bigger arrays stay, since they only hold spare capacity. */
  bool prev_verify_peer = ctx->verify_peer;
  ctx->trust_pems[ctx->trust_count] = pem;
  ctx->trust_lens[ctx->trust_count] = len;
  ctx->trust_count++;
  ctx->verify_peer = true;
  ccol_retval_t rv = _ctls_ctx_rebuild_locked(ctx);
  if (rv != ccol_success) {
    ctx->trust_count--;
    ctx->verify_peer = prev_verify_peer;
    _ccol_mem_free(ctx->m_procs, pem);
  }
  _ccol_mutex_unlock(ctx->lock);
  if (rv != ccol_success && err_str)
    *err_str = "ctls_ctx_trust: SSL_CTX rebuild failure";
  return rv;
}

ccol_retval_t ctls_ctx_trust_system(ctls_ctx_t *ctx) {
  if (!ctx) return ccol_invalid_args;
  _ccol_mutex_lock(ctx->lock);
  bool prev_default_store = ctx->verify_default_store;
  bool prev_verify_peer = ctx->verify_peer;
  ctx->verify_default_store = true;
  ctx->verify_peer = true;
  ccol_retval_t rv = _ctls_ctx_rebuild_locked(ctx);
  if (rv != ccol_success) {
    ctx->verify_default_store = prev_default_store;
    ctx->verify_peer = prev_verify_peer;
  }
  _ccol_mutex_unlock(ctx->lock);
  return rv;
}

ccol_retval_t ctls_ctx_peer_cert_optional(ctls_ctx_t *ctx, bool optional) {
  if (!ctx) return ccol_invalid_args;
  _ccol_mutex_lock(ctx->lock);
  if (ctx->peer_cert_optional == optional) {
    _ccol_mutex_unlock(ctx->lock);
    return ccol_success;
  }
  bool previous = ctx->peer_cert_optional;
  ctx->peer_cert_optional = optional;
  /* A context without a trust store has no verify mode, so there's nothing
   * to rebuild yet; the first trust call builds with this flag. */
  ccol_retval_t rv = (ctx->trust_count == 0 && !ctx->verify_default_store)
                         ? ccol_success
                         : _ctls_ctx_rebuild_locked(ctx);
  if (rv != ccol_success) ctx->peer_cert_optional = previous;
  _ccol_mutex_unlock(ctx->lock);
  return rv;
}

ccol_retval_t ctls_ctx_alpn_add(ctls_ctx_t *ctx, const char *protocol_name,
                                ctls_alpn_selected_fn on_selected,
                                void *alpn_udata,
                                ctls_alpn_cleanup_fn on_cleanup,
                                char **err_str) {
  if (!ctx || !protocol_name) return ccol_invalid_args;
  size_t name_len = strlen(protocol_name);
  if (name_len == 0 || name_len > 255) {
    if (err_str)
      *err_str = "ctls_ctx_alpn_add: protocol name must be 1-255 bytes";
    return ccol_invalid_args;
  }
  char *name_copy = _ctls_strdup(ctx->m_procs, protocol_name);
  if (!name_copy) return ccol_not_enough_memory;

  _ccol_mutex_lock(ctx->lock);
  /* Replace an existing registration with the same name instead of adding a
   * duplicate entry. */
  for (size_t i = 0; i < ctx->alpn_count; ++i) {
    if (ctx->alpn[i].name_len == name_len &&
        memcmp(ctx->alpn[i].name, name_copy, name_len) == 0) {
      /* The replaced registration's cleanup runs after ctx->lock is released,
       * because a cleanup that calls back into this module on the same
       * context, ctls_ctx_retain() or ctls_ctx_release() for example, would
       * otherwise lock a mutex this thread already holds. */
      ctls_alpn_entry old = ctx->alpn[i];
      ctx->alpn[i] = (ctls_alpn_entry){.name = name_copy,
                                       .name_len = name_len,
                                       .on_selected = on_selected,
                                       .udata = alpn_udata,
                                       .on_cleanup = on_cleanup};
      ccol_retval_t rv = _ctls_ctx_rebuild_locked(ctx);
      if (rv != ccol_success) {
        /* A failed rebuild keeps the earlier registration, and the new one
         * was never stored, so its udata stays with the caller. */
        ctx->alpn[i] = old;
        _ccol_mutex_unlock(ctx->lock);
        _ccol_mem_free(ctx->m_procs, name_copy);
        if (err_str) *err_str = "ctls_ctx_alpn_add: SSL_CTX rebuild failure";
        return rv;
      }
      _ccol_mutex_unlock(ctx->lock);
      _ccol_mem_free(ctx->m_procs, old.name);
      if (old.on_cleanup) old.on_cleanup(old.udata);
      return ccol_success;
    }
  }
  if (ctx->alpn_count == ctx->alpn_cap) {
    size_t new_cap = ctx->alpn_cap ? ctx->alpn_cap * 2 : 4;
    ctls_alpn_entry *new_alpn = (ctls_alpn_entry *)_ccol_mem_realloc(
        ctx->m_procs, ctx->alpn, new_cap * sizeof(ctls_alpn_entry));
    if (!new_alpn) {
      _ccol_mem_free(ctx->m_procs, name_copy);
      _ccol_mutex_unlock(ctx->lock);
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
  ccol_retval_t rv = _ctls_ctx_rebuild_locked(ctx);
  /* A failed rebuild takes the registration back out; the bigger array
   * stays, since it only holds spare capacity. */
  if (rv != ccol_success) ctx->alpn_count--;
  _ccol_mutex_unlock(ctx->lock);
  if (rv != ccol_success) {
    _ccol_mem_free(ctx->m_procs, name_copy);
    if (err_str) *err_str = "ctls_ctx_alpn_add: SSL_CTX rebuild failure";
  }
  return rv;
}

size_t ctls_ctx_alpn_count(const ctls_ctx_t *ctx) {
  return ctx ? ctx->alpn_count : 0;
}

void ctls_ctx_retain(ctls_ctx_t *ctx) {
  if (!ctx) return;
  _ccol_mutex_lock(ctx->lock);
  ctx->ref++;
  _ccol_mutex_unlock(ctx->lock);
}

void ctls_ctx_release(ctls_ctx_t *ctx) {
  if (!ctx) return;
  _ccol_mutex_lock(ctx->lock);
  size_t remaining = --ctx->ref;
  _ccol_mutex_unlock(ctx->lock);
  if (remaining > 0) return;

  if (ctx->ctx_default) SSL_CTX_free(ctx->ctx_default);
  _ccol_mem_free(ctx->m_procs, ctx->default_cert_pem);
  _ccol_mem_free(ctx->m_procs, ctx->default_key_pem);
  _ccol_mem_free(ctx->m_procs, ctx->default_pk_password);
  _ccol_mem_free(ctx->m_procs, ctx->default_self_signed_name);

  if (ctx->named_certs) {
    /* The walk allocates nothing, so a release that has run out of memory
     * still frees every named certificate. */
    ccol_chmap_walk walk = {0};
    const void *key_bytes;
    const void *val_bytes;
    while (
        ccol_chmap_walk_next(ctx->named_certs, &walk, &key_bytes, &val_bytes)) {
      ctls_named_cert *nc;
      memcpy(&nc, val_bytes, sizeof(nc));
      _ctls_named_cert_destroy(ctx, nc);
    }
    __chmap_destroy(ctx->named_certs);
  }

  for (size_t i = 0; i < ctx->trust_count; ++i)
    _ccol_mem_free(ctx->m_procs, ctx->trust_pems[i]);
  _ccol_mem_free(ctx->m_procs, ctx->trust_pems);
  _ccol_mem_free(ctx->m_procs, ctx->trust_lens);

  for (size_t i = 0; i < ctx->alpn_count; ++i) {
    if (ctx->alpn[i].on_cleanup) ctx->alpn[i].on_cleanup(ctx->alpn[i].udata);
    _ccol_mem_free(ctx->m_procs, ctx->alpn[i].name);
  }
  _ccol_mem_free(ctx->m_procs, ctx->alpn);

  _ccol_mutex_destroy(ctx->lock);
  /* When ctx->m_procs isn't NULL it's a heap copy of the caller's procs,
   * freed through the free() function pointer inside it. So ctx must be
   * freed FIRST, while mp is still a live object we can dereference. Freeing
   * mp before ctx would leave mp dangling for the second _ccol_mem_free()
   * call, which would then read mp->free from already freed memory, and
   * valgrind reports a use-after-free. */
  ccol_memmgmt_procs_t *mp = ctx->m_procs;
  _ccol_mem_free(mp, ctx);
  _ccol_mem_free(mp, mp);
}

/* ========================================================================== */
/*                                CONNECTIONS                                 */
/* ========================================================================== */

/* Releases the connection's SSL* and every SSL_CTX reference the connection
 * holds, all in one ctx->lock critical section.
 *
 * The lock orders this connection's last reads through a shared SSL_CTX
 * against the destruction of that context. The commit step of
 * _ctls_ctx_rebuild_locked frees every replaced context under the same lock,
 * so a thread changing the configuration can't get to the free that destroys
 * an object until every connection that used it has passed through here.
 * Dropping these references outside the lock would leave that order to
 * OpenSSL's reference counter alone, an atomic inside a library nothing
 * instruments and that ThreadSanitizer can't see, so the destroy would then
 * look like a race against this connection's earlier reads through the same
 * object.
 *
 * SSL_shutdown() is NOT part of this function, on purpose. It writes a
 * close_notify to the peer, and a ctx->lock held across that write would
 * make three things wait on one peer's socket: the creation of every other
 * connection, the teardown of every other connection, and every
 * configuration change. SSL_free() itself does no I/O; it drops the two
 * references SSL_new() took, on the connection's active context and on its
 * session context, and then frees the connection's state. */
static void _ctls_conn_release_ssl(ctls_conn_t *conn) {
  ctls_ctx_t *ctx = conn->ctx;
  _ccol_mutex_lock(ctx->lock);
  if (conn->ssl) {
    SSL_free(conn->ssl);
    conn->ssl = NULL;
  }
  /* SSL_free() above freed the BIO, so its count on the method can go, but
   * the release itself happens after ctx->lock is dropped. */
  bool release_sock_bio = conn->holds_sock_bio;
  conn->holds_sock_bio = false;
  /* SSL_CTX_free() does nothing on NULL, as its documentation says, so
   * neither pin needs its own guard. The accounting does, because the count
   * must not record the release of a pin that was never taken. */
  if (conn->pinned_sni_ctx) {
    SSL_CTX_free(conn->pinned_sni_ctx);
    conn->pinned_sni_ctx = NULL;
    _ctls_account_ctx_pins(-1);
  }
  if (conn->pinned_ctx_default) {
    SSL_CTX_free(conn->pinned_ctx_default);
    conn->pinned_ctx_default = NULL;
    _ctls_account_ctx_pins(-1);
  }
  _ccol_mutex_unlock(ctx->lock);
  if (release_sock_bio) _ctls_sock_bio_release();
}

/* Unwinds a connection that failed halfway through its own construction:
 * everything _ctls_conn_release_ssl covers, plus the ctls_ctx_t reference
 * _ctls_conn_alloc took, and the connection record itself. m_procs is read
 * before the reference on the context is released, because that release
 * may be the last one and free the context, and reading m_procs afterwards
 * would read freed memory. */
static void _ctls_conn_create_unwind(ctls_conn_t *conn) {
  _ctls_conn_release_ssl(conn);
  ctls_ctx_t *ctx = conn->ctx;
  ccol_memmgmt_procs_t *mp = ctx->m_procs;
  ctls_ctx_release(ctx);
  _ccol_mem_free(mp, conn);
}

static ctls_conn_t *_ctls_conn_alloc(ctls_ctx_t *ctx, bool is_server) {
  ctls_conn_t *conn =
      (ctls_conn_t *)_ccol_mem_calloc(ctx->m_procs, 1, sizeof(*conn));
  if (!conn) return NULL;
  conn->ctx = ctx;
  conn->is_server = is_server;
  ctls_ctx_retain(ctx);
  return conn;
}

ctls_conn_t *ctls_conn_create_client(ctls_ctx_t *ctx, int fd,
                                     const char *hostname, bool verify_host,
                                     char **err_str) {
  if (!ctx || fd < 0) {
    if (err_str) *err_str = "ctls_conn_create_client: invalid arguments";
    return NULL;
  }
  /* Verifying the peer's name needs a name to verify against, so asking for
   * it without a name has to fail here instead of quietly connecting with no
   * name check at all. A caller that really wants no name check says so
   * with verify_host set to false. */
  if (verify_host && (!hostname || !*hostname)) {
    if (err_str)
      *err_str =
          "ctls_conn_create_client: hostname verification requested with no "
          "hostname";
    return NULL;
  }
  ctls_conn_t *conn = _ctls_conn_alloc(ctx, false);
  if (!conn) {
    if (err_str) *err_str = "ctls_conn_create_client: allocation failure";
    return NULL;
  }

  _ccol_mutex_lock(ctx->lock);
  SSL *ssl = SSL_new(ctx->ctx_default);
  if (ssl) {
    /* See the doc comment on the pins of struct ctls_conn. This pin is taken
     * under ctx->lock, in the same critical section as the SSL_new() call
     * that reads ctx->ctx_default, so it always names the object this
     * connection's SSL* was created against. conn->ssl is published here and
     * not at the end of the function, so every failure path below can unwind
     * through one helper that already knows how to release it; nothing else
     * can reach conn until this function returns it. */
    SSL_CTX_up_ref(ctx->ctx_default);
    conn->pinned_ctx_default = ctx->ctx_default;
    _ctls_account_ctx_pins(1);
    conn->ssl = ssl;
  }
  _ccol_mutex_unlock(ctx->lock);
  if (!ssl) {
    _ctls_conn_create_unwind(conn);
    if (err_str) *err_str = "ctls_conn_create_client: SSL_new failure";
    return NULL;
  }
  if (!SSL_set_ex_data(ssl, _ctls_conn_ex_idx(), conn)) {
    _ctls_conn_create_unwind(conn);
    if (err_str) *err_str = "ctls_conn_create_client: SSL_set_ex_data failure";
    return NULL;
  }

  BIO *bio = _ctls_sock_bio_new(fd);
  if (!bio) {
    _ctls_conn_create_unwind(conn);
    if (err_str) *err_str = "ctls_conn_create_client: socket BIO failure";
    return NULL;
  }
  /* One BIO for both directions; SSL_set_bio() takes over the single
   * reference BIO_new() gave. */
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
        /* The SSL_free() inside the helper also frees the BIO reference
         * SSL_set_bio() took. */
        _ctls_conn_create_unwind(conn);
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

ctls_conn_t *ctls_conn_create_server(ctls_ctx_t *ctx, int fd, void *udata,
                                     char **err_str) {
  if (!ctx || fd < 0) {
    if (err_str) *err_str = "ctls_conn_create_server: invalid arguments";
    return NULL;
  }
  ctls_conn_t *conn = _ctls_conn_alloc(ctx, true);
  if (!conn) {
    if (err_str) *err_str = "ctls_conn_create_server: allocation failure";
    return NULL;
  }
  conn->udata = udata;

  _ccol_mutex_lock(ctx->lock);
  SSL *ssl = SSL_new(ctx->ctx_default);
  if (ssl) {
    /* See the doc comment on the pins of struct ctls_conn. This pin is taken
     * under ctx->lock, in the same critical section as the SSL_new() call
     * that reads ctx->ctx_default, so it always names the object this
     * connection's SSL* was created against. conn->ssl is published here and
     * not at the end of the function, so every failure path below can unwind
     * through one helper that already knows how to release it; nothing else
     * can reach conn until this function returns it. */
    SSL_CTX_up_ref(ctx->ctx_default);
    conn->pinned_ctx_default = ctx->ctx_default;
    _ctls_account_ctx_pins(1);
    conn->ssl = ssl;
  }
  _ccol_mutex_unlock(ctx->lock);
  if (!ssl) {
    _ctls_conn_create_unwind(conn);
    if (err_str) *err_str = "ctls_conn_create_server: SSL_new failure";
    return NULL;
  }
  if (!SSL_set_ex_data(ssl, _ctls_conn_ex_idx(), conn)) {
    _ctls_conn_create_unwind(conn);
    if (err_str) *err_str = "ctls_conn_create_server: SSL_set_ex_data failure";
    return NULL;
  }

  BIO *bio = _ctls_sock_bio_new(fd);
  if (!bio) {
    _ctls_conn_create_unwind(conn);
    if (err_str) *err_str = "ctls_conn_create_server: socket BIO failure";
    return NULL;
  }
  /* One BIO for both directions; SSL_set_bio() takes over the single
   * reference BIO_new() gave. */
  SSL_set_bio(ssl, bio, bio);
  conn->holds_sock_bio = true;
  BIO_set_callback_arg(bio, (char *)&conn->rec_track);
  SSL_set_accept_state(ssl);
  return conn;
}

/* Records the protocol conn's handshake agreed on and fires its on_selected
 * callback. ctls_conn_handshake_step() calls it in both modes once the
 * handshake is done, as its last use of conn, so a callback may destroy its
 * own connection. The callback runs with no lock held, because it may come
 * back into this same ctx, with its own ctls_ctx_alpn_add() call for
 * example. */
static void _ctls_record_alpn(ctls_conn_t *conn) {
  ctls_ctx_t *tls = conn->ctx;
  const unsigned char *proto = NULL;
  unsigned int proto_len = 0;
  SSL_get0_alpn_selected(conn->ssl, &proto, &proto_len);
  _ccol_mutex_lock(tls->lock);
  if (tls->alpn_count == 0) {
    _ccol_mutex_unlock(tls->lock);
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
    /* The two sides agree on no protocol: the peer sent no ALPN extension,
     * or no protocol this context registered. Nothing is recorded on conn
     * and no on_selected callback fires. Reporting a registered protocol
     * name here would tell a client that offered {"h2", "http/1.1"} that it
     * agreed on "h2" with a peer that never answered, and that client would
     * then speak HTTP/2 framing to an HTTP/1.1 server. */
    _ccol_mutex_unlock(tls->lock);
    return;
  }
  /* We copy the name BYTES into conn's own buffer because a pointer to
   * matched->name isn't enough, for the same reason _ctls_alpn_select_cb
   * copies them: tls->alpn is mutable storage that a concurrent
   * ctls_ctx_alpn_add() call can free or reallocate, while a caller may read
   * conn->alpn_selected_name at any later point in conn's life, long after
   * this handshake has completed and this lock has been released. */
  memcpy(conn->alpn_selected_buf, matched->name, matched->name_len);
  conn->alpn_selected_buf[matched->name_len] = '\0';
  conn->alpn_selected_name = conn->alpn_selected_buf;
  conn->alpn_selected_len = matched->name_len;
  ctls_alpn_selected_fn cb = matched->on_selected;
  size_t name_len = matched->name_len;
  void *udata = matched->udata;
  _ccol_mutex_unlock(tls->lock);
  if (cb) cb(conn, conn->alpn_selected_buf, name_len, udata);
}

ctls_handshake_result_t ctls_conn_handshake_step(ctls_conn_t *conn) {
  if (!conn || !conn->ssl) return CTLS_HANDSHAKE_ERROR;
  ERR_clear_error();
  /* SSL_accept and SSL_connect: see this function's @note in ctls.h about a
   * known ThreadSanitizer race that belongs to a third party alone. The race
   * is inside libcrypto (ASN1_STRING_cmp reading what ASN1_STRING_set
   * writes) and shows up when many concurrent client handshakes verify
   * certificates against one shared trust store; this call site can't fix
   * it without serializing handshakes that are meant to run at the same
   * time. */
  int ri = conn->is_server ? SSL_accept(conn->ssl) : SSL_connect(conn->ssl);
  if (ri == 1) {
    conn->handshake_done = true;
    _ctls_record_alpn(conn);
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

/* Shared classification of SSL_read and SSL_write results. It is hardened
 * on purpose in three ways. SSL_ERROR_SYSCALL must never turn into an
 * EWOULDBLOCK that makes the caller spin, SSL_ERROR_SSL is a fatal
 * record-layer problem and not the same thing as a clean close, and
 * ERR_clear_error() must run before every SSL_get_error() classification
 * because in this codebase the TLS I/O of many unrelated connections shares
 * a small set of threads.
 *
 * Whenever it classifies a result as -1 with EWOULDBLOCK, it also records on
 * conn the direction OpenSSL reported (SSL_ERROR_WANT_READ or
 * SSL_ERROR_WANT_WRITE). The doc comment of ctls_conn_wants_write() explains
 * why a caller can't assume that the direction matches the function it
 * called, ctls_conn_read() or ctls_conn_write(). */
static ssize_t _ctls_classify_io_result(ctls_conn_t *conn, int ret) {
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
      /* SSL_ERROR_WANT_READ and every other non-fatal outcome: the default
       * that assumes a read. */
      conn->last_io_wants_write = false;
      errno = EWOULDBLOCK;
      return -1;
  }
}

/* SSL_read and SSL_write take an int length, while the two entry points
 * below take a size_t, which is what a caller of a byte-stream API has. A
 * plain cast is wrong for any length above INT_MAX: it either truncates to a
 * smaller positive value, silently moving fewer bytes than asked and
 * reporting that count as a success, or it gives a negative value that
 * OpenSSL rejects outright.
 *
 * The clamp turns both cases into an ordinary SHORT transfer, which is
 * already part of these two functions' contract: SSL_MODE_ENABLE_PARTIAL_WRITE
 * is on (see _ctls_apply_base_settings) and a read returns whatever one
 * record holds, so every caller in this library (chttp1_stream_read and
 * chttp1_stream_write) already loops on the returned count.
 *
 * A length above INT_MAX can really happen: a streaming response handler
 * passes chttpsvr_resp_write a buffer of its own choosing, and the write
 * loop of _send_response hands the remaining length straight through. */
static inline int _ctls_clamp_io_len(size_t len) {
  return len > (size_t)INT_MAX ? INT_MAX : (int)len;
}

/* Whether a failed SSL_read() is the peer closing the socket without
 * close_notify on a record boundary. curl and the Go net/http client take
 * such a close as the end of the data because many servers close that way,
 * and so do we: ctls_conn_read() then returns 0, as for close_notify. A
 * close inside a record is a truncation with no innocent explanation, so it
 * stays an error, as in Go's crypto/tls. OpenSSL 3 reports the close as
 * SSL_R_UNEXPECTED_EOF_WHILE_READING, OpenSSL 1.1 and LibreSSL as
 * SSL_ERROR_SYSCALL. The BIO's EOF flag says whether the socket really
 * reached its end, and the record tracker whether any part of a record is
 * left over. */
static bool _ctls_eof_on_record_boundary(ctls_conn_t *conn, int ret) {
  int err = SSL_get_error(conn->ssl, ret);
  if (err != SSL_ERROR_SYSCALL && err != SSL_ERROR_SSL) return false;
  BIO *rb = SSL_get_rbio(conn->ssl);
  if (!rb || !BIO_test_flags(rb, _CTLS_SOCK_BIO_EOF)) return false;
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

ssize_t ctls_conn_read(ctls_conn_t *conn, void *buf, size_t len) {
  if (!conn || !conn->ssl) {
    errno = EINVAL;
    return -1;
  }
  /* A read of nothing returns 0 at once, as read(2) does. SSL_read
   * answers a length of 0 with 0, which the classification below would
   * report as a reset or as waiting for input. */
  if (len == 0) return 0;
  ERR_clear_error();
  int ret = SSL_read(conn->ssl, buf, _ctls_clamp_io_len(len));
  if (ret <= 0 && _ctls_eof_on_record_boundary(conn, ret)) {
    ERR_clear_error();
    return 0;
  }
  return _ctls_classify_io_result(conn, ret);
}

ssize_t ctls_conn_write(ctls_conn_t *conn, const void *buf, size_t len) {
  if (!conn || !conn->ssl) {
    errno = EINVAL;
    return -1;
  }
  /* A write of nothing moves nothing, as write(2) on a socket does. SSL_write
   * answers a length of 0 with 0, which the classification below can't tell
   * from a failure and would report as a reset. */
  if (len == 0) return 0;
  ERR_clear_error();
  int ret = SSL_write(conn->ssl, buf, _ctls_clamp_io_len(len));
  return _ctls_classify_io_result(conn, ret);
}

bool ctls_conn_wants_write(const ctls_conn_t *conn) {
  return conn && conn->last_io_wants_write;
}

/* SSL_pending() counts the decrypted bytes of the current record no read has
 * taken yet, and SSL_has_pending() also covers record bytes OpenSSL holds
 * but hasn't decrypted. Either one means the socket can be empty while a
 * read still has something to give, which poll(2) and epoll(7) can't see.
 * This module puts no buffer of its own between the socket BIO and OpenSSL,
 * so those two answers are the whole story. LibreSSL has no
 * SSL_has_pending(), and there the socket BIO tells whether the TLS layer
 * holds part of a record; see _ctls_rec_track_t. */
bool ctls_conn_has_pending_input(const ctls_conn_t *conn) {
  if (!conn || !conn->ssl) return false;
#if defined(LIBRESSL_VERSION_NUMBER)
  return SSL_pending(conn->ssl) > 0 || conn->rec_track.hdr_got != 0 ||
         conn->rec_track.lost;
#else
  return SSL_pending(conn->ssl) > 0 || SSL_has_pending(conn->ssl) == 1;
#endif
}

/* The peer's certificate without a new reference, since the session keeps
 * one. LibreSSL and OpenSSL 1.1.1 have no SSL_get0_peer_certificate(), and
 * their SSL_get_peer_certificate() takes a reference, which we drop right
 * away. */
static X509 *_ctls_peer_cert0(const SSL *ssl) {
#if defined(LIBRESSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER < 0x30000000L
  X509 *cert = SSL_get_peer_certificate(ssl);
  X509_free(cert);
  return cert;
#else
  return SSL_get0_peer_certificate(ssl);
#endif
}

long ctls_conn_verify_result(const ctls_conn_t *conn) {
  if (!conn || !conn->ssl) return -1;
  return SSL_get_verify_result(conn->ssl);
}

/* SSL_get_verify_result() reports X509_V_OK for a connection that verified
 * nothing at all, one without any verify mode included, so the answer also
 * needs a peer certificate and a verify mode that checked it. A resumed
 * session carries the certificate and the verify result of the handshake
 * that established it. */
bool ctls_conn_peer_cert_verified(const ctls_conn_t *conn) {
  if (!conn || !conn->ssl || !conn->handshake_done) return false;
  return (SSL_get_verify_mode(conn->ssl) & SSL_VERIFY_PEER) != 0 &&
         _ctls_peer_cert0(conn->ssl) != NULL &&
         SSL_get_verify_result(conn->ssl) == X509_V_OK;
}

/* Makes the DER encoding and the subject of the peer's verified certificate,
 * once per certificate. Returns that certificate, or NULL when the peer has
 * no verified certificate or an allocation fails; whatever couldn't be made
 * stays NULL. */
static const X509 *_ctls_conn_peer_info(ctls_conn_t *conn) {
  if (!ctls_conn_peer_cert_verified(conn)) return NULL;
  X509 *cert = _ctls_peer_cert0(conn->ssl);
  if (!cert) return NULL;
  if (conn->peer_info_cert == cert) return cert;
  ccol_memmgmt_procs_t *mp = conn->ctx->m_procs;
  _ccol_mem_free(mp, conn->peer_der);
  _ccol_mem_free(mp, conn->peer_subject);
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
  unsigned char *der = (unsigned char *)_ccol_mem_alloc(mp, (size_t)der_len);
  if (!der) return NULL;
  unsigned char *w = der;
  if (i2d_X509(cert, &w) != der_len) {
    ERR_clear_error();
    _ccol_mem_free(mp, der);
    return NULL;
  }

  /* XN_FLAG_RFC2253 escapes every byte above 0x7F and every control
   * character, so the text is printable ASCII without a NUL inside. */
  char *subject = NULL;
  BIO *bio = BIO_new(BIO_s_mem());
  if (bio && X509_NAME_print_ex(bio, X509_get_subject_name(cert), 0,
                                XN_FLAG_RFC2253) >= 0) {
    char *text = NULL;
    long text_len = BIO_get_mem_data(bio, &text);
    if (text_len >= 0) {
      subject = (char *)_ccol_mem_alloc(mp, (size_t)text_len + 1);
      if (subject) {
        if (text_len) memcpy(subject, text, (size_t)text_len);
        subject[text_len] = '\0';
      }
    }
  }
  BIO_free(bio);
  ERR_clear_error();
  if (!subject || X509_up_ref(cert) != 1) {
    _ccol_mem_free(mp, subject);
    _ccol_mem_free(mp, der);
    return NULL;
  }
  conn->peer_der = der;
  conn->peer_der_len = (size_t)der_len;
  conn->peer_subject = subject;
  conn->peer_info_cert = cert;
  return cert;
}

const unsigned char *ctls_conn_peer_cert_der(ctls_conn_t *conn,
                                             size_t *len_out) {
  if (len_out) *len_out = 0;
  if (!_ctls_conn_peer_info(conn)) return NULL;
  if (len_out) *len_out = conn->peer_der_len;
  return conn->peer_der;
}

bool ctls_conn_peer_cert_sha256(ctls_conn_t *conn,
                                unsigned char out[CTLS_SHA256_LEN]) {
  if (!out) return false;
  memset(out, 0, CTLS_SHA256_LEN);
  if (!_ctls_conn_peer_info(conn)) return false;
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

const char *ctls_conn_peer_cert_subject(ctls_conn_t *conn) {
  if (!_ctls_conn_peer_info(conn)) return NULL;
  return conn->peer_subject;
}

const char *ctls_conn_alpn_selected(const ctls_conn_t *conn, size_t *len_out) {
  if (!conn || !conn->alpn_selected_name) {
    if (len_out) *len_out = 0;
    return NULL;
  }
  if (len_out) *len_out = conn->alpn_selected_len;
  return conn->alpn_selected_name;
}

void *ctls_conn_udata(const ctls_conn_t *conn) {
  return conn ? conn->udata : NULL;
}

void ctls_conn_destroy(ctls_conn_t *conn) {
  if (!conn) return;
  /* This runs outside ctx->lock because it writes a close_notify to the peer,
   * and holding a lock across one peer's socket would queue every other
   * connection of this context behind that socket. */
  /* A connection whose handshake never completed has no session to close.
   * SSL_shutdown() refuses it and leaves an entry on this thread's error
   * queue, where a later, unrelated OpenSSL call on the same thread could
   * read it as its own. A close_notify to a peer that's gone fails the same
   * way, so the queue is cleared after either outcome. */
  if (conn->ssl && conn->handshake_done) SSL_shutdown(conn->ssl);
  ERR_clear_error();
  /* The SSL* and both SSL_CTX pins go together under ctx->lock, which is what
   * orders this connection's last reads through a shared context against the
   * destruction of that context; the doc comment of _ctls_conn_release_ssl()
   * explains why. */
  _ctls_conn_release_ssl(conn);
  /* m_procs is read before this connection's reference is released, because
   * ctls_ctx_release() frees ctx if this is the last reference, and reading
   * conn->ctx->m_procs after that call would be a use-after-free. */
  ccol_memmgmt_procs_t *mp = conn->ctx->m_procs;
  _ccol_mem_free(mp, conn->peer_der);
  _ccol_mem_free(mp, conn->peer_subject);
  X509_free(conn->peer_info_cert);
  ctls_ctx_release(conn->ctx);
  _ccol_mem_free(mp, conn);
}

#ifdef RUNNING_UNIT_TESTS
/* White-box accessor exposing the raw OpenSSL SSL*, so tests/ctls/tests.c can
 * inspect the peer certificate the two sides agreed on, which the suite uses
 * to verify the SNI dispatch. Real callers need no public equivalent. */
SSL *_ctls_conn_ssl_for_tests(ctls_conn_t *conn) {
  return conn ? conn->ssl : NULL;
}

/* White-box accessor giving the SSL_CTX a servername dispatch installed on
 * this connection and that this module holds a reference on, or NULL for a
 * connection that never dispatched. A suite can then assert that the context
 * a connection serves from is one we pin ourselves, rather than taking that
 * answer from OpenSSL's internal bookkeeping. */
SSL_CTX *_ctls_conn_pinned_sni_ctx_for_tests(ctls_conn_t *conn) {
  return conn ? conn->pinned_sni_ctx : NULL;
}

/* White-box accessor giving the SSL_CTX that a ClientHello naming
 * server_name is served from: the matching named certificate's context,
 * found the same way the servername callback finds it, or the default
 * context for a NULL or unmatched name. A suite can then read the
 * certificate every name serves after each configuration change, without a
 * handshake per name. */
SSL_CTX *_ctls_ctx_served_ctx_for_tests(ctls_ctx_t *tls,
                                        const char *server_name) {
  if (!tls) return NULL;
  _ccol_mutex_lock(tls->lock);
  SSL_CTX *found = _ctls_find_named_ctx(tls, server_name);
  if (!found) found = tls->ctx_default;
  _ccol_mutex_unlock(tls->lock);
  return found;
}
#endif
