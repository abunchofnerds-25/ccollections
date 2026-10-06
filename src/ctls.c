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
#include <internal/csock.h>
#include <internal/ctls.h>
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

/* ========================================================================== */
/*                       PROCESS-WIDE SELF-SIGNED ROOT KEY                    */
/* ========================================================================== */

/* The module generates this key one time for each process, and only when it
 * first needs the key. Every self-signed certificate that this module
 * generates shares the key. The shape of this code is the same as the shape
 * of fio_tls_make_root_key. This state belongs to the whole process, and not
 * to one ctls_ctx_t. A ccol_once_flag_t therefore guards it, and not a mutex
 * of one instance. */
static struct {
  EVP_PKEY *key;
  ccol_mutex_t mutex;
  ccol_once_flag_t once;
} ctls_root_key_bundle = {0};

static void _ctls_root_key_globals_init(void) {
  if (ccol_mutex_init(ctls_root_key_bundle.mutex) != 0)
    ccol_fatal_err("ctls root key: failed to initialize mutex");
}

static EVP_PKEY *_ctls_get_root_key(void) {
  ccol_call_once(ctls_root_key_bundle.once, _ctls_root_key_globals_init);
  ccol_mutex_lock(ctls_root_key_bundle.mutex);
  if (!ctls_root_key_bundle.key) {
    /* EVP_RSA_gen is a thin macro over EVP_PKEY_Q_keygen. It is the modern
     * OpenSSL 3.x replacement for the older sequence of RSA_new, BN_new,
     * RSA_generate_key_ex and EVP_PKEY_assign_RSA. OpenSSL 3.0 deprecates
     * that older sequence, so the older sequence fires
     * -Wdeprecated-declarations in the -Werror build of this codebase. This
     * file is not vendored code, so it does not build with relaxed
     * warnings. */
    ctls_root_key_bundle.key = EVP_RSA_gen(2048);
  }
  EVP_PKEY *key = ctls_root_key_bundle.key;
  ccol_mutex_unlock(ctls_root_key_bundle.mutex);
  return key;
}

/* ========================================================================== */
/*                       ALPN EX-DATA INDEX (SSL* -> ctls_conn_t*) */
/* ========================================================================== */

/* idx starts at -1. This is the "not registered yet" sentinel that
 * _ctls_conn_ex_idx() tests for. A start value of 0 looks exactly like a
 * successful registration. It makes the registration below unreachable, and
 * this module then silently uses ex_data index 0. OpenSSL reserves index 0
 * for the SSL_set_app_data() and SSL_get_app_data() macros. Other code in
 * the process that gets an SSL* and uses those macros then reads the
 * ctls_conn_t* of this module through them, and writes over it through them.
 * SSL_get_ex_new_index() also reports its own failure as -1. A failed
 * registration therefore leaves the sentinel in place, and the next call
 * tries again. The two SSL_set_ex_data() call sites already report a failure
 * to their own caller. */
static struct {
  int idx;
  ccol_mutex_t mutex;
  ccol_once_flag_t once;
} ctls_conn_ex_idx_bundle = {.idx = -1};

static void _ctls_ex_idx_globals_init(void) {
  if (ccol_mutex_init(ctls_conn_ex_idx_bundle.mutex) != 0)
    ccol_fatal_err("ctls conn ex_data index: failed to initialize mutex");
}

static int _ctls_conn_ex_idx(void) {
  ccol_call_once(ctls_conn_ex_idx_bundle.once, _ctls_ex_idx_globals_init);
  ccol_mutex_lock(ctls_conn_ex_idx_bundle.mutex);
  if (ctls_conn_ex_idx_bundle.idx < 0)
    ctls_conn_ex_idx_bundle.idx =
        SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
  int idx = ctls_conn_ex_idx_bundle.idx;
  ccol_mutex_unlock(ctls_conn_ex_idx_bundle.mutex);
  return idx;
}

/* ========================================================================== */
/*                  SOCKET BIO THAT NEVER RAISES SIGPIPE                      */
/* ========================================================================== */

/* Every connection of this module reads and writes through this BIO_METHOD.
 * It is a socket BIO with the behavior of the socket BIO of OpenSSL, except
 * that it writes with send(2) and MSG_NOSIGNAL. The socket BIO of OpenSSL
 * writes with write(2). A write(2) to a stream socket whose peer is gone
 * raises SIGPIPE, and the default disposition of SIGPIPE ends the whole
 * process. A peer that resets a keep-alive connection is ordinary network
 * behavior, and the close_notify that ctls_conn_destroy() sends goes to
 * exactly such a socket. With MSG_NOSIGNAL that write fails with EPIPE
 * instead, and the caller sees an ordinary I/O error, whatever disposition of
 * SIGPIPE the application has chosen.
 *
 * The read side, the retry flags, the end-of-file flag and the control
 * queries match the socket BIO of OpenSSL, because the record layer of
 * OpenSSL reads them: BIO_eof() is what tells an abrupt close of the peer
 * from a retryable read, and the poll descriptors are what OpenSSL reports
 * through SSL_get_rpoll_descriptor(). The type carries BIO_TYPE_DESCRIPTOR,
 * so BIO_find_type() and SSL_get_fd() find it. The file descriptor lives in
 * the data pointer of the BIO, so a write reads it with no control call.
 *
 * The method outlives every BIO built from it. Each BIO that this module
 * creates holds one count on it, and the count drops after SSL_free() has
 * freed that BIO. The destructor at the exit of the process frees the method
 * only once no BIO holds it. When a connection is still live at that point,
 * the destructor marks the method retired and the release of the last BIO
 * frees it. This library does not control the order of the destructors of a
 * process, and a later destructor can still tear down a TLS connection and
 * write its close_notify through this method. A connection created after the
 * destructor ran builds a fresh method under the same rule. The mutex is
 * never destroyed, because a release can follow the destructor. */
static struct {
  BIO_METHOD *meth;
  size_t live_bios;
  bool retired;
  ccol_mutex_t mutex;
  ccol_once_flag_t once;
} ctls_sock_bio_bundle = {.once = CCOL_ONCE_INIT};

static void _ctls_sock_bio_globals_init(void) {
  if (ccol_mutex_init(ctls_sock_bio_bundle.mutex) != 0)
    ccol_fatal_err("ctls socket BIO: failed to initialize mutex");
}

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
      BIO_set_flags(b, BIO_FLAGS_IN_EOF);
  }
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
      return BIO_test_flags(b, BIO_FLAGS_IN_EOF) != 0;
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

/* Builds a socket BIO over fd that does not close fd, and charges one count
 * on the method. Gives NULL on a failure, and charges nothing then. */
static BIO *_ctls_sock_bio_new(int fd) {
  /* The socket comes from the caller. Where SIGPIPE is suppressed per socket
   * and not per send, the BIO makes that true itself, and refuses a socket on
   * which it cannot. */
  if (ccol_sock_nosigpipe(fd) != 0) return NULL;
  ccol_call_once(ctls_sock_bio_bundle.once, _ctls_sock_bio_globals_init);
  ccol_mutex_lock(ctls_sock_bio_bundle.mutex);
  if (!ctls_sock_bio_bundle.meth) {
    ctls_sock_bio_bundle.meth = _ctls_sock_bio_method_build();
    ctls_sock_bio_bundle.retired = false;
  }
  /* The count is charged before the unlock, so the method cannot be freed
   * while BIO_new() below uses it outside the lock. */
  BIO_METHOD *meth = ctls_sock_bio_bundle.meth;
  if (meth) ctls_sock_bio_bundle.live_bios++;
  ccol_mutex_unlock(ctls_sock_bio_bundle.mutex);
  if (!meth) return NULL;
  BIO *bio = BIO_new(meth);
  if (!bio) {
    _ctls_sock_bio_release();
    return NULL;
  }
  BIO_set_fd(bio, fd, BIO_NOCLOSE);
  return bio;
}

/* Drops the count of one BIO that is already freed. It frees the method when
 * the destructor has retired it and this was the last BIO. */
static void _ctls_sock_bio_release(void) {
  ccol_mutex_lock(ctls_sock_bio_bundle.mutex);
  if (--ctls_sock_bio_bundle.live_bios == 0 && ctls_sock_bio_bundle.retired) {
    BIO_meth_free(ctls_sock_bio_bundle.meth);
    ctls_sock_bio_bundle.meth = NULL;
    ctls_sock_bio_bundle.retired = false;
  }
  ccol_mutex_unlock(ctls_sock_bio_bundle.mutex);
}

__attribute__((destructor)) static void _ctls_sock_bio_cleanup(void) {
  ccol_call_once(ctls_sock_bio_bundle.once, _ctls_sock_bio_globals_init);
  ccol_mutex_lock(ctls_sock_bio_bundle.mutex);
  if (ctls_sock_bio_bundle.meth) {
    if (ctls_sock_bio_bundle.live_bios == 0) {
      BIO_meth_free(ctls_sock_bio_bundle.meth);
      ctls_sock_bio_bundle.meth = NULL;
    } else {
      ctls_sock_bio_bundle.retired = true;
    }
  }
  ccol_mutex_unlock(ctls_sock_bio_bundle.mutex);
}

#ifdef RUNNING_UNIT_TESTS
/* A white-box accessor. It gives the number of BIOs of this module that are
 * still alive, so a suite can assert that every connection gives its count
 * back. */
