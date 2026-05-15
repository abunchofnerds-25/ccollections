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

#pragma once

#include <common.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/**
 * @file ctls.h
 * @brief INTERNAL ONLY. A reactor-agnostic OpenSSL wrapper: TLS context
 *        configuration (certificates, trust, ALPN, name-based dispatch) plus
 *        a step-driven, non-blocking per-connection handshake/read/write API
 *        for both client mode and server mode.
 *
 * This is not a public collections module: it has no type-safe macros and is
 * never included by chttp.h/chttpclient.h/chttpserver.h or any other public
 * header. src/chttpclient.c and src/chttpserver.c are the only two files
 * meant to #include this header.
 *
 * Design notes, for anyone extending this module later:
 *
 * - Every function reports failure via a return value (NULL, or a
 *   ccol_retval_t); nothing in this module ever calls exit()/abort() on a
 *   caller-supplied bad configuration (e.g. an unreadable cert file). This
 *   was a deliberate, confirmed design choice: a misconfigured TLS setup is
 *   the caller's own input error to detect and act on (log, refuse to
 *   start, retry with a different path, ...), not a reason for this
 *   library to kill the whole process out from under an application that
 *   may have other unrelated work in flight.
 *
 * - ctls_ctx_t is a ref-counted, mutable, mutex-protected TLS configuration
 *   object shared by many ctls_conn_t connections. Every mutator
 *   (ctls_ctx_cert_add / ctls_ctx_trust / ctls_ctx_trust_system /
 *   ctls_ctx_alpn_add) triggers a full internal rebuild of the underlying
 *   OpenSSL SSL_CTX object(s) from the stored configuration, always
 *   rebuilding from scratch rather than patching one in place; this keeps
 *   the invariant simple (the live SSL_CTX is always a pure function of the
 *   stored config) at the cost of a full rebuild per mutation, which is
 *   fine since these are one-time startup-configuration calls, never a
 *   per-connection cost.
 *
 * - Real per-hostname (SNI) certificate dispatch: ctls_ctx_cert_add
 *   (server_name != NULL) genuinely dispatches by hostname, rather than
 *   merely calling SSL_CTX_use_certificate a second time on the same
 *   SSL_CTX (which would just overwrite the same OpenSSL certificate-type
 *   slot and never actually dispatch by hostname despite superficially
 *   looking like per-name configuration). Each named certificate gets its
 *   own fully-built SSL_CTX, and an SSL_CTX_set_tlsext_servername_callback
 *   installed once on the context's default SSL_CTX swaps the live SSL
 *   object onto the matching named SSL_CTX via SSL_set_SSL_CTX() the moment
 *   a ClientHello's SNI extension names it. This is deliberate, confirmed
 *   capability, not an incidental side effect of the certificate-storage
 *   design.
 *
 * - ALPN protocol selection (ctls_ctx_alpn_add) fires its on_selected
 *   callback synchronously, from directly inside whatever call is already
 *   driving the handshake (ctls_conn_handshake_step()), rather than
 *   deferring it onto a reactor task queue: this reactor-agnostic module
 *   has no such queue and no need for one, since ctls_conn_handshake_step
 *   is already a synchronous call from the caller's own perspective,
 *   whether invoked from a blocking loop or from an event_loop
 *   on_readable/on_writable callback. This is a deliberate simplification,
 *   not a functionality cut: the caller learns the selected protocol at the
 *   same logical point either way, and no application code in this
 *   codebase currently drives ALPN selection asynchronously in the first
 *   place.
 *
 * - Session resumption (OpenSSL's session cache and TLS 1.3 tickets) and
 *   mutual TLS (a configured trust store implies SSL_VERIFY_PEER, requiring
 *   a peer certificate) both work as unmodified OpenSSL default behavior:
 *   only three explicit settings are applied on top
 *   (SSL_MODE_ENABLE_PARTIAL_WRITE, SSL_CTX_set_min_proto_version
 *   (TLS1_2_VERSION), SSL_OP_NO_COMPRESSION) and everything else is left
 *   alone.
 */

/** @brief Opaque, ref-counted TLS configuration object (certs/trust/ALPN). */
typedef struct ctls_ctx ctls_ctx_t;

/** @brief Opaque per-connection handle (one OpenSSL SSL object + BIO). */
typedef struct ctls_conn ctls_conn_t;

/** @brief Result of one non-blocking handshake step; see
 *         ctls_conn_handshake_step(). */
typedef enum ctls_handshake_result {
  CTLS_HANDSHAKE_DONE = 0,   /**< Handshake complete; conn is ready for I/O. */
  CTLS_HANDSHAKE_WANT_READ,  /**< Call again once the fd is readable. */
  CTLS_HANDSHAKE_WANT_WRITE, /**< Call again once the fd is writable. */
  CTLS_HANDSHAKE_ERROR       /**< Fatal error; destroy the connection. */
} ctls_handshake_result_t;

/**
 * @brief ALPN protocol-selection callback, fired synchronously from within
 *        ctls_conn_handshake_step() the moment a protocol is settled on
 *        (server mode: the client's offer matched this entry, or no entry
 *        matched and this is the default (first-registered) entry acting as
 *        a fallback; client mode: the server selected this protocol, or, if
 *        the server didn't select one at all, the default entry again acts
 *        as a fallback).
 *
 * @param conn           The connection the protocol was selected for. Use
 *                        ctls_conn_udata() to retrieve any per-connection
 *                        context the caller attached at creation time.
 * @param protocol_name  Selected protocol name. NOT NUL-terminated (raw TLS
 *                        wire bytes); always use protocol_len.
 * @param protocol_len   Byte length of protocol_name.
 * @param alpn_udata     The udata pointer passed to ctls_ctx_alpn_add() for
 *                        this specific protocol registration.
 */
typedef void (*ctls_alpn_selected_fn)(ctls_conn_t *conn,
                                      const char *protocol_name,
                                      size_t protocol_len, void *alpn_udata);

/**
 * @brief Cleanup callback for an ALPN registration's alpn_udata, called when
 *        the owning ctls_ctx_t is destroyed. May be NULL.
 */
typedef void (*ctls_alpn_cleanup_fn)(void *alpn_udata);

/* ========================================================================== */
/*                    CONTEXT (ctls_ctx_t) CONSTRUCTION                       */
/* ========================================================================== */

/**
 * @brief Creates a new, empty TLS context (custom allocator).
 *
 * A freshly created context has no certificate, no trust store, and no ALPN
 * protocols configured; it is immediately usable to create client-mode
 * connections with no certificate presented (the common case for an
 * outbound HTTPS client with no mutual-TLS requirement). Add a certificate
 * via ctls_ctx_cert_add() before using it to create server-mode connections.
 *
 * @param mp       Custom allocator, or NULL for malloc/free.
 * @param err_str  Optional: receives a static diagnostic string on failure.
 * @return New context with a reference count of 1, or NULL on allocation
 *         failure.
 */
ctls_ctx_t *ctls_ctx_new_mp(ccol_memmgmt_procs_t *mp, char **err_str);

/** @brief Creates a new, empty TLS context (default allocator). */
static inline __attribute__((always_inline)) ctls_ctx_t *ctls_ctx_new(
    char **err_str) {
  return ctls_ctx_new_mp(NULL, err_str);
}

/**
 * @brief Adds (or replaces) a certificate/key pair on ctx.
 *
 * server_name NULL or "" configures the context's DEFAULT certificate: used
 * for client-mode connections (i.e. this is how a client presents its own
 * certificate for mutual TLS) and for server-mode connections when no SNI
 * name is sent by the peer, or when no named certificate below matches the
 * SNI name that was sent. Calling this again with server_name NULL/""
 * replaces the previous default certificate.
 *
 * server_name non-NULL/non-empty registers (or replaces, if the same name
 * was already registered) a certificate dispatched by exact hostname match
 * against the TLS ClientHello's SNI extension; a leading "*." is matched as
 * a one-label wildcard (e.g. "*.example.com" matches "foo.example.com" but
 * not "example.com" or "a.b.example.com"), mirroring common CA-issued
 * wildcard certificates. The very first call with a non-empty server_name
 * installs the SNI dispatch callback on ctx; subsequent calls simply add
 * more named entries.
 *
 * cert_path and key_path must both be NULL, or both non-NULL:
 * - Both non-NULL: loads the certificate/key from these PEM files (read
 *   once, at this call; ctx does not re-read them later).
 * - Both NULL: generates a self-signed certificate (development/testing
 *   convenience). If server_name is non-NULL/non-empty, it is used as the
 *   certificate's subject (and, per the SNI dispatch rules above, also
 *   registers it as a named entry); if server_name is NULL/"", a generic
 *   fixed subject name is used instead, since the default slot itself
 *   carries no name of its own to borrow one from; this is how to get a
 *   self-signed DEFAULT certificate (used when no SNI name matches).
 *
 * @param ctx          Context to modify.
 * @param server_name  See above. May be NULL.
 * @param cert_path    PEM certificate file path, or NULL. See above.
 * @param key_path     PEM private key file path, or NULL. See above.
 * @param pk_password  Optional private-key decryption password, or NULL.
 * @param err_str       Optional: receives a static diagnostic string on
 *                      failure.
 * @return ccol_success, ccol_invalid_args (ctx NULL, or an invalid
 *         cert_path/key_path/server_name combination), ccol_not_enough_memory,
 *         or ccol_http_tls_cert_load_failed (cert_path/key_path unreadable
 *         or malformed).
 */
ccol_retval_t ctls_ctx_cert_add(ctls_ctx_t *ctx, const char *server_name,
                                const char *cert_path, const char *key_path,
                                const char *pk_password, char **err_str);

/**
 * @brief Adds a CA bundle to ctx's trust store and enables peer certificate
 *        verification (SSL_VERIFY_PEER) for connections created from ctx.
 *
 * For a client-mode context: verifies the server's certificate chain
 * against this bundle. For a server-mode context: additionally requests a
 * client certificate and, if one is presented, verifies it against this
 * same bundle; this is what enables mutual TLS on the server side.
 * Verbatim OpenSSL semantics for plain SSL_VERIFY_PEER with no
 * SSL_VERIFY_FAIL_IF_NO_PEER_CERT (which this module does not set): a
 * client that presents NO certificate at all is still accepted (there is
 * nothing to fail verification on); a client that DOES present one must
 * have it verify successfully against ctx's trust store, or the handshake
 * fails. In other words, this is already a request-but-don't-strictly-
 * require mode by construction, not (as an earlier draft of this comment
 * claimed) an unconditional requirement; a caller wanting to reject
 * anonymous (no-certificate) clients outright needs a stricter mode this
 * module does not currently provide.
 *
 * May be called more than once; each call adds to the trust store rather
 * than replacing it.
 *
 * @param ctx            Context to modify.
 * @param ca_bundle_path PEM file containing one or more trusted certificates.
 * @param err_str        Optional: receives a static diagnostic string on
 *                        failure.
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory, or
 *         ccol_http_tls_cert_load_failed.
 */
ccol_retval_t ctls_ctx_trust(ctls_ctx_t *ctx, const char *ca_bundle_path,
                             char **err_str);

/**
 * @brief Marks ctx as trusting the operating system's default CA store, in
 *        addition to (or instead of) anything added via ctls_ctx_trust().
 *        Also enables SSL_VERIFY_PEER, same as ctls_ctx_trust().
 */
void ctls_ctx_trust_system(ctls_ctx_t *ctx);

/**
 * @brief Registers an ALPN protocol on ctx.
 *
 * The first protocol ever registered on a given ctx becomes the "default":
 * used as an on_selected fallback (see ctls_alpn_selected_fn's doc comment)
 * when wire-level ALPN negotiation itself doesn't produce a match.
 *
 * @param ctx             Context to modify.
 * @param protocol_name   Protocol name (e.g. "http/1.1"); at most 255 bytes.
 * @param on_selected     Optional callback fired when this protocol is
 *                        selected for a connection. May be NULL.
 * @param alpn_udata      Opaque pointer passed to on_selected/on_cleanup.
 * @param on_cleanup      Optional cleanup for alpn_udata, called when ctx is
 *                        destroyed or this registration is replaced. May be
 *                        NULL.
 * @param err_str         Optional: receives a static diagnostic string on
 *                        failure.
 * @return ccol_success, ccol_invalid_args (ctx/protocol_name NULL, or
 *         protocol_name longer than 255 bytes), or ccol_not_enough_memory.
 */
ccol_retval_t ctls_ctx_alpn_add(ctls_ctx_t *ctx, const char *protocol_name,
                                ctls_alpn_selected_fn on_selected,
                                void *alpn_udata,
                                ctls_alpn_cleanup_fn on_cleanup,
                                char **err_str);

/** @brief Returns the number of ALPN protocols registered on ctx. */
size_t ctls_ctx_alpn_count(const ctls_ctx_t *ctx);

/**
 * @brief Increments ctx's reference count. Pair with ctls_ctx_release().
 *
 * Used to pin a context for the lifetime of an in-flight connection/request
 * against a concurrent reconfiguration (a new ctls_ctx_cert_add() etc. call)
 * on the same ctx from another thread.
 */
void ctls_ctx_retain(ctls_ctx_t *ctx);

/**
 * @brief Decrements ctx's reference count, freeing it once it reaches zero.
 *        Safe to call with ctx == NULL (no-op).
 */
void ctls_ctx_release(ctls_ctx_t *ctx);

/* ========================================================================== */
/*                  CONNECTION (ctls_conn_t) - STEP-DRIVEN API                */
/* ========================================================================== */

/**
 * @brief Creates a client-mode TLS connection over an already-connected (or
 *        in-progress non-blocking connect) file descriptor.
 *
 * Does not perform any I/O or take ownership of fd (the caller closes fd,
 * after calling ctls_conn_destroy(), exactly as with a plain socket).
 *
 * @param ctx          Context to use. Retained internally (see
 *                      ctls_ctx_retain()); the caller's own reference is
 *                      unaffected and may be released independently.
 * @param fd           Connected (or connecting) socket file descriptor.
 * @param hostname     Server hostname or IP-literal, for SNI (skipped for
 *                      an IP literal, per RFC 6066) and, if verify_host is
 *                      true, certificate hostname/IP verification. May be
 *                      NULL to skip both.
 * @param verify_host  If true, verifies the certificate's hostname/IP
 *                      against hostname (in addition to whatever chain
 *                      verification ctx's trust store configuration
 *                      implies; hostname verification with no chain trust
 *                      configured at all gives no real security guarantee,
 *                      since the certificate could be entirely forged;
 *                      callers should generally pair this with
 *                      ctls_ctx_trust()/ctls_ctx_trust_system()).
 * @param err_str      Optional: receives a static diagnostic string on
 *                      failure.
 * @return New connection, or NULL on failure (ctx/fd invalid, allocation
 *         failure, or the requested hostname/IP verification target could
 *         not be configured).
 */
ctls_conn_t *ctls_conn_create_client(ctls_ctx_t *ctx, int fd,
                                     const char *hostname, bool verify_host,
                                     char **err_str);

/**
 * @brief Creates a server-mode TLS connection over an already-accepted file
 *        descriptor.
 *
 * @param ctx    Context to use (must have at least a default certificate
 *               configured via ctls_ctx_cert_add() for the handshake to
 *               succeed; this is not checked here, only at handshake time,
 *               matching how a missing certificate would surface anyway).
 * @param fd     Accepted socket file descriptor.
 * @param udata  Opaque per-connection pointer, retrievable via
 *               ctls_conn_udata() (e.g. from an ALPN on_selected callback).
 * @param err_str Optional: receives a static diagnostic string on failure.
 * @return New connection, or NULL on failure.
 */
ctls_conn_t *ctls_conn_create_server(ctls_ctx_t *ctx, int fd, void *udata,
                                     char **err_str);

/**
 * @brief Drives one non-blocking step of the TLS handshake.
 *
 * The caller re-invokes this after the fd becomes readable/writable
 * (whichever CTLS_HANDSHAKE_WANT_* was last returned) until it returns
 * something other than WANT_READ/WANT_WRITE. Never blocks.
 *
 * Once this returns CTLS_HANDSHAKE_DONE, any negotiated ALPN protocol's
 * on_selected callback has already fired (see ctls_alpn_selected_fn), and
 * ctls_conn_read()/ctls_conn_write() may be used.
 *
 * @param conn Connection to drive.
 * @return CTLS_HANDSHAKE_DONE, CTLS_HANDSHAKE_WANT_READ,
 *         CTLS_HANDSHAKE_WANT_WRITE, or CTLS_HANDSHAKE_ERROR (fatal;
 *         destroy conn).
 */
ctls_handshake_result_t ctls_conn_handshake_step(ctls_conn_t *conn);

/**
 * @brief Reads decrypted application data from conn.
 *
 * Behaves like a non-blocking read(2): returns the number of bytes read
 * (which may be less than len), 0 on a clean peer-initiated TLS shutdown
 * (close_notify), or -1 with errno set to EWOULDBLOCK/EAGAIN (try again
 * once the fd is readable) or another errno for a fatal I/O/protocol error.
 *
 * Only valid after ctls_conn_handshake_step() has returned
 * CTLS_HANDSHAKE_DONE.
 */
ssize_t ctls_conn_read(ctls_conn_t *conn, void *buf, size_t len);

/**
 * @brief Writes application data to conn, encrypting it.
 *
 * Behaves like a non-blocking write(2): returns the number of bytes
 * consumed (which may be less than len; SSL_MODE_ENABLE_PARTIAL_WRITE is
 * always set), or -1 with errno set to EWOULDBLOCK/EAGAIN (try again once
 * the fd is writable) or another errno for a fatal I/O/protocol error.
 *
 * Only valid after ctls_conn_handshake_step() has returned
 * CTLS_HANDSHAKE_DONE.
 */
ssize_t ctls_conn_write(ctls_conn_t *conn, const void *buf, size_t len);

/**
 * @brief Returns the peer certificate verification result (an OpenSSL
 *        X509_V_OK / X509_V_ERR_* value; see <openssl/x509_vfy.h>).
 *
 * Meaningful to call once the handshake has failed and the caller wants to
 * distinguish "certificate verification failed" from some other handshake
 * error.
 */
long ctls_conn_verify_result(const ctls_conn_t *conn);

/**
 * @brief Returns the negotiated ALPN protocol name for conn, or NULL if
 *        none was negotiated (no ALPN protocols were registered on the
 *        owning ctx, or the peer/negotiation produced no selection).
 *
 * @param conn     Connection to query.
 * @param len_out  Optional: receives the byte length of the returned name
 *                 (NOT NUL-terminated). May be NULL.
 */
const char *ctls_conn_alpn_selected(const ctls_conn_t *conn, size_t *len_out);

/** @brief Returns the udata pointer passed to ctls_conn_create_server(). */
void *ctls_conn_udata(const ctls_conn_t *conn);

/**
 * @brief Destroys conn, freeing its OpenSSL state and releasing its
 *        reference on the owning ctls_ctx_t. Does not close the underlying
 *        file descriptor. Safe to call with conn == NULL (no-op).
 */
void ctls_conn_destroy(ctls_conn_t *conn);
