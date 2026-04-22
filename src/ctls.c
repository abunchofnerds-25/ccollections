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
#include <ctls.h>
#include <netinet/in.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/*                       PROCESS-WIDE SELF-SIGNED ROOT KEY                    */
/* ========================================================================== */

/* Lazily generated once per process, shared by every self-signed certificate
 * this module ever generates (mirrors fio_tls_make_root_key's own shape).
 * This is genuinely process-global state (not tied to any one ctls_ctx_t
 * instance), so it is guarded by a once_flag_t rather than an instance-level
 * mutex. */
static EVP_PKEY *g_ctls_root_key = NULL;
static mutex_t g_ctls_root_key_mutex;
static once_flag_t g_ctls_root_key_once = ONCE_INIT;

static void _ctls_root_key_globals_init(void) {
  mutex_init(g_ctls_root_key_mutex);
}

static EVP_PKEY *_ctls_get_root_key(void) {
  call_once(g_ctls_root_key_once, _ctls_root_key_globals_init);
  mutex_lock(g_ctls_root_key_mutex);
  if (!g_ctls_root_key) {
    /* EVP_RSA_gen (a thin macro over EVP_PKEY_Q_keygen) is the modern,
     * non-deprecated OpenSSL 3.x replacement for the RSA_new/BN_new/
     * RSA_generate_key_ex/EVP_PKEY_assign_RSA sequence facio's own
     * fio_tls_make_root_key used; that sequence is deprecated as of
     * OpenSSL 3.0 and would trip -Wdeprecated-declarations under this
     * codebase's -Werror build (unlike third_party/facio, this is not
     * vendored code built with relaxed warnings). */
    g_ctls_root_key = EVP_RSA_gen(2048);
  }
  EVP_PKEY *key = g_ctls_root_key;
  mutex_unlock(g_ctls_root_key_mutex);
  return key;
}

/* ========================================================================== */
/*                       ALPN EX-DATA INDEX (SSL* -> ctls_conn_t*) */
/* ========================================================================== */

static int g_ctls_conn_ex_idx = -1;
static mutex_t g_ctls_ex_idx_mutex;
static once_flag_t g_ctls_ex_idx_once = ONCE_INIT;

static void _ctls_ex_idx_globals_init(void) { mutex_init(g_ctls_ex_idx_mutex); }

static int _ctls_conn_ex_idx(void) {
  call_once(g_ctls_ex_idx_once, _ctls_ex_idx_globals_init);
  mutex_lock(g_ctls_ex_idx_mutex);
  if (g_ctls_conn_ex_idx < 0)
    g_ctls_conn_ex_idx = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
  int idx = g_ctls_conn_ex_idx;
  mutex_unlock(g_ctls_ex_idx_mutex);
  return idx;
}

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

/* One named (SNI-dispatched) certificate: stored config plus its own fully
 * built SSL_CTX, rebuilt whenever ctx-wide state (trust/ALPN) changes. */
typedef struct ctls_named_cert {
  char *cert_pem;
  size_t cert_len;
  char *key_pem;
  size_t key_len;
  char *pk_password;
  bool self_signed;
  char *self_signed_name; /* subject name used if self_signed */
  SSL_CTX *built_ctx;     /* NULL until the first successful rebuild */
} ctls_named_cert;

struct ctls_ctx {
  size_t ref;
  mutex_t lock;
  ccol_memmgmt_procs_t *m_procs; /* owned copy, or NULL for default alloc */

  /* Default certificate (server_name NULL/"" in ctls_ctx_cert_add). */
  bool has_default_cert;
  char *default_cert_pem;
  size_t default_cert_len;
  char *default_key_pem;
  size_t default_key_len;
  char *default_pk_password;
  bool default_self_signed;
  char *default_self_signed_name;

  /* Named (SNI-dispatched) certificates: chmap(char* hostname ->
   * ctls_named_cert*). */
  chmap named_certs;
  bool sni_callback_installed;

  /* Trust store: an array of loaded CA-bundle PEM blobs. */
  char **trust_pems;
  size_t *trust_lens;
  size_t trust_count;
  size_t trust_cap;
  bool verify_default_store;
  bool verify_peer;

  /* ALPN protocol registrations, in registration order (index 0 = default). */
  ctls_alpn_entry *alpn;
  size_t alpn_count;
  size_t alpn_cap;

  SSL_CTX
  *ctx_default; /* built from the fields above; NULL until first build */
};