size_t _ctls_live_sock_bio_count_for_tests(void) {
  ccol_call_once(ctls_sock_bio_bundle.once, _ctls_sock_bio_globals_init);
  ccol_mutex_lock(ctls_sock_bio_bundle.mutex);
  size_t n = ctls_sock_bio_bundle.live_bios;
  ccol_mutex_unlock(ctls_sock_bio_bundle.mutex);
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

/* One named certificate, which the module dispatches by SNI. It holds the
 * stored configuration and its own SSL_CTX, which the module builds in full.
 * The module rebuilds that SSL_CTX every time the state of the whole ctx
 * changes, which means the trust store or the ALPN protocols. */
typedef struct ctls_named_cert {
  char *cert_pem;
  size_t cert_len;
  char *key_pem;
  size_t key_len;
  char *pk_password;
  bool self_signed;
  char *self_signed_name; /* the subject name of a self_signed entry */
  SSL_CTX *built_ctx;     /* NULL until the first successful rebuild */
} ctls_named_cert;

struct ctls_ctx {
  size_t ref;
  ccol_mutex_t lock;
  ccol_memmgmt_procs_t *m_procs; /* owned copy, or NULL for default alloc */

  /* The default certificate, which a server_name of NULL or "" in
   * ctls_ctx_cert_add selects. */
  bool has_default_cert;
  char *default_cert_pem;
  size_t default_cert_len;
  char *default_key_pem;
  size_t default_key_len;
  char *default_pk_password;
  bool default_self_signed;
  char *default_self_signed_name;

  /* The named certificates, which the module dispatches by SNI. The map is a
   * chmap from a char* hostname to a ctls_named_cert*. */
  chmap named_certs;
  bool sni_callback_installed;

  /* The trust store: an array of the CA-bundle PEM blobs that the module
   * loaded. */
  char **trust_pems;
  size_t *trust_lens;
  size_t trust_count;
  size_t trust_cap;
  bool verify_default_store;
  bool verify_peer;
  /* False, the default, makes a server-mode connection with a trust store
   * refuse a peer that presents no certificate. True only requests one. See
   * ctls_ctx_peer_cert_optional(). */
  bool peer_cert_optional;

  /* The ALPN protocol registrations, in the order of the registrations.
   * Index 0 is the default one. */
  ctls_alpn_entry *alpn;
  size_t alpn_count;
  size_t alpn_cap;

  SSL_CTX
  *ctx_default; /* from the fields above. NULL until the first build */
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
  /* The connection owns this copy of the selected ALPN protocol name. The
   * documented limit of ctls_ctx_alpn_add is 1 to 255 bytes. The buffer holds
   * that limit plus one byte of headroom. _ctls_alpn_select_cb fills the
   * buffer in server mode, and _ctls_record_client_alpn fills it in client
   * mode. Neither of them points alpn_selected_name into the tls->alpn[]
   * storage of the ctls_ctx_t that owns the connection. That storage is
   * mutable. A concurrent ctls_ctx_alpn_add() call that replaces or adds a
   * protocol can _ccol_mem_free() the name buffer of an entry that exists.
   * It can also _ccol_mem_realloc() the whole array. The doc comment of
   * ctls_ctx_retain() documents this kind of concurrent change of the
   * configuration under live traffic as a supported use case, and not as a
   * hypothetical one. The name of the selected protocol of a connection must
   * therefore not stay a raw pointer into that storage for the life of the
   * connection. */
  char alpn_selected_buf[256];
  /* _ctls_classify_io_result sets this field every time ctls_conn_read() or
   * ctls_conn_write() returns -1 with EWOULDBLOCK. It sets the field to the
   * direction that OpenSSL reported for that one call, which is
   * SSL_ERROR_WANT_READ or SSL_ERROR_WANT_WRITE. See the doc comment of
   * ctls_conn_wants_write() in ctls.h for the reason that this field exists.
   * After the handshake, SSL_read() and SSL_write() can each need the
   * OPPOSITE direction to the one that the name of the call suggests. Two
   * examples are a flush of a deferred TLS 1.3 session ticket during
   * SSL_read(), and a read that a TLS 1.2 renegotiation drives during
   * SSL_write(). A plain read(2) or write(2) is different: its own
   * EWOULDBLOCK always means "wait for this exact same direction". This
   * field keeps the value from the last classified call. It has a meaning
   * only immediately after a read or write call that returned -1 with
   * EWOULDBLOCK. */
  bool last_io_wants_write;
  /* These two fields name every SSL_CTX that this connection can still
   * reach. This module takes its own reference on each one with
   * SSL_CTX_up_ref, and _ctls_conn_release_ssl() releases both references.
   *
   *   pinned_ctx_default: the SSL_CTX* that ctx->ctx_default held at the
   *     moment of the SSL_new() call for this connection. The module takes
   *     the pin in the same ctx->lock critical section as that call. The pin
   *     therefore always names the object that the SSL* of the connection
   *     was created against. It is NULL if SSL_new() failed before the
   *     module could take the pin. OpenSSL reads this object for the whole
   *     life of the connection through its own separate session-context
   *     field. It does this even after an SNI dispatch moves the active
   *     context of the connection to another object.
   *
   *   pinned_sni_ctx: the SSL_CTX of the named certificate that the module
   *     dispatches by SNI. _ctls_servername_cb installs that SSL_CTX with
   *     SSL_set_SSL_CTX(). The module takes the pin in the same ctx->lock
   *     critical section as the lookup that produced the object and the
   *     install that put it on the connection. This field is NULL for a
   *     connection that never reaches the servername callback. Every client
   *     connection is such a connection, and so is every server connection
   *     whose peer sends no SNI extension. The field is also NULL for a
   *     connection whose SNI name matches no named certificate. All of these
   *     connections continue to serve from pinned_ctx_default.
   *
   * OpenSSL takes references of its own on both objects. SSL_new takes one
   * reference for the active context of the connection, and one for its
   * session context. SSL_set_SSL_CTX takes one reference on the context that
   * it installs, and it drops the reference on the context that it replaces.
   * These pins are therefore not the only thing that holds either object
   * alive. The pins make the guarantee this module's own: a context that a
   * live connection can still reach stays alive because this module holds a
   * reference on it. It does not stay alive because the internal field
   * bookkeeping of a third party happens to hold it.
   *
   * The module releases both pins under ctx->lock. The commit step of
   * _ctls_ctx_rebuild_locked frees every replaced context under that same
   * lock, and the module calls SSL_free() under it for the same reason. A
   * release outside that lock leaves an ordering to OpenSSL alone. That
   * ordering is between "this connection stopped reading a shared context"
   * and "another thread can now destroy that context". The only thing that
   * carries it is then the internal reference counter of OpenSSL, which is
   * an atomic inside a library that nothing instruments. ThreadSanitizer
   * cannot see that atomic. It therefore reports the destroy of a context by
   * the thread that changes the configuration as a race against the earlier
   * reads of this connection through the same context. The module takes
   * ctx->lock for the release, which supplies the missing ordering from a
   * primitive that this module owns. */
  SSL_CTX *pinned_ctx_default;
  SSL_CTX *pinned_sni_ctx;
  /* True while the SSL* of this connection owns a BIO that charged a count on
   * the socket BIO method. See ctls_sock_bio_bundle. */
  bool holds_sock_bio;
  /* The DER encoding and the RFC 2253 subject of the verified certificate of
   * the peer, made the first time that a caller asks and kept for the life
   * of the connection; see ctls_conn_peer_cert_der(). peer_info_cert names
   * the certificate that they describe, so that a different certificate,
   * which a renegotiation can bring, makes them again. The connection holds
   * a reference on it, so its address can never name another certificate
   * while the cache describes it. The allocator of the context owns both
   * buffers. */
  X509 *peer_info_cert;
  unsigned char *peer_der;
  size_t peer_der_len;
  char *peer_subject;
};

#ifdef RUNNING_UNIT_TESTS
/* The number of SSL_CTX references that this module holds for live
 * connections. See the pin fields of struct ctls_conn. Every pin that the
 * module takes adds one, and every pin that it releases subtracts one. A
 * suite can therefore assert two things. First, a connection pins exactly
 * the contexts that it can reach. Second, the total comes back to its start
 * value after the module destroys every connection. The second assertion
 * turns "a context that the module rebuilds many times does not collect
 * references" from a claim into a measurement.
 *
 * The counter is relaxed. The module takes and releases a pin on the thread
 * that drives that connection, so several threads write the counter at the
 * same time. The counter must therefore be atomic. Without this, the counter
 * is itself a data race in code that every sanitizer run compiles. Nothing
 * orders the counter beyond the ctx->lock around it, and a test reads it
 * only after it joins the threads under test. */
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
  for (size_t i = 0; i < len; ++i) d[i] = (char)tolower((unsigned char)s[i]);
  return d;
}

/* This ceiling bounds the allocation that a path from the caller can cause
 * in this function. A PEM artefact is small. A certificate or a private key
 * is a few kilobytes. A full system CA bundle, which is the Mozilla set, is
 * a few hundred kilobytes. 16 MB is far above every legitimate input. It
 * also keeps a path that points at something completely different a fast,
 * clean error. Such a path can name a log, a disk image or a core dump, and
 * the ceiling stops a multi-gigabyte allocation for it. The type is off_t,
 * which matches the size that fstat(2) reports and that the code compares it
 * against. The comparison therefore carries no conversion of the
 * signedness. */
#define CTLS_MAX_PEM_FILE_SIZE ((off_t)16 * 1024 * 1024)

/* Reads a whole file into a new buffer. Returns false on every failure, and
 * leaves no partial allocation behind. The failures are a file that is
 * absent, a path that does not name a regular file, an error of the read, and
 * a file of zero length. A file of zero length is never a meaningful
 * certificate, key or CA bundle.
 *
 * The open never waits. A path that names a FIFO with no writer, a terminal
 * or a device blocks a plain open(2) or its first read for ever, and the
 * caller can hold a lock of a client or a server while it reads. O_NONBLOCK
 * makes the open of such a path return at once, the check of the type of the
 * opened descriptor then refuses it before a byte is read, and O_NONBLOCK has
 * no effect on the reads of a regular file. The check runs on the descriptor
 * and not on the path, so a path that something replaces between a check and
 * the open cannot get past it. O_NOCTTY keeps a terminal from becoming the
 * controlling terminal of the process, and O_CLOEXEC keeps a process that
 * another thread spawns meanwhile from inheriting the descriptor. */
static bool _ctls_read_file(ccol_memmgmt_procs_t *mp, const char *path,
                            char **out_data, size_t *out_len, char **why) {
  *out_data = NULL;
  *out_len = 0;
  /* Every failure below keeps this string, if that failure has nothing more
   * exact to say. A caller therefore never reports a bare "unreadable" for a
   * file that it could read without a problem. */
  if (why) *why = "file is missing or unreadable";
  if (!path) return false;
  int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
  if (fd < 0) return false;
  /* Only a regular file has a meaningful size to read. A certificate option
   * or a CA-bundle option that points at a directory, a FIFO or a device is
   * an ordinary slip in the configuration. OpenSSL itself keeps CAfile apart
   * from CApath for this reason. */
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
  /* A file that shrank since the fstat reads short, and is refused rather
   * than parsed in part. */
  if (got != sz) {
    _ccol_mem_free(mp, buf);
    return false;
  }
  *out_data = buf;
  *out_len = sz;
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

static X509 *_ctls_create_self_signed(const char *server_name) {
  EVP_PKEY *root_key = _ctls_get_root_key();
  if (!root_key) return NULL;
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

/* ========================================================================== */
/*                          SSL_CTX (RE)BUILDING                              */
/* ========================================================================== */

/* The session id context that every SSL_CTX of this module carries. See
 * _ctls_apply_base_settings(). One constant for every context is what keeps
 * resumption working across an SNI dispatch: SSL_set_SSL_CTX() replaces the
 * session id context of a connection with the one of the new context only
 * when the connection still carries the one of the old context, and the two
 * are then byte for byte the same. The value does not need to tell one
 * ctls_ctx_t from another. The session cache and the ticket keys belong to
 * the SSL_CTX that a connection starts on, which is the default context of
 * one ctls_ctx_t and of one build of it, so a session never crosses from one
 * ctls_ctx_t to another in the first place. */
static const unsigned char _ctls_session_id_context[] = "ccol-ctls";

/* Applies the five explicit settings that every SSL_CTX of this module
 * carries. It applies nothing else. The module does not touch the defaults of
 * OpenSSL for the session cache and for the tickets.
 *
 * SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER lets a caller retry a write that
 * reported "want write" from a different address, with the same bytes. A
 * server that parks a slow reader copies the unsent tail of its response
 * into storage of its own and retries from there on another thread. Without
 * this mode OpenSSL refuses that retry with "bad write retry", because by
 * default it insists on the very same buffer pointer.
 *
 * The session id context is the one setting that resumption depends on. A
 * server context with SSL_VERIFY_PEER, which is every context with a trust
 * store, refuses to resume a session when no session id context is set.
 * OpenSSL then fails the whole handshake with "session id context
 * uninitialized", and it does not fall back to a full handshake. Every client
 * that offers a cached session, which is every browser, curl, and a proxy
 * that reuses sessions, then fails on its second connection. */
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
    X509_free(cert);
    return ok && SSL_CTX_check_private_key(ctx) == 1;
  }
  if (!cert_pem || !key_pem) return true; /* no cert configured. Not an error */

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
            /* An intermediate certificate that does not attach silently
             * hands every client an incomplete chain. The trust-bundle loop
             * below is different: it holds many independent trust anchors,
             * and the loss of one is a tolerable degradation. The chain of
             * this server is one sequence, and every part of it carries
             * load. Every failure here therefore fails the whole
             * application of the certificate. */
            cert_ok = false;
          }
        }
      }
      sk_X509_INFO_pop_free(inf, X509_INFO_free);
    }
    BIO_free(cbio);
  }
  /* Neither install call answers "do these two belong together" on its own.
   * SSL_CTX_use_PrivateKey can cross-check only against a certificate that
   * is already installed. SSL_CTX_use_certificate answers a mismatch in
   * another way: it discards the installed key, clears the error queue and
   * reports success. Without this check, the module builds and commits a
   * context that carries a pair that does not match. Every handshake that
   * the context serves then fails with no certificate and no diagnostic.
   * ctls_ctx_cert_add rejects such a pair when it accepts the input, so no
   * current path arrives here with one. This check keeps a built SSL_CTX
   * free of such a pair, whatever way a pair arrives. */
  return cert_ok && SSL_CTX_check_private_key(ctx) == 1;
}

/* Installs the trust store and the verify mode on one SSL_CTX. Every SSL_CTX
 * that the module builds for one ctls_ctx_t, the default one and each one of
 * a named certificate, goes through this function, so an SNI dispatch never
 * moves a connection onto a context with a weaker mode.
 *
 * SSL_VERIFY_FAIL_IF_NO_PEER_CERT makes a server refuse a client that sends
 * no certificate. Plain SSL_VERIFY_PEER only asks for one, and it lets a
 * client that sends nothing through, because there is nothing to verify.
 * OpenSSL ignores the flag in client mode, where a server always presents a
 * certificate that the chain check then verifies.
 *
 * A certificate revocation list in a trusted bundle is enforced. When any
 * bundle holds one, the store gets X509_V_FLAG_CRL_CHECK and
 * X509_V_FLAG_CRL_CHECK_ALL, as the ssl_crl directive of nginx does: every
 * certificate of the chain of the peer, its trust anchor included, must then
 * have a CRL of its issuer in the store, a revoked certificate fails the
 * handshake, and so does a chain that a missing or expired CRL leaves
 * unchecked. The decision follows the presence of a CRL in a bundle, and not
 * the success of adding it, so a CRL that fails to load fails closed. */
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
  if (tls->verify_default_store && SSL_CTX_set_default_verify_paths(ctx) != 1)
    return false;
  bool any_crl = false;
  for (size_t i = 0; i < tls->trust_count; ++i) {
    BIO *bio = BIO_new_mem_buf(tls->trust_pems[i], (int)tls->trust_lens[i]);
    if (!bio) return false;
    STACK_OF(X509_INFO) *inf = PEM_X509_INFO_read_bio(bio, NULL, NULL, NULL);
    if (inf) {
      for (int j = 0; j < sk_X509_INFO_num(inf); ++j) {
        X509_INFO *tmp = sk_X509_INFO_value(inf, j);
        if (tmp->x509) X509_STORE_add_cert(store, tmp->x509);
        if (tmp->crl) {
          any_crl = true;
          X509_STORE_add_crl(store, tmp->crl);
        }
      }
      sk_X509_INFO_pop_free(inf, X509_INFO_free);
    }
    BIO_free(bio);
  }
  if (any_crl &&
      X509_STORE_set_flags(
          store, X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL) != 1)
    return false;
  return true;
}

/* The synchronous ALPN selection callback for server mode. on_selected
 * fires directly here, from inside the SSL_accept() or SSL_do_handshake()
 * call that triggered this callback. ctls_conn_handshake_step() already
 * makes that call in a synchronous way. See the doc comment with the design
 * notes at the top of ctls.h. */
static int _ctls_alpn_select_cb(SSL *ssl, const unsigned char **out,
                                unsigned char *outlen, const unsigned char *in,
                                unsigned int inlen, void *arg) {
  ctls_ctx_t *tls = (ctls_ctx_t *)arg;
  ctls_conn_t *conn = (ctls_conn_t *)SSL_get_ex_data(ssl, _ctls_conn_ex_idx());
  /* In practice conn is never NULL. The module installs this callback only
   * on a ctx that it built itself. Also, ctls_conn_create_server() always
   * sets this ex_data before the handshake that can reach this point. The
   * NULL check below is a defence. It exists because there is no safe place
   * to copy a selected protocol name to without conn. See below. It does
   * not exist because a real code path leaves conn unset. */
  if (!conn) return SSL_TLSEXT_ERR_NOACK;

  /* tls->lock guards the read of tls->alpn[] and tls->alpn_count below. The
   * client-side mirror of this code, _ctls_record_client_alpn, locks around
   * its own read of the same fields for the same reason. tls->alpn is
   * mutable storage. A concurrent ctls_ctx_alpn_add() call can
   * _ccol_mem_free() it when it replaces an entry that exists. It can also
   * _ccol_mem_realloc() it when it grows the array. Either one can happen
   * under a handshake that is in progress. That is a documented, supported
   * use case, and not a hypothetical one. See the doc comment of
   * ctls_ctx_retain().
   *
   * This function copies the name, the name_len, the on_selected and the
   * udata of the matched entry into locals before it unlocks. The copy of
   * the NAME BYTES THEMSELVES into conn->alpn_selected_buf is the important
   * one, because a pointer to those bytes is not enough. Two readers depend
   * on this. OpenSSL reads *out from inside this same SSL_accept() call,
   * after this callback returns. A caller can read conn->alpn_selected_name
   * at any later point in the life of conn, long after this handshake and
   * after any number of more ctls_ctx_alpn_add() calls. The copies keep
   * both readers away from memory that the protection of tls->lock already
   * left behind.
   *
   * This function calls on_selected only after it unlocks. The discipline of
   * _ctls_record_client_alpn is the same. An application callback can
   * re-enter this same ctx, for example with its own ctls_ctx_alpn_add()
   * call. Such a callback must not deadlock against a lock that this
   * function does not need at that point. */
  ccol_mutex_lock(tls->lock);
  if (tls->alpn_count == 0) {
    ccol_mutex_unlock(tls->lock);
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
    /* This ctx holds none of the protocols that the peer offered, so the two
     * sides agree on no protocol. The negotiation on the wire reports NOACK,
     * the module records nothing on conn, and no on_selected callback fires.
     * A registered protocol name here tells a caller that the two sides
     * settled on a protocol that the peer never acknowledged. A caller that
     * picks its framing on the wire from that report speaks a protocol that
     * the peer does not speak. */
    ccol_mutex_unlock(tls->lock);
    return SSL_TLSEXT_ERR_NOACK;
  }

  memcpy(conn->alpn_selected_buf, matched->name, matched->name_len);
  conn->alpn_selected_buf[matched->name_len] = '\0';
  conn->alpn_selected_name = conn->alpn_selected_buf;
  conn->alpn_selected_len = matched->name_len;
  ctls_alpn_selected_fn cb = matched->on_selected;
  size_t name_len = matched->name_len;
  void *udata = matched->udata;
  ccol_mutex_unlock(tls->lock);

  *out = (const unsigned char *)conn->alpn_selected_buf;
  *outlen = (unsigned char)name_len;
  if (cb) cb(conn, conn->alpn_selected_buf, name_len, udata);
  return SSL_TLSEXT_ERR_OK;
}