struct ctls_conn {
  SSL *ssl;
  ctls_ctx_t *ctx; /* retained */
  bool is_server;
  bool handshake_done;
  void *udata;
  const char
      *alpn_selected_name; /* points into an ctx alpn_entry's name, or NULL */
  size_t alpn_selected_len;
};

/* ========================================================================== */
/*                             SMALL HELPERS                                  */
/* ========================================================================== */

static char *_ctls_strdup(ccol_memmgmt_procs_t *mp, const char *s) {
  if (!s) return NULL;
  size_t len = strlen(s) + 1;
  char *d = (char *)_mem_alloc(mp, len);
  if (!d) return NULL;
  memcpy(d, s, len);
  return d;
}

static char *_ctls_strdup_lower(ccol_memmgmt_procs_t *mp, const char *s) {
  if (!s) return NULL;
  size_t len = strlen(s) + 1;
  char *d = (char *)_mem_alloc(mp, len);
  if (!d) return NULL;
  for (size_t i = 0; i < len; ++i) d[i] = (char)tolower((unsigned char)s[i]);
  return d;
}

/* Reads an entire file into a freshly allocated buffer. Returns false (no
 * partial allocation left behind) on any failure: missing file, read error,
 * or a zero-length file (never a meaningful cert/key/CA bundle). */
static bool _ctls_read_file(ccol_memmgmt_procs_t *mp, const char *path,
                            char **out_data, size_t *out_len) {
  *out_data = NULL;
  *out_len = 0;
  if (!path) return false;
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return false;
  }
  long sz = ftell(f);
  if (sz <= 0) {
    fclose(f);
    return false;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return false;
  }
  char *buf = (char *)_mem_alloc(mp, (size_t)sz);
  if (!buf) {
    fclose(f);
    return false;
  }
  size_t got = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (got != (size_t)sz) {
    _mem_free(mp, buf);
    return false;
  }
  *out_data = buf;
  *out_len = (size_t)sz;
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
  ASN1_INTEGER_set(X509_get_serialNumber(cert), (long)serial);
  X509_gmtime_adj(X509_get_notBefore(cert), 0);
  X509_gmtime_adj(X509_get_notAfter(cert), 15552000L); /* 180 days */
  X509_set_pubkey(cert, root_key);
  X509_NAME *s = X509_get_subject_name(cert);
  size_t name_len = strlen(server_name);
  X509_NAME_add_entry_by_txt(s, "O", MBSTRING_ASC,
                             (const unsigned char *)server_name, (int)name_len,
                             -1, 0);
  X509_NAME_add_entry_by_txt(s, "CN", MBSTRING_ASC,
                             (const unsigned char *)server_name, (int)name_len,
                             -1, 0);
  X509_set_issuer_name(cert, s);
  if (!X509_sign(cert, root_key, EVP_sha512())) {
    X509_free(cert);
    return NULL;
  }
  return cert;
}

/* ========================================================================== */
/*                          SSL_CTX (RE)BUILDING                              */
/* ========================================================================== */

/* Applies the three explicit settings this module's TLS session-resumption
 * behavior implicitly depends on, and nothing else: leaving OpenSSL's own
 * session-cache/ticket defaults untouched is what makes resumption "just
 * work", matching the facio layer this replaces exactly. */
static bool _ctls_apply_base_settings(SSL_CTX *ctx) {
  if (!ctx) return false;
  SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
  return true;
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
    return ok;
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
          } else {
            SSL_CTX_add1_chain_cert(ctx, tmp->x509);
          }
        }
      }
      sk_X509_INFO_pop_free(inf, X509_INFO_free);
    }
    BIO_free(cbio);
  }
  return cert_ok;
}

static bool _ctls_apply_trust(SSL_CTX *ctx, ctls_ctx_t *tls) {
  if (tls->trust_count == 0 && !tls->verify_default_store) return true;
  X509_STORE *store = X509_STORE_new();
  if (!store) return false;
  SSL_CTX_set_cert_store(ctx, store);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
  if (tls->verify_default_store) SSL_CTX_set_default_verify_paths(ctx);
  for (size_t i = 0; i < tls->trust_count; ++i) {
    BIO *bio = BIO_new_mem_buf(tls->trust_pems[i], (int)tls->trust_lens[i]);
    if (!bio) continue;
    STACK_OF(X509_INFO) *inf = PEM_X509_INFO_read_bio(bio, NULL, NULL, NULL);
    if (inf) {
      for (int j = 0; j < sk_X509_INFO_num(inf); ++j) {
        X509_INFO *tmp = sk_X509_INFO_value(inf, j);
        if (tmp->x509) X509_STORE_add_cert(store, tmp->x509);
        if (tmp->crl) X509_STORE_add_crl(store, tmp->crl);
      }
      sk_X509_INFO_pop_free(inf, X509_INFO_free);
    }
    BIO_free(bio);
  }
  return true;
}