static bool _ctls_apply_alpn(SSL_CTX *ctx, ctls_ctx_t *tls) {
  if (tls->alpn_count == 0) return true;
  size_t wire_len = 0;
  for (size_t i = 0; i < tls->alpn_count; ++i)
    wire_len += tls->alpn[i].name_len + 1;
  unsigned char *wire =
      (unsigned char *)_ccol_mem_alloc(tls->m_procs, wire_len);
  if (!wire) return false;
  size_t pos = 0;
  for (size_t i = 0; i < tls->alpn_count; ++i) {
    wire[pos++] = (unsigned char)tls->alpn[i].name_len;
    memcpy(wire + pos, tls->alpn[i].name, tls->alpn[i].name_len);
    pos += tls->alpn[i].name_len;
  }
  /* SSL_CTX_set_alpn_protos builds the offer list for client mode. It copies
   * its input internally, so wire does not need to live longer than this
   * call. */
  SSL_CTX_set_alpn_protos(ctx, wire, (unsigned int)wire_len);
  _ccol_mem_free(tls->m_procs, wire);
  SSL_CTX_set_alpn_select_cb(ctx, _ctls_alpn_select_cb, tls);
  return true;
}

/* Looks one registered pattern up by its exact spelling. The caller gives
 * that spelling in lower case. Returns the built SSL_CTX of that entry.
 * Gives NULL if the pattern is not registered. */
static SSL_CTX *_ctls_named_ctx_for_pattern(ctls_ctx_t *tls, char *pattern) {
  cmap_pair kp = {.ptr = pattern, .size = strlen(pattern) + 1};
  const cmap_pair *vp = NULL;
  if (chmap_get_elem_ref(tls->named_certs, &kp, &vp) != ccol_success)
    return NULL;
  ctls_named_cert *nc;
  memcpy(&nc, vp->ptr, sizeof(nc));
  return nc->built_ctx;
}

/* Resolves the SNI name of a ClientHello to the SSL_CTX of a named
 * certificate. The function tries an exact match first. It tries a leading
 * "*." wildcard of one label only when no exact entry exists. This order of
 * precedence makes the answer a property of the name. Without the order, the
 * answer is a property of the order of the ctls_ctx_cert_add calls. Both
 * "foo.example.com" and "*.example.com" describe a request for
 * "foo.example.com". Without a rank between them, a host that asks for a
 * specific name gets the wildcard certificate. That throws away everything
 * that makes the specific certificate special: its own key, its own chain,
 * and a leaf that a client pins.
 *
 * Each of the two halves is one direct probe, and not a scan. At most one
 * registered pattern can match a given name as a wildcard, because the "*."
 * form covers exactly one label. The only candidate is "*." plus everything
 * after the first dot of the name. There is therefore nothing to rank among
 * the wildcards either. */
static SSL_CTX *_ctls_find_named_ctx(ctls_ctx_t *tls, const char *sni_name) {
  if (!tls->named_certs || !sni_name || !*sni_name) return NULL;
  char *lower = _ctls_strdup_lower(tls->m_procs, sni_name);
  if (!lower) return NULL;
  SSL_CTX *found = _ctls_named_ctx_for_pattern(tls, lower);
  if (!found) {
    char *dot = strchr(lower, '.');
    /* The two bytes immediately before the suffix are the last character of
     * the first label of the name and the dot itself. The exact probe above
     * already consumed both of them. The code therefore writes the wildcard
     * pattern over them in place, and it needs no second allocation. A name
     * whose first label is empty has no wildcard form to build. A name that
     * ends in its only dot has none either. */
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

/* Records the reference of this module on the SSL_CTX that a servername
 * dispatch installed on conn. It replaces a reference that an earlier
 * dispatch on the same connection left behind. OpenSSL runs the servername
 * callback one time for each ClientHello, so a TLS 1.3 HelloRetryRequest
 * exchange reaches this function two times. The caller holds ctx->lock, and
 * this call sits in the same critical section as the lookup and the
 * SSL_set_SSL_CTX() that produced installed. Nothing can therefore rebuild
 * that object away in between.
 *
 * Without this pin, one thing alone holds a dispatched named context alive:
 * the reference that SSL_set_SSL_CTX() takes internally. That reference is
 * the bookkeeping of a third party, and not of this module. One property
 * matters here: every context that one of its live connections can still
 * reach is a context that the module holds a reference on. Without the pin,
 * the module can neither state nor check that property. */
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
  /* This code resolves conn before it takes tls->lock. The order is the same
   * as the order in _ctls_alpn_select_cb. _ctls_conn_ex_idx() has a mutex of
   * its own. This code never takes that mutex while it holds tls->lock,
   * which keeps the two locks independent of each other. */
  ctls_conn_t *conn = (ctls_conn_t *)SSL_get_ex_data(ssl, _ctls_conn_ex_idx());
  const char *name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
  if (!name) return SSL_TLSEXT_ERR_OK;
  /* SSL_set_SSL_CTX() must run in the SAME critical section as the lookup
   * that produced matched. It must not run after the code unlocks tls->lock.
   * matched is a raw SSL_CTX* into the built_ctx field of a named
   * certificate. The doc comment of ctls_ctx_retain() states that a pin of a
   * context against a concurrent change of the configuration on another
   * thread is an explicitly supported use case. Such a change comes from
   * ctls_ctx_cert_add(), from ctls_ctx_trust() and from the other functions
   * that change a context. One example is a rotation of a certificate under
   * live traffic. That use case is not a hypothetical one.
   * _ctls_ctx_rebuild_locked() frees the OLD built_ctx of a rotated named
   * certificate in its own commit step, and its caller always holds
   * tls->lock. An unlock here before the use of matched lets that free
   * happen in the window between the lookup and the use. SSL_set_SSL_CTX()
   * then gets a dangling pointer, which is a real use-after-free on a live
   * handshake and not a theoretical one. The discipline of
   * ctls_conn_create_client and ctls_conn_create_server is the same: each
   * calls SSL_new() on the default context while it still holds tls->lock,
   * for the same reason. */
  ccol_mutex_lock(tls->lock);
  SSL_CTX *matched = _ctls_find_named_ctx(tls, name);
  /* SSL_set_SSL_CTX() reports a failure with a return of NULL. It duplicates
   * the certificate of the new context, and that allocation can fail. After
   * such a failure the connection stays on the context that it already had.
   * A pin of matched in that case holds an object that the connection does
   * not use. After a success, the call returns the context that it
   * installed. */
  if (matched && SSL_set_SSL_CTX(ssl, matched) && conn)
    _ctls_conn_pin_sni_ctx(conn, matched);
  ccol_mutex_unlock(tls->lock);
  return SSL_TLSEXT_ERR_OK;
}

/* Builds one SSL_CTX from the start. It applies the base settings, an
 * optional certificate, the shared trust store and the shared ALPN
 * configuration. The module uses this function for ctx_default and for the
 * SSL_CTX of every named certificate that it dispatches by SNI. Each of
 * those contexts is fully independent and fully configured, so that
 * SSL_set_SSL_CTX() can move onto it as a whole from the servername
 * callback. Returns NULL on a failure. The caller must keep the previous,
 * still valid SSL_CTX pointer of the ctx_t until this function succeeds. */
static SSL_CTX *_ctls_build_one_ctx(ctls_ctx_t *tls, const char *cert_pem,
                                    size_t cert_len, const char *key_pem,
                                    size_t key_len, const char *pk_password,
                                    bool self_signed,
                                    const char *self_signed_name) {
  SSL_CTX *ctx = SSL_CTX_new(TLS_method());
  if (!ctx) return NULL;
  if (!_ctls_apply_base_settings(ctx) ||
      !_ctls_apply_cert(ctx, cert_pem, cert_len, key_pem, key_len, pk_password,
                        self_signed, self_signed_name) ||
      !_ctls_apply_trust(ctx, tls) || !_ctls_apply_alpn(ctx, tls)) {
    SSL_CTX_free(ctx);
    return NULL;
  }
  return ctx;
}

/* Rebuilds ctx_default and the SSL_CTX of every named certificate from the
 * configuration that the context stores now. The module calls this function
 * under tls->lock after every call that changes the context: cert_add,
 * trust, trust_system and alpn_add. The function always rebuilds from the
 * start. It never patches the SSL_CTX objects that exist in place. On a
 * failure it keeps the previous, still valid SSL_CTX objects in place and
 * tears down nothing. A failed change of the configuration therefore never
 * leaves ctx in a worse, unusable state than the state before the call. */
static bool _ctls_ctx_rebuild_locked(ctls_ctx_t *tls) {
  SSL_CTX *new_default = _ctls_build_one_ctx(
      tls, tls->has_default_cert ? tls->default_cert_pem : NULL,
      tls->default_cert_len,
      tls->has_default_cert ? tls->default_key_pem : NULL, tls->default_key_len,
      tls->default_pk_password, tls->default_self_signed,
      tls->default_self_signed_name);
  if (!new_default) return false;

  if (tls->named_certs && chmap_elem_count(tls->named_certs) > 0) {
    SSL_CTX_set_tlsext_servername_callback(new_default, _ctls_servername_cb);
    SSL_CTX_set_tlsext_servername_arg(new_default, tls);
  }

  /* The module rebuilds the SSL_CTX of every named certificate before it
   * commits new_default. A failure in the middle of the rebuild therefore
   * leaves everything consistent. This includes the entries whose new
   * SSL_CTX the module already built. Either every context rebuilds
   * successfully and the module swaps all of them in together, or the
   * module swaps none of them in. */
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
      return false;
    }
    char *err = NULL;
    cmap_iterator *it = chashmap_begin_iter(tls->named_certs, &err);
    /* A named_count above 0 guarantees that tls->named_certs is not empty.
     * This function runs with ctx->lock held, so nothing can change the map
     * after the code reads the count above. A NULL from chashmap_begin_iter
     * here can therefore mean only one thing: the call could not allocate
     * the iterator itself, because there is not enough memory. It cannot
     * mean "there is nothing to iterate". ok must start as false in that
     * case. With ok true at all times, the while loop below correctly never
     * runs, because it is NULL. But the commit loop further down then reads
     * new_named_ctxs and new_named_entries as if they hold full data. The
     * code above allocates both arrays with _ccol_mem_alloc, and not with
     * _ccol_mem_calloc, so they hold no data at all. The commit loop then
     * dereferences garbage pointers. The static analyzer of clang flags that
     * read. No dynamic test can reach it, because such a test must inject an
     * out-of-memory condition exactly inside chashmap_begin_iter, with named
     * SNI certificates configured. */
    size_t i = 0;
    bool ok = (it != NULL);
    while (it && ok) {
      ctls_named_cert *nc;
      memcpy(&nc, it->val_pair->ptr, sizeof(nc));
      SSL_CTX *built = _ctls_build_one_ctx(
          tls, nc->cert_pem, nc->cert_len, nc->key_pem, nc->key_len,
          nc->pk_password, nc->self_signed, nc->self_signed_name);
      if (!built) {
        ok = false;
        break;
      }
      new_named_ctxs[i] = built;
      new_named_entries[i] = nc;
      ++i;
      it = it->_next_fn(it);
    }
    if (it) ccol_iter_destroy(it);
    if (!ok) {
      for (size_t j = 0; j < i; ++j) SSL_CTX_free(new_named_ctxs[j]);
      _ccol_mem_free(tls->m_procs, new_named_ctxs);
      _ccol_mem_free(tls->m_procs, new_named_entries);
      SSL_CTX_free(new_default);
      return false;
    }
  }

  /* Commit: free the old contexts, install the new ones. */
  if (tls->ctx_default) SSL_CTX_free(tls->ctx_default);
  tls->ctx_default = new_default;
  for (size_t i = 0; i < named_count; ++i) {
    if (new_named_entries[i]->built_ctx)
      SSL_CTX_free(new_named_entries[i]->built_ctx);
    new_named_entries[i]->built_ctx = new_named_ctxs[i];
  }
  _ccol_mem_free(tls->m_procs, new_named_ctxs);
  _ccol_mem_free(tls->m_procs, new_named_entries);
  return true;
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
 * The pair is usable only if the key matches the public key of the leaf
 * certificate that _ctls_apply_cert installs later. That leaf is the first
 * entry of the certificate stream. The other entries are chain
 * certificates, and they carry no private key of their own.
 *
 * The module makes this check when it accepts the input, before the pair
 * reaches the stored configuration. It does not make the check from the
 * rebuild step. A pair that the module commits, and only then finds bad,
 * stays registered. Every later rebuild that this context runs for an
 * unrelated reason then fails on that pair again. Such a rebuild can come
 * from another certificate, another trust bundle or another ALPN
 * protocol. */
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
    /* A key that the module cannot read leaves the reason on the error queue
     * of the thread. Such a key is malformed PEM, or an encrypted key with a
     * wrong password or with no password. The code clears that queue.
     * Without this, a later SSL_get_error classification of an unrelated
     * connection reports this failure of the configuration. */
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
  if (ccol_mutex_init(tls->lock) != 0) {
    _ccol_mem_free(mp, tls->m_procs);
    _ccol_mem_free(mp, tls);
    if (err_str) *err_str = "ctls_ctx_new: failed to initialize mutex";
    return NULL;
  }
  char *err = NULL;
  tls->named_certs =
      chmap_create_mp(16, ccol_string, ccol_pointer, tls->m_procs, &err);
  if (!tls->named_certs) {
    ccol_mutex_destroy(tls->lock);
    _ccol_mem_free(mp, tls->m_procs);
    _ccol_mem_free(mp, tls);
    if (err_str) *err_str = "ctls_ctx_new: chmap allocation failure";
    return NULL;
  }
  if (!_ctls_ctx_rebuild_locked(tls)) {
    __chmap_destroy(tls->named_certs);
    ccol_mutex_destroy(tls->lock);
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
  /* The module needs neither file when it generates a self-signed
   * certificate. This covers two cases. The first is a named self-signed
   * certificate, which the module dispatches by SNI, and there server_name
   * gives the subject CN. The second is a self-signed DEFAULT certificate,
   * where server_name is NULL or "". The code below then falls back to a
   * generic subject name. It does this because a self-signed certificate
   * always needs a subject, but the default slot carries no name of its own
   * to take one from. */
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
    /* A certificate and a key that do not belong together are a failure of
     * the load. They are not a usable configuration. The code rejects the
     * pair here, before it stores anything, so ctx continues to serve what
     * it served before this call. Without this check, the module accepts the
     * pair and the rebuild commits it. Every handshake after that fails,
     * because the module has no certificate to present. For a named SNI
     * entry only that one hostname breaks, while the context looks healthy.
     * An update of one of the two paths that does not update the other is
     * the ordinary way that a rotation goes wrong. */
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
    if (pk_password) {
      pw_copy = _ctls_strdup(ctx->m_procs, pk_password);
      if (!pw_copy) {
        _ccol_mem_free(ctx->m_procs, cert_pem);
        _ccol_mem_free(ctx->m_procs, key_pem);
        return ccol_not_enough_memory;
      }
    }
  }

  ccol_mutex_lock(ctx->lock);
  ccol_retval_t rv = ccol_success;
  if (!have_name) {
    /* Replace the default certificate. */
    char *self_signed_name = NULL;
    if (self_signed) {
      /* The code uses a generic, fixed subject name when the caller gives
       * none. The default slot has no name of its own. A named entry is
       * different, because its server_name is also the subject of the
       * self-signed certificate. This name is good enough for the common
       * use case, which is a server that runs with no certificate
       * configuration at all. A caller that depends on hostname
       * verification against this certificate must configure a real name in
       * any case. */
      self_signed_name = _ctls_strdup(ctx->m_procs, "ctls-default");
      if (!self_signed_name) {
        _ccol_mem_free(ctx->m_procs, cert_pem);
        _ccol_mem_free(ctx->m_procs, key_pem);
        _ccol_mem_free(ctx->m_procs, pw_copy);
        ccol_mutex_unlock(ctx->lock);
        return ccol_not_enough_memory;
      }
    }
    _ccol_mem_free(ctx->m_procs, ctx->default_cert_pem);
    _ccol_mem_free(ctx->m_procs, ctx->default_key_pem);
    _ccol_mem_free(ctx->m_procs, ctx->default_pk_password);
    _ccol_mem_free(ctx->m_procs, ctx->default_self_signed_name);
    ctx->default_cert_pem = cert_pem;
    ctx->default_cert_len = cert_len;
    ctx->default_key_pem = key_pem;
    ctx->default_key_len = key_len;
    ctx->default_pk_password = pw_copy;
    ctx->default_self_signed = self_signed;
    ctx->default_self_signed_name = self_signed_name;
    ctx->has_default_cert = true;
  } else {
    char *lower_name = _ctls_strdup_lower(ctx->m_procs, server_name);
    if (!lower_name) {
      _ccol_mem_free(ctx->m_procs, cert_pem);
      _ccol_mem_free(ctx->m_procs, key_pem);
      _ccol_mem_free(ctx->m_procs, pw_copy);
      ccol_mutex_unlock(ctx->lock);
      return ccol_not_enough_memory;
    }
    ctls_named_cert *nc =
        (ctls_named_cert *)_ccol_mem_calloc(ctx->m_procs, 1, sizeof(*nc));
    if (!nc) {
      _ccol_mem_free(ctx->m_procs, lower_name);
      _ccol_mem_free(ctx->m_procs, cert_pem);
      _ccol_mem_free(ctx->m_procs, key_pem);
      _ccol_mem_free(ctx->m_procs, pw_copy);
      ccol_mutex_unlock(ctx->lock);
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
        /* The code must check this failure, as it checks every other
         * allocation in this function. Without the check,
         * nc->self_signed_name stays NULL while nc->self_signed stays true.
         * No code below this point tests self_signed_name for NULL before
         * it uses the name. _ctls_ctx_rebuild_locked calls
         * _ctls_build_one_ctx, which then calls
         * _ctls_create_self_signed(NULL). That function always calls
         * strlen(server_name). The result is a real crash under continuous
         * memory pressure. The documented, graceful answer is
         * ccol_not_enough_memory, which every other allocation failure in
         * this function already reports. At this point nothing can reach nc
         * from ctx->named_certs, because the insert into the map happens
         * further below. _ctls_named_cert_destroy(ctx, nc) here is
         * therefore a clean, complete unwind. It also frees cert_pem,
         * key_pem and pw_copy, which nc owns from the assignments above. */
        _ctls_named_cert_destroy(ctx, nc);
        _ccol_mem_free(ctx->m_procs, lower_name);
        ccol_mutex_unlock(ctx->lock);
        return ccol_not_enough_memory;
      }
    }

    cmap_pair kp = {.ptr = lower_name, .size = strlen(lower_name) + 1};
    const cmap_pair *old_vp = NULL;
    ctls_named_cert *old_nc = NULL;
    /* The code takes only the pointer VALUE here. old_vp itself points into
     * the storage of the map, and the insert below can move that storage. */
    if (chmap_get_elem_ref(ctx->named_certs, &kp, &old_vp) == ccol_success)
      memcpy(&old_nc, old_vp->ptr, sizeof(old_nc));
    cmap_pair vp = {.ptr = &nc, .size = sizeof(nc)};
    rv = chmap_insert_elem(ctx->named_certs, &kp, &vp);
    _ccol_mem_free(ctx->m_procs, lower_name);
    /* ccol_key_already_present means that the map updated the value slot of
     * this hostname to point at nc. The entry already existed, for example
     * because this call rotates the certificate of a name that an earlier
     * call added. This code is not a failure. The function must not destroy
     * nc here, because that leaves the map with a dangling pointer to nc. */
    if (rv != ccol_success && rv != ccol_key_already_present) {
      _ctls_named_cert_destroy(ctx, nc);
      ccol_mutex_unlock(ctx->lock);
      return rv;
    }
    /* The code frees the entry that it replaces only after the map points at
     * the new entry. A free before that leaves the key present with a value
     * that points at freed memory, on every path where the insert then
     * fails. The next rebuild, the next SNI lookup and ctls_ctx_release each
     * dereference that value and free it again. No current path depends on
     * this order, because the map updates the value of a key that is already
     * present in place. For a value of the size of a pointer that update
     * allocates nothing and cannot fail. This order is therefore an
     * invariant that the code keeps, and not a live failure that it
     * prevents. */
    if (old_nc) _ctls_named_cert_destroy(ctx, old_nc);
  }

  bool built = _ctls_ctx_rebuild_locked(ctx);
  ccol_mutex_unlock(ctx->lock);
  if (!built) {
    if (err_str) *err_str = "ctls_ctx_cert_add: SSL_CTX rebuild failure";
    return ccol_http_tls_cert_load_failed;
  }
  return ccol_success;
}