/* Synchronous ALPN selection callback (server mode): unlike facio's async
 * fio_defer-based dispatch, on_selected fires directly here, from within
 * the SSL_accept()/SSL_do_handshake() call that triggered it -- itself
 * already invoked synchronously by ctls_conn_handshake_step(). See the
 * design-notes doc comment at the top of ctls.h. */
static int _ctls_alpn_select_cb(SSL *ssl, const unsigned char **out,
                                unsigned char *outlen, const unsigned char *in,
                                unsigned int inlen, void *arg) {
  ctls_ctx_t *tls = (ctls_ctx_t *)arg;
  ctls_conn_t *conn = (ctls_conn_t *)SSL_get_ex_data(ssl, _ctls_conn_ex_idx());
  if (tls->alpn_count == 0) return SSL_TLSEXT_ERR_NOACK;
  const unsigned char *p = in;
  const unsigned char *end = in + inlen;
  while (p < end) {
    uint8_t l = p[0];
    const unsigned char *name = p + 1;
    p += (size_t)l + 1;
    for (size_t i = 0; i < tls->alpn_count; ++i) {
      ctls_alpn_entry *e = &tls->alpn[i];
      if (e->name_len == l && memcmp(e->name, name, l) == 0) {
        *out = (const unsigned char *)e->name;
        *outlen = l;
        if (conn) {
          conn->alpn_selected_name = e->name;
          conn->alpn_selected_len = e->name_len;
        }
        if (e->on_selected)
          e->on_selected(conn, e->name, e->name_len, e->udata);
        return SSL_TLSEXT_ERR_OK;
      }
    }
  }
  /* No overlap: fall back to the default (first-registered) protocol's
   * callback anyway, mirroring facio's own fio_tls_alpn_selector_cb
   * fallback behavior exactly, even though the wire negotiation itself
   * reports NOACK. */
  ctls_alpn_entry *def = &tls->alpn[0];
  if (conn) {
    conn->alpn_selected_name = def->name;
    conn->alpn_selected_len = def->name_len;
  }
  if (def->on_selected)
    def->on_selected(conn, def->name, def->name_len, def->udata);
  return SSL_TLSEXT_ERR_NOACK;
}

static bool _ctls_apply_alpn(SSL_CTX *ctx, ctls_ctx_t *tls) {
  if (tls->alpn_count == 0) return true;
  size_t wire_len = 0;
  for (size_t i = 0; i < tls->alpn_count; ++i)
    wire_len += tls->alpn[i].name_len + 1;
  unsigned char *wire = (unsigned char *)_mem_alloc(tls->m_procs, wire_len);
  if (!wire) return false;
  size_t pos = 0;
  for (size_t i = 0; i < tls->alpn_count; ++i) {
    wire[pos++] = (unsigned char)tls->alpn[i].name_len;
    memcpy(wire + pos, tls->alpn[i].name, tls->alpn[i].name_len);
    pos += tls->alpn[i].name_len;
  }
  /* SSL_CTX_set_alpn_protos (client-mode offer list) copies its input
   * internally, so wire need not outlive this call. */
  SSL_CTX_set_alpn_protos(ctx, wire, (unsigned int)wire_len);
  _mem_free(tls->m_procs, wire);
  SSL_CTX_set_alpn_select_cb(ctx, _ctls_alpn_select_cb, tls);
  return true;
}

/* Case-insensitive exact match, or one-level leading "*." wildcard match
 * (e.g. a registered "*.example.com" matches a queried "foo.example.com"). */
static bool _ctls_sni_name_matches(const char *pattern, const char *query) {
  if (strcmp(pattern, query) == 0) return true;
  if (pattern[0] == '*' && pattern[1] == '.') {
    const char *dot = strchr(query, '.');
    if (dot && strcmp(pattern + 2, dot + 1) == 0) return true;
  }
  return false;
}

static SSL_CTX *_ctls_find_named_ctx(ctls_ctx_t *tls, const char *sni_name) {
  if (!tls->named_certs || !sni_name) return NULL;
  char *lower = _ctls_strdup_lower(tls->m_procs, sni_name);
  if (!lower) return NULL;
  SSL_CTX *found = NULL;
  char *err = NULL;
  cmap_iterator *it = chashmap_begin_iter(tls->named_certs, &err);
  while (it) {
    const char *pattern = (const char *)it->key_pair->ptr;
    if (_ctls_sni_name_matches(pattern, lower)) {
      ctls_named_cert *nc;
      memcpy(&nc, it->val_pair->ptr, sizeof(nc));
      found = nc->built_ctx;
      ccol_iter_destroy(it);
      break;
    }
    it = it->_next_fn(it);
  }
  _mem_free(tls->m_procs, lower);
  return found;
}