/* Reports whether a PEM stream holds at least one certificate. Some streams
 * carry none: an empty file, a private key on its own, or a text file that is
 * not PEM at all. OpenSSL parses such a stream into an empty set, and it
 * reports no error. The module must therefore check the content of a bundle
 * that an operator gives it, and not only whether the bundle parses. A bundle
 * that contributes nothing still switches peer verification on. It leaves a
 * store that trusts no issuer. A client then rejects every peer that it talks
 * to. A server keeps accepting every client that presents no certificate at
 * all, because its peer verification asks for a certificate but does not need
 * one. The mutual TLS that the operator configured the bundle to enforce is
 * then silently absent. Only certificates count here. A CRL carries no trust
 * anchor, so a stream of nothing but CRLs names no issuer that a peer could
 * chain to, and it is refused like an empty one. A bundle that holds both
 * certificates and CRLs is accepted, and its CRLs are enforced; see
 * _ctls_apply_trust. */
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
  /* The code makes this check here, before it commits the entry below. It
   * does not make the check from the rebuild step. A bundle that reaches
   * ctx->trust_pems and only then fails stays registered. Every later
   * rebuild that this context runs for an unrelated reason then fails on
   * that bundle again. Such a rebuild can come from another certificate or
   * from another ALPN protocol. */
  if (!_ctls_pem_has_certificate(pem, len)) {
    _ccol_mem_free(ctx->m_procs, pem);
    if (err_str) {
      *err_str = "ctls_ctx_trust: CA bundle contains no certificates";
    }
    return ccol_http_tls_cert_load_failed;
  }

  ccol_mutex_lock(ctx->lock);
  if (ctx->trust_count == ctx->trust_cap) {
    size_t new_cap = ctx->trust_cap ? ctx->trust_cap * 2 : 4;
    /* The code commits the new pointer to ctx->trust_pems immediately. It
     * does not wait for the check of the second realloc below. A successful
     * realloc can move the block, which frees the old one. It can also
     * extend the original allocation. Both outcomes make the copy of that
     * pointer in ctx->trust_pems invalid. A commit that waits for both
     * reallocs to succeed leaves ctx->trust_pems dangling every time this
     * first call succeeds and moves the block while the second call fails.
     * That is a real use-after-free on the next access of this ctx to
     * trust_pems. It also leaks new_pems itself, and the static analyzer of
     * clang flags that leak. */
    char **new_pems = (char **)_ccol_mem_realloc(ctx->m_procs, ctx->trust_pems,
                                                 new_cap * sizeof(char *));
    if (!new_pems) {
      _ccol_mem_free(ctx->m_procs, pem);
      ccol_mutex_unlock(ctx->lock);
      return ccol_not_enough_memory;
    }
    ctx->trust_pems = new_pems;
    size_t *new_lens = (size_t *)_ccol_mem_realloc(
        ctx->m_procs, ctx->trust_lens, new_cap * sizeof(size_t));
    if (!new_lens) {
      _ccol_mem_free(ctx->m_procs, pem);
      ccol_mutex_unlock(ctx->lock);
      return ccol_not_enough_memory;
    }
    ctx->trust_lens = new_lens;
    ctx->trust_cap = new_cap;
  }
  ctx->trust_pems[ctx->trust_count] = pem;
  ctx->trust_lens[ctx->trust_count] = len;
  ctx->trust_count++;
  ctx->verify_peer = true;
  bool built = _ctls_ctx_rebuild_locked(ctx);
  ccol_mutex_unlock(ctx->lock);
  if (!built) {
    if (err_str) *err_str = "ctls_ctx_trust: SSL_CTX rebuild failure";
    return ccol_http_tls_cert_load_failed;
  }
  return ccol_success;
}

ccol_retval_t ctls_ctx_trust_system(ctls_ctx_t *ctx) {
  if (!ctx) return ccol_invalid_args;
  ccol_mutex_lock(ctx->lock);
  ctx->verify_default_store = true;
  ctx->verify_peer = true;
  bool built = _ctls_ctx_rebuild_locked(ctx);
  ccol_mutex_unlock(ctx->lock);
  return built ? ccol_success : ccol_http_tls_cert_load_failed;
}