static int _ctls_servername_cb(SSL *ssl, int *ad, void *arg) {
  (void)ad;
  ctls_ctx_t *tls = (ctls_ctx_t *)arg;
  const char *name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
  if (!name) return SSL_TLSEXT_ERR_OK;
  mutex_lock(tls->lock);
  SSL_CTX *matched = _ctls_find_named_ctx(tls, name);
  mutex_unlock(tls->lock);
  if (matched) SSL_set_SSL_CTX(ssl, matched);
  return SSL_TLSEXT_ERR_OK;
}

/* Builds one SSL_CTX from scratch, applying base settings, an (optional)
 * certificate, the shared trust store, and the shared ALPN configuration.
 * Used both for ctx_default and for every named (SNI) certificate's own
 * SSL_CTX -- each is a fully independent, fully configured context so that
 * SSL_set_SSL_CTX() can swap onto it wholesale from the servername
 * callback. Returns NULL on failure (caller must not leave the ctx_t's
 * previous, still-valid SSL_CTX pointer overwritten until this succeeds). */
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

/* Rebuilds ctx_default and every named certificate's SSL_CTX from the
 * currently stored configuration. Called (under tls->lock) after every
 * mutating call (cert_add/trust/trust_system/alpn_add), mirroring facio's
 * fio_tls_build_context "always rebuild from scratch" approach. On failure,
 * the previous, still-valid SSL_CTX objects are left in place rather than
 * torn down, so a failed reconfiguration attempt cannot leave ctx in a
 * worse (unusable) state than before the call. */
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

  /* Rebuild every named cert's own SSL_CTX before committing new_default,
   * so a mid-rebuild failure leaves everything (including the entries
   * whose new SSL_CTX we already built) consistent: either every context
   * rebuilds successfully and we swap all of them in together, or none of
   * them are swapped in at all. */
  size_t named_count =
      tls->named_certs ? chmap_elem_count(tls->named_certs) : 0;
  SSL_CTX **new_named_ctxs = NULL;
  ctls_named_cert **new_named_entries = NULL;
  if (named_count > 0) {
    new_named_ctxs =
        (SSL_CTX **)_mem_alloc(tls->m_procs, named_count * sizeof(SSL_CTX *));
    new_named_entries = (ctls_named_cert **)_mem_alloc(
        tls->m_procs, named_count * sizeof(ctls_named_cert *));
    if (!new_named_ctxs || !new_named_entries) {
      _mem_free(tls->m_procs, new_named_ctxs);
      _mem_free(tls->m_procs, new_named_entries);
      SSL_CTX_free(new_default);
      return false;
    }
    char *err = NULL;
    cmap_iterator *it = chashmap_begin_iter(tls->named_certs, &err);
    size_t i = 0;
    bool ok = true;
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
      _mem_free(tls->m_procs, new_named_ctxs);
      _mem_free(tls->m_procs, new_named_entries);
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
  _mem_free(tls->m_procs, new_named_ctxs);
  _mem_free(tls->m_procs, new_named_entries);
  return true;
}

/* ========================================================================== */
/*                       ctls_ctx_t CONSTRUCTION / MUTATION                   */
/* ========================================================================== */

static void _ctls_named_cert_destroy(ctls_ctx_t *tls, ctls_named_cert *nc) {
  if (!nc) return;
  _mem_free(tls->m_procs, nc->cert_pem);
  _mem_free(tls->m_procs, nc->key_pem);
  _mem_free(tls->m_procs, nc->pk_password);
  _mem_free(tls->m_procs, nc->self_signed_name);
  if (nc->built_ctx) SSL_CTX_free(nc->built_ctx);
  _mem_free(tls->m_procs, nc);
}