ccol_retval_t ctls_ctx_peer_cert_optional(ctls_ctx_t *ctx, bool optional) {
  if (!ctx) return ccol_invalid_args;
  ccol_mutex_lock(ctx->lock);
  if (ctx->peer_cert_optional == optional) {
    ccol_mutex_unlock(ctx->lock);
    return ccol_success;
  }
  bool previous = ctx->peer_cert_optional;
  ctx->peer_cert_optional = optional;
  /* A context with no trust store carries no verify mode, so there is
   * nothing to rebuild yet; the first trust call builds with this flag. */
  bool built = (ctx->trust_count == 0 && !ctx->verify_default_store) ||
               _ctls_ctx_rebuild_locked(ctx);
  if (!built) ctx->peer_cert_optional = previous;
  ccol_mutex_unlock(ctx->lock);
  return built ? ccol_success : ccol_http_tls_cert_load_failed;
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

  ccol_mutex_lock(ctx->lock);
  /* Replace a registration that exists with the same name. The code does not
   * add a duplicate entry. */
  for (size_t i = 0; i < ctx->alpn_count; ++i) {
    if (ctx->alpn[i].name_len == name_len &&
        memcmp(ctx->alpn[i].name, name_copy, name_len) == 0) {
      /* The cleanup of the replaced registration runs after ctx->lock is
       * released. A cleanup that calls back into this module on the same
       * context, for example ctls_ctx_retain() or ctls_ctx_release(), would
       * otherwise lock a mutex that this thread already holds. */
      ctls_alpn_cleanup_fn old_cleanup = ctx->alpn[i].on_cleanup;
      void *old_udata = ctx->alpn[i].udata;
      _ccol_mem_free(ctx->m_procs, ctx->alpn[i].name);
      ctx->alpn[i] = (ctls_alpn_entry){.name = name_copy,
                                       .name_len = name_len,
                                       .on_selected = on_selected,
                                       .udata = alpn_udata,
                                       .on_cleanup = on_cleanup};
      bool built = _ctls_ctx_rebuild_locked(ctx);
      ccol_mutex_unlock(ctx->lock);
      if (old_cleanup) old_cleanup(old_udata);
      if (!built) {
        if (err_str) *err_str = "ctls_ctx_alpn_add: SSL_CTX rebuild failure";
        return ccol_http_tls_cert_load_failed;
      }
      return ccol_success;
    }
  }
  if (ctx->alpn_count == ctx->alpn_cap) {
    size_t new_cap = ctx->alpn_cap ? ctx->alpn_cap * 2 : 4;
    ctls_alpn_entry *new_alpn = (ctls_alpn_entry *)_ccol_mem_realloc(
        ctx->m_procs, ctx->alpn, new_cap * sizeof(ctls_alpn_entry));
    if (!new_alpn) {
      _ccol_mem_free(ctx->m_procs, name_copy);
      ccol_mutex_unlock(ctx->lock);
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
  bool built = _ctls_ctx_rebuild_locked(ctx);
  ccol_mutex_unlock(ctx->lock);
  if (!built) {
    if (err_str) *err_str = "ctls_ctx_alpn_add: SSL_CTX rebuild failure";
    return ccol_http_tls_cert_load_failed;
  }
  return ccol_success;
}

size_t ctls_ctx_alpn_count(const ctls_ctx_t *ctx) {
  return ctx ? ctx->alpn_count : 0;
}

void ctls_ctx_retain(ctls_ctx_t *ctx) {
  if (!ctx) return;
  ccol_mutex_lock(ctx->lock);
  ctx->ref++;
  ccol_mutex_unlock(ctx->lock);
}

void ctls_ctx_release(ctls_ctx_t *ctx) {
  if (!ctx) return;
  ccol_mutex_lock(ctx->lock);
  size_t remaining = --ctx->ref;
  ccol_mutex_unlock(ctx->lock);
  if (remaining > 0) return;

  if (ctx->ctx_default) SSL_CTX_free(ctx->ctx_default);
  _ccol_mem_free(ctx->m_procs, ctx->default_cert_pem);
  _ccol_mem_free(ctx->m_procs, ctx->default_key_pem);
  _ccol_mem_free(ctx->m_procs, ctx->default_pk_password);
  _ccol_mem_free(ctx->m_procs, ctx->default_self_signed_name);

  if (ctx->named_certs) {
    char *err = NULL;
    cmap_iterator *it = chashmap_begin_iter(ctx->named_certs, &err);
    while (it) {
      ctls_named_cert *nc;
      memcpy(&nc, it->val_pair->ptr, sizeof(nc));
      _ctls_named_cert_destroy(ctx, nc);
      it = it->_next_fn(it);
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

  ccol_mutex_destroy(ctx->lock);
  /* ctx->m_procs, when it is not NULL, is a heap copy of the procs of the
   * caller. The code frees it through the free() function pointer inside it.
   * The code must therefore free ctx FIRST, while mp is still a live object
   * that it can dereference. A free of mp before ctx leaves mp dangling for
   * the second _ccol_mem_free() call. That call then reads mp->free from
   * memory that is already free, and valgrind reports a use-after-free. */
  ccol_memmgmt_procs_t *mp = ctx->m_procs;
  _ccol_mem_free(mp, ctx);
  _ccol_mem_free(mp, mp);
}

/* ========================================================================== */
/*                                CONNECTIONS                                 */
/* ========================================================================== */

/* Releases the SSL* of the connection and every SSL_CTX reference that the
 * connection holds. All of this happens in one ctx->lock critical section.
 *
 * The lock orders the last reads of this connection through a shared SSL_CTX
 * against the destruction of that context. The commit step of
 * _ctls_ctx_rebuild_locked frees every replaced context under the same lock.
 * A thread that changes the configuration therefore cannot reach the free
 * that destroys an object until every connection that still used it passes
 * through here. A drop of these references outside the lock leaves that
 * order to the reference counter of OpenSSL alone. That counter is an atomic
 * inside a library that nothing instruments, and ThreadSanitizer cannot see
 * it. The destroy then reads as a race against the earlier reads of this
 * connection through the same object.
 *
 * SSL_shutdown() is deliberately NOT part of this function. It writes a
 * close_notify to the peer. A ctx->lock that a thread holds across that write
 * blocks three things on the socket of one peer. Those are the creation of
 * every other connection, the teardown of every other connection, and every
 * change of the configuration. SSL_free()
 * itself does no I/O. It drops the two references that SSL_new() took, which
 * are the reference on the active context of the connection and the
 * reference on its session context. It then frees the state of the
 * connection. */
static void _ctls_conn_release_ssl(ctls_conn_t *conn) {
  ctls_ctx_t *ctx = conn->ctx;
  ccol_mutex_lock(ctx->lock);
  if (conn->ssl) {
    SSL_free(conn->ssl);
    conn->ssl = NULL;
  }
  /* SSL_free() above freed the BIO, so its count on the method can go. The
   * release itself runs after ctx->lock is dropped. */
  bool release_sock_bio = conn->holds_sock_bio;
  conn->holds_sock_bio = false;
  /* SSL_CTX_free() does nothing on NULL, and the documents say so. Neither
   * pin therefore needs a guard of its own. The accounting does need one,
   * because the count must not record a release of a pin that the module
   * never took. */
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
  ccol_mutex_unlock(ctx->lock);
  if (release_sock_bio) _ctls_sock_bio_release();
}

/* Unwinds a connection that failed part way through its own construction. It
 * covers everything that _ctls_conn_release_ssl covers. It also covers the
 * ctls_ctx_t reference that _ctls_conn_alloc took, and the connection record
 * itself. The code reads m_procs before it releases the reference on the
 * context. That release can be the last one and free the context, and a
 * later read of m_procs then reads memory that is already free. */
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
  /* Verification of the name of the peer needs a name to verify against. A
   * request for it with no name must fail here, and must not quietly connect
   * with no check of the name at all. A caller that genuinely wants no check
   * of the name says so with a verify_host of false. */
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

  ccol_mutex_lock(ctx->lock);
  SSL *ssl = SSL_new(ctx->ctx_default);
  if (ssl) {
    /* See the doc comment about the pins of struct ctls_conn. The code takes
     * this pin under ctx->lock, in the same critical section as the
     * SSL_new() call that reads ctx->ctx_default. The pin therefore always
     * names the object that the SSL* of this connection was created against.
     * The code publishes conn->ssl here, and not at the end of this
     * function. Every failure path below then unwinds through one helper
     * that already knows how to release it. Nothing else can reach conn
     * until this function returns it. */
    SSL_CTX_up_ref(ctx->ctx_default);
    conn->pinned_ctx_default = ctx->ctx_default;
    _ctls_account_ctx_pins(1);
    conn->ssl = ssl;
  }
  ccol_mutex_unlock(ctx->lock);
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
  BIO_up_ref(bio);
  SSL_set0_rbio(ssl, bio);
  SSL_set0_wbio(ssl, bio);
  conn->holds_sock_bio = true;

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
        /* The SSL_free() inside the helper also frees the one bio reference
         * that SSL_set0_rbio and SSL_set0_wbio each took. */
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

  ccol_mutex_lock(ctx->lock);
  SSL *ssl = SSL_new(ctx->ctx_default);
  if (ssl) {
    /* See the doc comment about the pins of struct ctls_conn. The code takes
     * this pin under ctx->lock, in the same critical section as the
     * SSL_new() call that reads ctx->ctx_default. The pin therefore always
     * names the object that the SSL* of this connection was created against.
     * The code publishes conn->ssl here, and not at the end of this
     * function. Every failure path below then unwinds through one helper
     * that already knows how to release it. Nothing else can reach conn
     * until this function returns it. */
    SSL_CTX_up_ref(ctx->ctx_default);
    conn->pinned_ctx_default = ctx->ctx_default;
    _ctls_account_ctx_pins(1);
    conn->ssl = ssl;
  }
  ccol_mutex_unlock(ctx->lock);
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
  BIO_up_ref(bio);
  SSL_set0_rbio(ssl, bio);
  SSL_set0_wbio(ssl, bio);
  conn->holds_sock_bio = true;
  SSL_set_accept_state(ssl);
  return conn;
}

static void _ctls_record_client_alpn(ctls_conn_t *conn) {
  ctls_ctx_t *tls = conn->ctx;
  const unsigned char *proto = NULL;
  unsigned int proto_len = 0;
  SSL_get0_alpn_selected(conn->ssl, &proto, &proto_len);
  ccol_mutex_lock(tls->lock);
  if (tls->alpn_count == 0) {
    ccol_mutex_unlock(tls->lock);
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
    /* The server acknowledged no protocol. It sent no ALPN extension, or it
     * named a protocol that this context never registered. The two sides
     * therefore agree on no protocol. The module records nothing on conn,
     * and no on_selected callback fires. A registered protocol name here
     * tells a client that offered {"h2", "http/1.1"} that it agreed on "h2"
     * with a peer that never answered. That client then speaks HTTP/2
     * framing to an HTTP/1.1 server. */
    ccol_mutex_unlock(tls->lock);
    return;
  }
  /* The code copies the name BYTES into the buffer that conn owns. A pointer
   * to matched->name is not enough. _ctls_alpn_select_cb, the server-side
   * mirror of this code, copies the bytes for the same reason. tls->alpn is
   * mutable storage. A concurrent ctls_ctx_alpn_add() call can free it or
   * reallocate it. A caller can read conn->alpn_selected_name at a later
   * point in the life of conn, long after this handshake completes and after
   * the code unlocks this lock. */
  memcpy(conn->alpn_selected_buf, matched->name, matched->name_len);
  conn->alpn_selected_buf[matched->name_len] = '\0';
  conn->alpn_selected_name = conn->alpn_selected_buf;
  conn->alpn_selected_len = matched->name_len;
  ctls_alpn_selected_fn cb = matched->on_selected;
  size_t name_len = matched->name_len;
  void *udata = matched->udata;
  ccol_mutex_unlock(tls->lock);
  if (cb) cb(conn, conn->alpn_selected_buf, name_len, udata);
}

ctls_handshake_result_t ctls_conn_handshake_step(ctls_conn_t *conn) {
  if (!conn || !conn->ssl) return CTLS_HANDSHAKE_ERROR;
  ERR_clear_error();
  /* SSL_accept and SSL_connect: see the @note of this function in ctls.h. It
   * describes a known ThreadSanitizer race that belongs to a third party
   * alone. The race is inside X509_NAME_cmp and X509_cmp of OpenSSL, and it
   * appears when many concurrent client handshakes verify certificates
   * against one shared trust store. This call site cannot fix it without a
   * serial order over handshakes that are meant to run at the same time. */
  int ri = conn->is_server ? SSL_accept(conn->ssl) : SSL_connect(conn->ssl);
  if (ri == 1) {
    conn->handshake_done = true;
    if (!conn->is_server) _ctls_record_client_alpn(conn);
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

/* The shared classification logic for the results of SSL_read and SSL_write.
 * This function observes three points of hardening, and it does so
 * deliberately. SSL_ERROR_SYSCALL must not become an EWOULDBLOCK that makes
 * the caller spin. SSL_ERROR_SSL is a fatal problem in the record layer, and
 * it is not the same as a clean close. ERR_clear_error() must run before
 * every SSL_get_error() classification, because in this codebase the TLS I/O
 * of many unrelated connections shares a small set of threads.
 *
 * The function also records on conn the direction that OpenSSL reported,
 * which is SSL_ERROR_WANT_READ or SSL_ERROR_WANT_WRITE. It records that
 * direction every time it classifies a result as -1 with EWOULDBLOCK. See
 * the doc comment of ctls_conn_wants_write() for the reason that a caller
 * cannot assume that the direction matches the function it called, which is
 * ctls_conn_read() or ctls_conn_write(). */
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
      /* SSL_ERROR_WANT_READ and every other outcome that is not fatal. This
       * is the default that assumes a read. */
      conn->last_io_wants_write = false;
      errno = EWOULDBLOCK;
      return -1;
  }
}

/* SSL_read and SSL_write take an int length. The two entry points below take
 * a size_t, because that is what a caller of a byte-stream API has. A plain
 * cast is wrong for any length above INT_MAX: it either truncates to a
 * smaller positive value, which silently moves fewer bytes than the caller
 * asked for and reports that count as a success, or it produces a negative
 * value, which OpenSSL rejects outright.
 *
 * The clamp turns both into an ordinary SHORT transfer instead. A short
 * transfer is already part of the contract of these two functions, because
 * SSL_MODE_ENABLE_PARTIAL_WRITE is on (see _ctls_apply_base_settings) and a
 * read returns whatever one record holds. Every caller in this library
 * therefore already loops on the returned count. chttp1_stream_read and
 * chttp1_stream_write are those callers.
 *
 * A length above INT_MAX is reachable. A streaming response handler passes
 * chttpsvr_resp_write a buffer of its own choosing, and the write loop of
 * _send_response hands the remaining length straight through. */
static inline int _ctls_clamp_io_len(size_t len) {
  return len > (size_t)INT_MAX ? INT_MAX : (int)len;
}

ssize_t ctls_conn_read(ctls_conn_t *conn, void *buf, size_t len) {
  if (!conn || !conn->ssl) {
    errno = EINVAL;
    return -1;
  }
  ERR_clear_error();
  int ret = SSL_read(conn->ssl, buf, _ctls_clamp_io_len(len));
  return _ctls_classify_io_result(conn, ret);
}

ssize_t ctls_conn_write(ctls_conn_t *conn, const void *buf, size_t len) {
  if (!conn || !conn->ssl) {
    errno = EINVAL;
    return -1;
  }
  /* A write of nothing moves nothing, as write(2) on a socket does. SSL_write
   * answers a length of 0 with a return of 0, which the classification below
   * cannot tell apart from a failure and would report as a reset. */
  if (len == 0) return 0;
  ERR_clear_error();
  int ret = SSL_write(conn->ssl, buf, _ctls_clamp_io_len(len));
  return _ctls_classify_io_result(conn, ret);
}

bool ctls_conn_wants_write(const ctls_conn_t *conn) {
  return conn && conn->last_io_wants_write;
}

/* SSL_pending() counts the decrypted bytes of the current record that no
 * read took yet. SSL_has_pending() also covers record bytes that OpenSSL
 * holds and did not decrypt yet. Either one means that the socket can be
 * empty while a read still has something to give, so poll(2) and epoll(7)
 * see nothing. This module adds no buffer of its own between the socket BIO
 * and OpenSSL, so these two answers are the whole of it. */
bool ctls_conn_has_pending_input(const ctls_conn_t *conn) {
  if (!conn || !conn->ssl) return false;
  return SSL_pending(conn->ssl) > 0 || SSL_has_pending(conn->ssl) == 1;
}

long ctls_conn_verify_result(const ctls_conn_t *conn) {
  if (!conn || !conn->ssl) return -1;
  return SSL_get_verify_result(conn->ssl);
}

/* SSL_get_verify_result() reports X509_V_OK for a connection that verified
 * nothing at all, a connection with no verify mode included. The answer
 * therefore also needs a peer certificate and a verify mode that checked it.
 * A resumed session carries the certificate and the verify result of the
 * handshake that established it. */
bool ctls_conn_peer_cert_verified(const ctls_conn_t *conn) {
  if (!conn || !conn->ssl || !conn->handshake_done) return false;
  return (SSL_get_verify_mode(conn->ssl) & SSL_VERIFY_PEER) != 0 &&
         SSL_get0_peer_certificate(conn->ssl) != NULL &&
         SSL_get_verify_result(conn->ssl) == X509_V_OK;
}

/* Makes the DER encoding and the subject of the verified certificate of the
 * peer, once for each certificate. It returns that certificate, or NULL when
 * the peer has no verified certificate or when an allocation fails; the
 * fields that it could not make stay NULL. */
static const X509 *_ctls_conn_peer_info(ctls_conn_t *conn) {
  if (!ctls_conn_peer_cert_verified(conn)) return NULL;
  X509 *cert = SSL_get0_peer_certificate(conn->ssl);
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
   * character, so the text is printable ASCII with no NUL inside it. */
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
  /* This call runs outside ctx->lock. It writes a close_notify to the peer.
   * A lock that a thread holds across the socket of a peer puts every other
   * connection on this context in a queue behind that socket. */
  /* A connection whose handshake never completed has no session to close.
   * SSL_shutdown() refuses it and leaves an entry on the error queue of this
   * thread, where a later, unrelated call of OpenSSL on the same thread can
   * read it as its own. A close_notify to a peer that is gone fails in the
   * same way, so the queue is cleared after either outcome. */
  if (conn->ssl && conn->handshake_done) SSL_shutdown(conn->ssl);
  ERR_clear_error();
  /* The SSL* and both SSL_CTX pins go together, under ctx->lock. The lock is
   * what orders the last reads of this connection through a shared context
   * against the destruction of that context. See the doc comment of
   * _ctls_conn_release_ssl() for the reason. */
  _ctls_conn_release_ssl(conn);
  /* The code reads m_procs before it releases the reference of this
   * connection. ctls_ctx_release() frees ctx if this is the last reference,
   * so a read of conn->ctx->m_procs after that call is a use-after-free. */
  ccol_memmgmt_procs_t *mp = conn->ctx->m_procs;
  _ccol_mem_free(mp, conn->peer_der);
  _ccol_mem_free(mp, conn->peer_subject);
  X509_free(conn->peer_info_cert);
  ctls_ctx_release(conn->ctx);
  _ccol_mem_free(mp, conn);
}

#ifdef RUNNING_UNIT_TESTS
/* A white-box accessor. It exposes the raw OpenSSL SSL*, so that
 * tests/ctls/tests.c can inspect the certificate of the peer that the two
 * sides agreed on. The suite uses this to verify the SNI dispatch. Real
 * callers need no public equivalent of this accessor. */
SSL *_ctls_conn_ssl_for_tests(ctls_conn_t *conn) {
  return conn ? conn->ssl : NULL;
}

/* A white-box accessor. It gives the SSL_CTX that a servername dispatch
 * installed on this connection, and that this module holds a reference on.
 * It gives NULL for a connection that never dispatched. A suite can then
 * assert that the context that a connection serves from is a context that
 * the module pins itself. The suite does not have to take that answer from
 * the internal bookkeeping of OpenSSL. */
SSL_CTX *_ctls_conn_pinned_sni_ctx_for_tests(ctls_conn_t *conn) {
  return conn ? conn->pinned_sni_ctx : NULL;
}
#endif