ctls_ctx_t *ctls_ctx_new_mp(ccol_memmgmt_procs_t *mp, char **err_str) {
  ctls_ctx_t *tls = (ctls_ctx_t *)_mem_calloc(mp, 1, sizeof(*tls));
  if (!tls) {
    if (err_str) *err_str = "ctls_ctx_new: allocation failure";
    return NULL;
  }
  if (mp) {
    tls->m_procs = (ccol_memmgmt_procs_t *)_mem_alloc(mp, sizeof(*mp));
    if (!tls->m_procs) {
      _mem_free(mp, tls);
      if (err_str) *err_str = "ctls_ctx_new: allocation failure";
      return NULL;
    }
    *tls->m_procs = *mp;
  }
  tls->ref = 1;
  mutex_init(tls->lock);
  char *err = NULL;
  tls->named_certs =
      chmap_create_mp(16, ccol_string, ccol_pointer, tls->m_procs, &err);
  if (!tls->named_certs) {
    mutex_destroy(tls->lock);
    _mem_free(mp, tls->m_procs);
    _mem_free(mp, tls);
    if (err_str) *err_str = "ctls_ctx_new: chmap allocation failure";
    return NULL;
  }
  if (!_ctls_ctx_rebuild_locked(tls)) {
    __chmap_destroy(tls->named_certs);
    mutex_destroy(tls->lock);
    _mem_free(mp, tls->m_procs);
    _mem_free(mp, tls);
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
  /* Self-signed generation needs neither file: this covers both a named
   * (SNI-dispatched) self-signed certificate (server_name supplies the
   * subject CN) and a self-signed DEFAULT certificate (server_name NULL/"":
   * falls back to a generic subject name below, since a self-signed
   * certificate always needs some subject, but the default slot itself
   * carries no name of its own to borrow one from). */
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
    if (!_ctls_read_file(ctx->m_procs, cert_path, &cert_pem, &cert_len) ||
        !_ctls_read_file(ctx->m_procs, key_path, &key_pem, &key_len)) {
      _mem_free(ctx->m_procs, cert_pem);
      _mem_free(ctx->m_procs, key_pem);
      if (err_str) *err_str = "ctls_ctx_cert_add: cert/key file unreadable";
      return ccol_http_tls_cert_load_failed;
    }
    if (pk_password) {
      pw_copy = _ctls_strdup(ctx->m_procs, pk_password);
      if (!pw_copy) {
        _mem_free(ctx->m_procs, cert_pem);
        _mem_free(ctx->m_procs, key_pem);
        return ccol_not_enough_memory;
      }
    }
  }

  mutex_lock(ctx->lock);
  ccol_retval_t rv = ccol_success;
  if (!have_name) {
    /* Replace the default certificate. */
    char *self_signed_name = NULL;
    if (self_signed) {
      /* A generic, fixed subject name when the caller supplied none: the
       * default slot has no name of its own, unlike a named entry, whose
       * server_name doubles as the self-signed subject. Fine for the
       * common "get a server running with zero cert configuration"
       * use case, since a caller relying on hostname verification against
       * this certificate would need to configure a real name anyway. */
      self_signed_name = _ctls_strdup(ctx->m_procs, "ctls-default");
      if (!self_signed_name) {
        _mem_free(ctx->m_procs, cert_pem);
        _mem_free(ctx->m_procs, key_pem);
        _mem_free(ctx->m_procs, pw_copy);
        mutex_unlock(ctx->lock);
        return ccol_not_enough_memory;
      }
    }
    _mem_free(ctx->m_procs, ctx->default_cert_pem);
    _mem_free(ctx->m_procs, ctx->default_key_pem);
    _mem_free(ctx->m_procs, ctx->default_pk_password);
    _mem_free(ctx->m_procs, ctx->default_self_signed_name);
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
      _mem_free(ctx->m_procs, cert_pem);
      _mem_free(ctx->m_procs, key_pem);
      _mem_free(ctx->m_procs, pw_copy);
      mutex_unlock(ctx->lock);
      return ccol_not_enough_memory;
    }
    ctls_named_cert *nc =
        (ctls_named_cert *)_mem_calloc(ctx->m_procs, 1, sizeof(*nc));
    if (!nc) {
      _mem_free(ctx->m_procs, lower_name);
      _mem_free(ctx->m_procs, cert_pem);
      _mem_free(ctx->m_procs, key_pem);
      _mem_free(ctx->m_procs, pw_copy);
      mutex_unlock(ctx->lock);
      return ccol_not_enough_memory;
    }
    nc->cert_pem = cert_pem;
    nc->cert_len = cert_len;
    nc->key_pem = key_pem;
    nc->key_len = key_len;
    nc->pk_password = pw_copy;
    nc->self_signed = self_signed;
    if (self_signed)
      nc->self_signed_name = _ctls_strdup(ctx->m_procs, lower_name);

    cmap_pair kp = {.ptr = lower_name, .size = strlen(lower_name) + 1};
    cmap_pair *old_vp = NULL;
    if (chmap_get_elem_ref(ctx->named_certs, &kp, &old_vp) == ccol_success) {
      ctls_named_cert *old_nc;
      memcpy(&old_nc, old_vp->ptr, sizeof(old_nc));
      _ctls_named_cert_destroy(ctx, old_nc);
    }
    cmap_pair vp = {.ptr = &nc, .size = sizeof(nc)};
    rv = chmap_insert_elem(ctx->named_certs, &kp, &vp);
    _mem_free(ctx->m_procs, lower_name);
    if (rv != ccol_success) {
      _ctls_named_cert_destroy(ctx, nc);
      mutex_unlock(ctx->lock);
      return rv;
    }
  }

  bool built = _ctls_ctx_rebuild_locked(ctx);
  mutex_unlock(ctx->lock);
  if (!built) {
    if (err_str) *err_str = "ctls_ctx_cert_add: SSL_CTX rebuild failure";
    return ccol_http_tls_cert_load_failed;
  }
  return ccol_success;
}

ccol_retval_t ctls_ctx_trust(ctls_ctx_t *ctx, const char *ca_bundle_path,
                             char **err_str) {
  if (!ctx || !ca_bundle_path) return ccol_invalid_args;
  char *pem = NULL;
  size_t len = 0;
  if (!_ctls_read_file(ctx->m_procs, ca_bundle_path, &pem, &len)) {
    if (err_str) *err_str = "ctls_ctx_trust: CA bundle file unreadable";
    return ccol_http_tls_cert_load_failed;
  }

  mutex_lock(ctx->lock);
  if (ctx->trust_count == ctx->trust_cap) {
    size_t new_cap = ctx->trust_cap ? ctx->trust_cap * 2 : 4;
    char **new_pems = (char **)_mem_realloc(ctx->m_procs, ctx->trust_pems,
                                            new_cap * sizeof(char *));
    size_t *new_lens = (size_t *)_mem_realloc(ctx->m_procs, ctx->trust_lens,
                                              new_cap * sizeof(size_t));
    if (!new_pems || !new_lens) {
      _mem_free(ctx->m_procs, pem);
      mutex_unlock(ctx->lock);
      return ccol_not_enough_memory;
    }
    ctx->trust_pems = new_pems;
    ctx->trust_lens = new_lens;
    ctx->trust_cap = new_cap;
  }
  ctx->trust_pems[ctx->trust_count] = pem;
  ctx->trust_lens[ctx->trust_count] = len;
  ctx->trust_count++;
  ctx->verify_peer = true;
  bool built = _ctls_ctx_rebuild_locked(ctx);
  mutex_unlock(ctx->lock);
  if (!built) {
    if (err_str) *err_str = "ctls_ctx_trust: SSL_CTX rebuild failure";
    return ccol_http_tls_cert_load_failed;
  }
  return ccol_success;
}

void ctls_ctx_trust_system(ctls_ctx_t *ctx) {
  if (!ctx) return;
  mutex_lock(ctx->lock);
  ctx->verify_default_store = true;
  ctx->verify_peer = true;
  _ctls_ctx_rebuild_locked(ctx);
  mutex_unlock(ctx->lock);
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

  mutex_lock(ctx->lock);
  /* Replace an existing registration with the same name, matching facio's
   * own alpn_list_overwrite semantics. */
  for (size_t i = 0; i < ctx->alpn_count; ++i) {
    if (ctx->alpn[i].name_len == name_len &&
        memcmp(ctx->alpn[i].name, name_copy, name_len) == 0) {
      if (ctx->alpn[i].on_cleanup) ctx->alpn[i].on_cleanup(ctx->alpn[i].udata);
      _mem_free(ctx->m_procs, ctx->alpn[i].name);
      ctx->alpn[i] = (ctls_alpn_entry){.name = name_copy,
                                       .name_len = name_len,
                                       .on_selected = on_selected,
                                       .udata = alpn_udata,
                                       .on_cleanup = on_cleanup};
      bool built = _ctls_ctx_rebuild_locked(ctx);
      mutex_unlock(ctx->lock);
      return built ? ccol_success : ccol_http_tls_cert_load_failed;
    }
  }
  if (ctx->alpn_count == ctx->alpn_cap) {
    size_t new_cap = ctx->alpn_cap ? ctx->alpn_cap * 2 : 4;
    ctls_alpn_entry *new_alpn = (ctls_alpn_entry *)_mem_realloc(
        ctx->m_procs, ctx->alpn, new_cap * sizeof(ctls_alpn_entry));
    if (!new_alpn) {
      _mem_free(ctx->m_procs, name_copy);
      mutex_unlock(ctx->lock);
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
  mutex_unlock(ctx->lock);
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
  mutex_lock(ctx->lock);
  ctx->ref++;
  mutex_unlock(ctx->lock);
}

void ctls_ctx_release(ctls_ctx_t *ctx) {
  if (!ctx) return;
  mutex_lock(ctx->lock);
  size_t remaining = --ctx->ref;
  mutex_unlock(ctx->lock);
  if (remaining > 0) return;

  if (ctx->ctx_default) SSL_CTX_free(ctx->ctx_default);
  _mem_free(ctx->m_procs, ctx->default_cert_pem);
  _mem_free(ctx->m_procs, ctx->default_key_pem);
  _mem_free(ctx->m_procs, ctx->default_pk_password);
  _mem_free(ctx->m_procs, ctx->default_self_signed_name);

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
    _mem_free(ctx->m_procs, ctx->trust_pems[i]);
  _mem_free(ctx->m_procs, ctx->trust_pems);
  _mem_free(ctx->m_procs, ctx->trust_lens);

  for (size_t i = 0; i < ctx->alpn_count; ++i) {
    if (ctx->alpn[i].on_cleanup) ctx->alpn[i].on_cleanup(ctx->alpn[i].udata);
    _mem_free(ctx->m_procs, ctx->alpn[i].name);
  }
  _mem_free(ctx->m_procs, ctx->alpn);

  mutex_destroy(ctx->lock);
  /* ctx->m_procs (when non-NULL) is a heap-allocated copy of the caller's
   * procs, freed via its own contained free() function pointer -- so ctx
   * itself must be freed FIRST while mp is still a live, dereferenceable
   * object; freeing mp (i.e. ctx->m_procs) before ctx would leave mp
   * dangling for the second _mem_free() call, reading mp->free from
   * already-freed memory (a real use-after-free valgrind caught during
   * development). */
  ccol_memmgmt_procs_t *mp = ctx->m_procs;
  _mem_free(mp, ctx);
  _mem_free(mp, mp);
}

/* ========================================================================== */
/*                                CONNECTIONS                                 */
/* ========================================================================== */

static ctls_conn_t *_ctls_conn_alloc(ctls_ctx_t *ctx, bool is_server) {
  ctls_conn_t *conn =
      (ctls_conn_t *)_mem_calloc(ctx->m_procs, 1, sizeof(*conn));
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
  ctls_conn_t *conn = _ctls_conn_alloc(ctx, false);
  if (!conn) {
    if (err_str) *err_str = "ctls_conn_create_client: allocation failure";
    return NULL;
  }

  mutex_lock(ctx->lock);
  SSL *ssl = SSL_new(ctx->ctx_default);
  mutex_unlock(ctx->lock);
  if (!ssl) {
    ccol_memmgmt_procs_t *mp = ctx->m_procs;
    ctls_ctx_release(ctx);
    _mem_free(mp, conn);
    if (err_str) *err_str = "ctls_conn_create_client: SSL_new failure";
    return NULL;
  }
  SSL_set_ex_data(ssl, _ctls_conn_ex_idx(), conn);

  BIO *bio = BIO_new_socket(fd, 0);
  if (!bio) {
    SSL_free(ssl);
    ccol_memmgmt_procs_t *mp = ctx->m_procs;
    ctls_ctx_release(ctx);
    _mem_free(mp, conn);
    if (err_str) *err_str = "ctls_conn_create_client: BIO_new_socket failure";
    return NULL;
  }
  BIO_up_ref(bio);
  SSL_set0_rbio(ssl, bio);
  SSL_set0_wbio(ssl, bio);

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
        SSL_free(ssl); /* also frees the one bio ref set0_rbio/wbio each took */
        ccol_memmgmt_procs_t *mp = ctx->m_procs;
        ctls_ctx_release(ctx);
        _mem_free(mp, conn);
        if (err_str)
          *err_str =
              "ctls_conn_create_client: could not configure hostname "
              "verification target";
        return NULL;
      }
    }
  }
  SSL_set_connect_state(ssl);
  conn->ssl = ssl;
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

  mutex_lock(ctx->lock);
  SSL *ssl = SSL_new(ctx->ctx_default);
  mutex_unlock(ctx->lock);
  if (!ssl) {
    ccol_memmgmt_procs_t *mp = ctx->m_procs;
    ctls_ctx_release(ctx);
    _mem_free(mp, conn);
    if (err_str) *err_str = "ctls_conn_create_server: SSL_new failure";
    return NULL;
  }
  SSL_set_ex_data(ssl, _ctls_conn_ex_idx(), conn);

  BIO *bio = BIO_new_socket(fd, 0);
  if (!bio) {
    SSL_free(ssl);
    ccol_memmgmt_procs_t *mp = ctx->m_procs;
    ctls_ctx_release(ctx);
    _mem_free(mp, conn);
    if (err_str) *err_str = "ctls_conn_create_server: BIO_new_socket failure";
    return NULL;
  }
  BIO_up_ref(bio);
  SSL_set0_rbio(ssl, bio);
  SSL_set0_wbio(ssl, bio);
  SSL_set_accept_state(ssl);
  conn->ssl = ssl;
  return conn;
}

static void _ctls_record_client_alpn(ctls_conn_t *conn) {
  ctls_ctx_t *tls = conn->ctx;
  const unsigned char *proto = NULL;
  unsigned int proto_len = 0;
  SSL_get0_alpn_selected(conn->ssl, &proto, &proto_len);
  mutex_lock(tls->lock);
  if (tls->alpn_count == 0) {
    mutex_unlock(tls->lock);
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
  if (!matched) matched = &tls->alpn[0]; /* fallback, mirroring server side */
  conn->alpn_selected_name = matched->name;
  conn->alpn_selected_len = matched->name_len;
  ctls_alpn_selected_fn cb = matched->on_selected;
  const char *name = matched->name;
  size_t name_len = matched->name_len;
  void *udata = matched->udata;
  mutex_unlock(tls->lock);
  if (cb) cb(conn, name, name_len, udata);
}

ctls_handshake_result_t ctls_conn_handshake_step(ctls_conn_t *conn) {
  if (!conn || !conn->ssl) return CTLS_HANDSHAKE_ERROR;
  ERR_clear_error();
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

/* Shared classification logic for SSL_read/SSL_write results; see the
 * matching, already-hardened logic in third_party/facio/fio_tls_openssl.c's
 * own fio_tls_read/_write, ported here verbatim (same three documented
 * fixes: SSL_ERROR_SYSCALL must not busy-loop as EWOULDBLOCK,
 * SSL_ERROR_SSL is a fatal record-layer problem and not the same as a
 * clean close, ERR_clear_error() must run before every SSL_get_error()
 * classification on a codebase where many unrelated connections' TLS I/O
 * shares a small set of threads). */
static ssize_t _ctls_classify_io_result(SSL *ssl, int ret) {
  if (ret > 0) return ret;
  int err = SSL_get_error(ssl, ret);
  switch (err) {
    case SSL_ERROR_ZERO_RETURN:
      return 0;
    case SSL_ERROR_SSL:
      errno = ECONNRESET;
      return -1;
    case SSL_ERROR_SYSCALL:
      if (!errno) errno = ECONNRESET;
      return -1;
    default:
      errno = EWOULDBLOCK;
      return -1;
  }
}

ssize_t ctls_conn_read(ctls_conn_t *conn, void *buf, size_t len) {
  if (!conn || !conn->ssl) {
    errno = EINVAL;
    return -1;
  }
  ERR_clear_error();
  int ret = SSL_read(conn->ssl, buf, (int)len);
  return _ctls_classify_io_result(conn->ssl, ret);
}

ssize_t ctls_conn_write(ctls_conn_t *conn, const void *buf, size_t len) {
  if (!conn || !conn->ssl) {
    errno = EINVAL;
    return -1;
  }
  ERR_clear_error();
  int ret = SSL_write(conn->ssl, buf, (int)len);
  return _ctls_classify_io_result(conn->ssl, ret);
}

long ctls_conn_verify_result(const ctls_conn_t *conn) {
  if (!conn || !conn->ssl) return -1;
  return SSL_get_verify_result(conn->ssl);
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
  if (conn->ssl) {
    SSL_shutdown(conn->ssl);
    SSL_free(conn->ssl);
  }
  /* Capture m_procs before releasing our reference: ctx may be freed by
   * ctls_ctx_release() if this was the last reference, so reading
   * conn->ctx->m_procs afterward would be a use-after-free. */
  ccol_memmgmt_procs_t *mp = conn->ctx->m_procs;
  ctls_ctx_release(conn->ctx);
  _mem_free(mp, conn);
}

#ifdef RUNNING_UNIT_TESTS
/* White-box accessor: exposes the raw OpenSSL SSL* so tests/ctls/tests.c can
 * inspect the negotiated peer certificate (SNI dispatch verification) with
 * no public equivalent needed for real callers. */
SSL *_ctls_conn_ssl_for_tests(ctls_conn_t *conn) {
  return conn ? conn->ssl : NULL;
}
#endif
