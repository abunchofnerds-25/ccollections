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
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h> /* bool, true and false are keywords in C23 */
#endif
#include <stddef.h>
#include <sys/types.h>

/**
 * @file ctls.h
 * @brief INTERNAL ONLY. An OpenSSL wrapper that doesn't depend on a
 *        reactor. It configures a TLS context (certificates, trust,
 *        Application-Layer Protocol Negotiation (ALPN) and dispatch by
 *        name) and gives a step-driven, non-blocking API for the handshake,
 *        the reads and the writes of one connection, in client mode and in
 *        server mode.
 *
 * This isn't a public collections module and has no type-inferred macros.
 * No public header includes it (neither chttp.h, chttpclient.h nor
 * chttpserver.h); only src/chttpclient.c and src/chttpserver.c are meant to
 * #include it.
 *
 * Design notes for whoever extends this module later:
 *
 * - Every connection reads and writes its socket through this module's own
 *   socket BIO, whose writes are send(2) calls with MSG_NOSIGNAL. A write to
 *   a peer that has gone away, including the close_notify of
 *   ctls_conn_destroy(), fails with EPIPE and never raises SIGPIPE, so the
 *   module needs no particular SIGPIPE disposition and changes none. A
 *   connection's fd must be a socket.
 *
 * - Every function reports failure through its return value (NULL, or a
 *   ccol_retval_t). Nothing here calls exit() or abort() because the caller
 *   gave a bad configuration, such as a certificate file we can't read.
 *   That's on purpose: a bad TLS setup is the caller's input error, and the
 *   caller detects it and reacts by logging, refusing to start or retrying
 *   with another path. It's no reason for this library to stop the whole
 *   process, which may have other, unrelated work in flight.
 *
 * - ctls_ctx_t is a mutable, reference-counted TLS configuration object
 *   protected by a mutex and shared by many ctls_conn_t connections. Five
 *   functions change it: ctls_ctx_cert_add, ctls_ctx_trust,
 *   ctls_ctx_trust_system, ctls_ctx_peer_cert_optional and
 *   ctls_ctx_alpn_add. Each of them triggers a full internal rebuild of the
 *   OpenSSL SSL_CTX objects below it. The rebuild always starts from the
 *   stored configuration and never patches an SSL_CTX in place, which keeps
 *   the invariant simple: the live SSL_CTX is always a pure function of the
 *   stored configuration. The price is one full rebuild per change, which is
 *   fine because these calls happen once at startup and never cost anything
 *   per connection.
 *
 * - Certificates are really dispatched per hostname with Server Name
 *   Indication (SNI). ctls_ctx_cert_add with a non-NULL server_name
 *   dispatches by hostname instead of just calling SSL_CTX_use_certificate
 *   a second time on the same SSL_CTX. That second call would only
 *   overwrite the same OpenSSL certificate-type slot, and even though it
 *   looks like per-name configuration it never dispatches by hostname. Each
 *   named certificate gets its own fully built SSL_CTX. An
 *   SSL_CTX_set_tlsext_servername_callback is installed once on the
 *   context's default SSL_CTX, and that callback moves the live SSL object
 *   onto the matching named SSL_CTX with SSL_set_SSL_CTX() as soon as a
 *   ClientHello's SNI extension names that certificate, and we built it
 *   that way on purpose; it doesn't just fall out of how certificates are
 *   stored.
 *
 * - ALPN protocol selection (ctls_ctx_alpn_add) fires its on_selected
 *   callback synchronously, from inside the call that already drives the
 *   handshake, ctls_conn_handshake_step(). The callback isn't deferred onto
 *   a reactor's task queue: this module doesn't depend on a reactor, so it
 *   has no such queue and doesn't need one. For the caller,
 *   ctls_conn_handshake_step is a synchronous call anyway, from a blocking
 *   loop as well as from an on_readable or on_writable callback of a
 *   ccol_event_loop. So this is a simplification that loses no
 *   functionality, since the caller learns the selected protocol at the
 *   same logical point either way, and no application code in this codebase
 *   drives ALPN selection asynchronously.
 *
 * - Session resumption covers OpenSSL's session cache and TLS 1.3 tickets,
 *   for TLS 1.2 and TLS 1.3, with and without a client certificate, and
 *   across an SNI dispatch to a named certificate. A configured trust store
 *   implies SSL_VERIFY_PEER, which makes a server ask the client for a
 *   certificate, and OpenSSL refuses to resume any session on such a
 *   context unless a session id context is set, so we set one on every
 *   SSL_CTX we build. Five explicit settings are applied in all
 *   (SSL_MODE_ENABLE_PARTIAL_WRITE, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER,
 *   SSL_CTX_set_min_proto_version (TLS1_2_VERSION), SSL_OP_NO_COMPRESSION
 *   and SSL_CTX_set_session_id_context) and everything else is left alone.
 *   Every configuration change of a context builds new SSL_CTX objects,
 *   with a new session cache and new ticket keys, so a session from before
 *   such a change ends in a full handshake, never in a failure.
 */

/**
 * @brief An opaque, reference-counted TLS configuration object holding the
 *        certificates, the trust store and the ALPN protocols.
 */
typedef struct ctls_ctx ctls_ctx_t;

/**
 * @brief An opaque handle for one connection, holding one OpenSSL SSL
 *        object and one BIO.
 */
typedef struct ctls_conn ctls_conn_t;

/** @brief The result of one non-blocking handshake step. See
 *         ctls_conn_handshake_step(). */
typedef enum ctls_handshake_result {
  CTLS_HANDSHAKE_DONE = 0,   /**< The handshake ended. conn can do I/O. */
  CTLS_HANDSHAKE_WANT_READ,  /**< Call again after the fd becomes readable. */
  CTLS_HANDSHAKE_WANT_WRITE, /**< Call again after the fd becomes writable. */
  CTLS_HANDSHAKE_ERROR       /**< A fatal error. Destroy the connection. */
} ctls_handshake_result_t;

/**
 * @brief The ALPN protocol-selection callback. ctls_conn_handshake_step()
 *        fires it synchronously, in the call that completes a handshake
 *        which agreed on this entry's protocol: in server mode when the
 *        client's offer names this entry, and in client mode when the
 *        server picks this entry's protocol. It fires once the TLS library
 *        is done with conn for this step, so the callback may destroy conn,
 *        as long as the caller of ctls_conn_handshake_step() stops using it
 *        afterwards.
 *
 * Some handshakes agree on no protocol at all, either because the peer
 * offers nothing this context registered or because a server answers
 * without an ALPN extension. Such a handshake fires no callback and
 * ctls_conn_alpn_selected() reports NULL, so "the two sides agreed on this
 * protocol" and "the two sides agreed on nothing" never look the same to
 * the caller.
 *
 * @param conn           The connection the protocol was selected for. Use
 *                        ctls_conn_udata() to get the context the caller
 *                        attached to the connection when creating it.
 * @param protocol_name  The selected protocol name. It is NOT
 *                        NUL-terminated, since these are raw TLS wire
 *                        bytes, so always use protocol_len.
 * @param protocol_len   The byte length of protocol_name.
 * @param alpn_udata     The udata pointer the caller passed to
 *                        ctls_ctx_alpn_add() for this protocol
 *                        registration.
 */
typedef void (*ctls_alpn_selected_fn)(ctls_conn_t *conn,
                                      const char *protocol_name,
                                      size_t protocol_len, void *alpn_udata);

/**
 * @brief The cleanup callback for an ALPN registration's alpn_udata. It's
 *        called when the ctls_ctx_t owning the registration is destroyed,
 *        and when a later ctls_ctx_alpn_add() call replaces the
 *        registration. None of this module's locks are held during the call,
 *        so the callback may call any function of this module on the same
 *        context. It may be NULL.
 */
typedef void (*ctls_alpn_cleanup_fn)(void *alpn_udata);

/* ========================================================================== */
/*                    CONTEXT (ctls_ctx_t) CONSTRUCTION                       */
/* ========================================================================== */

/**
 * @brief Creates a new, empty TLS context with a custom allocator.
 *
 * A new context has no certificate, no trust store and no ALPN protocols.
 * You can use it right away to create client-mode connections that present
 * no certificate, which is the usual case for an outbound HTTPS client that
 * doesn't need mutual TLS. Add a certificate with ctls_ctx_cert_add() before
 * using the context for server-mode connections.
 *
 * @param mp       A custom allocator, or NULL for malloc and free.
 * @param err_str  Optional. Receives a static diagnostic string on failure.
 * @return The new context with a reference count of 1, or NULL if the
 *         allocation fails.
 */
ctls_ctx_t *ctls_ctx_new_mp(ccol_memmgmt_procs_t *mp, char **err_str);

/** @brief Creates a new, empty TLS context with the default allocator. */
static inline __attribute__((always_inline)) ctls_ctx_t *ctls_ctx_new(
    char **err_str) {
  return ctls_ctx_new_mp(NULL, err_str);
}

/**
 * @brief Adds a certificate and key pair to ctx, or replaces one.
 *
 * A server_name of NULL or "" configures the context's DEFAULT certificate.
 * The default certificate is used for client-mode connections, which is how
 * a client presents its own certificate for mutual TLS. It's also used for
 * server-mode connections in two cases: when the peer sends no SNI name, and
 * when no named certificate below matches the SNI name the peer sent. A
 * second call with a server_name of NULL or "" replaces the default
 * certificate.
 *
 * A non-NULL, non-empty server_name registers a named certificate, and if
 * that name is already registered the call replaces the entry. Names are
 * compared case-insensitively. A named certificate is dispatched on an exact
 * hostname match against the SNI extension of the TLS ClientHello, also
 * ignoring case. A leading "*." is a wildcard for one label: "*.example.com"
 * matches "foo.example.com" and "FOO.EXAMPLE.COM" but not "example.com" or
 * "a.b.example.com", the same as the usual wildcard certificates a CA
 * issues. An exact registration always wins over a wildcard registration
 * that also covers the name, whatever order they were registered in, so a
 * host with its own certificate never gets the wildcard's. The first call
 * with a non-empty server_name installs the SNI dispatch callback on ctx,
 * and every call after that only adds another named entry.
 *
 * cert_path and key_path must both be NULL or both be non-NULL.
 * - Both non-NULL: the certificate and the key are loaded from these PEM
 *   files. The files are read once, during this call, and ctx never reads
 *   them again. The two files have to belong together: a certificate whose
 *   public key isn't the public key of this private key is rejected with
 *   ccol_http_tls_cert_load_failed and ctx is left exactly as it was,
 *   instead of committing a configuration that couldn't serve a single
 *   handshake.
 * - Both NULL: a self-signed certificate is generated, as a convenience for
 *   development and for tests. With a non-NULL, non-empty server_name, that
 *   name becomes the certificate's subject, and since the SNI dispatch rules
 *   above apply, the certificate is also registered as a named entry. With a
 *   server_name of NULL or "", a generic fixed subject name is used, because
 *   the default slot has no name of its own to take one from. That's how
 *   you get a self-signed DEFAULT certificate, used when no SNI name
 *   matches.
 *
 * @param ctx          The context to change.
 * @param server_name  See above. It may be NULL.
 * @param cert_path    The path of the PEM certificate file, or NULL. See
 *                     above.
 * @param key_path     The path of the PEM private key file, or NULL. See
 *                     above.
 * @param pk_password  An optional password that decrypts the private key,
 *                     or NULL.
 * @param err_str       Optional. Receives a static diagnostic string on
 *                      failure.
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory, or
 *         ccol_http_tls_cert_load_failed. ccol_invalid_args when ctx is
 *         NULL or when the combination of cert_path, key_path and
 *         server_name isn't valid. ccol_http_tls_cert_load_failed in four
 *         cases: when cert_path or key_path can't be read (including a path
 *         that doesn't name a regular file of at most 16 MB; such a path is
 *         refused at once and never waited on), when a file is malformed,
 *         when the two files name a certificate and a key that don't match,
 *         and when OpenSSL refuses to serve a matching pair (a key below the
 *         security level, or a signature with a digest it rejects). Nothing
 *         is stored in any of these cases. When the SSL_CTX objects can't be
 *         rebuilt with the new certificate, the call gives
 *         ccol_not_enough_memory if an allocation failed and
 *         ccol_http_tls_cert_load_failed otherwise, and it changes nothing:
 *         every name keeps serving the certificate it served before the
 *         call, and a name the call would have added isn't registered.
 */
ccol_retval_t ctls_ctx_cert_add(ctls_ctx_t *ctx, const char *server_name,
                                const char *cert_path, const char *key_path,
                                const char *pk_password, char **err_str);

/**
 * @brief Adds a CA bundle to the trust store of ctx and turns on peer
 *        certificate verification (SSL_VERIFY_PEER) for every connection
 *        that comes from ctx.
 *
 * A client-mode context verifies the server's certificate chain against
 * this bundle. A server-mode context does more: it asks the client for a
 * certificate and, if the client presents one, verifies it against the
 * same bundle. That's what turns on mutual TLS on the server side.
 *
 * By default a server-mode context REQUIRES a client certificate. It sets
 * SSL_VERIFY_FAIL_IF_NO_PEER_CERT next to SSL_VERIFY_PEER, so a client that
 * presents no certificate at all fails the handshake, and so does a client
 * whose certificate doesn't verify against ctx's trust store.
 * ctls_ctx_peer_cert_optional() relaxes the first rule: the server then
 * accepts a client that presents nothing, but still refuses one whose
 * certificate doesn't verify. ctls_conn_peer_cert_verified() tells the two
 * kinds of accepted client apart. A client-mode context is unaffected by
 * either setting, because a server always presents a certificate.
 *
 * You can call this more than once; each call adds to the trust store
 * instead of replacing it.
 *
 * The bundle must contribute at least one certificate. Some files are
 * readable and still parse to no certificate at all, like an empty file, a
 * lone private key or a file that isn't PEM. Such a file is rejected
 * with ccol_http_tls_cert_load_failed and ctx is left exactly as it was, so
 * a trust store that trusts no issuer is never registered while peer
 * verification is on. A file with only CRLs is refused the same way.
 *
 * The bundle may hold certificate revocation lists beside its certificates.
 * When any bundle of ctx holds one, every SSL_CTX that ctx builds checks
 * revocation for the peer's whole chain (X509_V_FLAG_CRL_CHECK and
 * X509_V_FLAG_CRL_CHECK_ALL, as nginx sets for ssl_crl): every certificate
 * of the chain below its trust anchor needs a CRL of its issuer, and a
 * revoked certificate, a missing CRL or an expired CRL fails verification.
 *
 * The path must name a regular file of at most 16 MB. Any other kind of
 * file, a FIFO or a device included, is refused at once with
 * ccol_http_tls_cert_load_failed, and the call never waits on it.
 *
 * @param ctx            The context to change.
 * @param ca_bundle_path A PEM file holding one or more trusted
 *                       certificates, and optionally CRLs.
 * @param err_str        Optional. Receives a static diagnostic string on
 *                        failure.
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory, or
 *         ccol_http_tls_cert_load_failed. A failed rebuild of the SSL_CTX
 *         objects gives ccol_not_enough_memory if an allocation failed and
 *         ccol_http_tls_cert_load_failed otherwise; in that case the bundle
 *         isn't stored and ctx's verify settings stay as they were.
 */
ccol_retval_t ctls_ctx_trust(ctls_ctx_t *ctx, const char *ca_bundle_path,
                             char **err_str);

/**
 * @brief Marks ctx to trust the operating system's default CA store, on top
 *        of the certificates ctls_ctx_trust() added or instead of them. It
 *        also turns on SSL_VERIFY_PEER, the same way ctls_ctx_trust()
 *        does.
 *
 * The default store is OpenSSL's default CA file: the file SSL_CERT_FILE
 * names, or the one OpenSSL was built with when the variable is unset or the
 * process runs set-user-ID or set-group-ID. That file is read through a
 * close-on-exec descriptor and parsed once per process, at the first call of
 * this function, and every later context adds the same parsed certificates
 * (and CRLs) to its own store, so changing the file or SSL_CERT_FILE after
 * that first call has no effect. A failed load isn't remembered. Only when
 * the file is absent or holds no certificate does the context fall back to
 * OpenSSL's hashed directory (SSL_CERT_DIR, or the directory OpenSSL was
 * built with), and OpenSSL then opens that directory's files during the
 * handshakes that need them, with descriptors that aren't close-on-exec.
 *
 * @param ctx The context to change.
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory when an
 *         allocation fails, or ccol_http_tls_cert_load_failed (on a
 *         platform without a configured default CA store, for example). On
 *         failure ctx's verify settings stay as they were.
 */
ccol_retval_t ctls_ctx_trust_system(ctls_ctx_t *ctx);

/**
 * @brief Chooses whether a server-mode connection from ctx that has a trust
 *        store only requests a client certificate (optional true) or
 *        requires one (optional false, the default).
 *
 * The setting applies to every SSL_CTX that ctx builds, those of named
 * certificates included, whether this call comes before or after
 * ctls_ctx_trust(). Client-mode connections ignore it.
 *
 * @param ctx       The context to change.
 * @param optional  true to accept a client that presents no certificate.
 * @return ccol_success, ccol_invalid_args when ctx is NULL, or, when the
 *         rebuild of the SSL_CTX objects fails, ccol_not_enough_memory if
 *         an allocation failed and ccol_http_tls_cert_load_failed
 *         otherwise. In that case ctx keeps its previous setting.
 */
ccol_retval_t ctls_ctx_peer_cert_optional(ctls_ctx_t *ctx, bool optional);

/**
 * @brief Registers an ALPN protocol on ctx.
 *
 * A server-mode context selects the registered protocol that the client
 * offers first. The client's preference order decides, and the order of the
 * registrations means nothing on its own. A handshake that finds no common
 * protocol agrees on nothing at all rather than falling back to some
 * registered protocol; see the doc comment of ctls_alpn_selected_fn.
 *
 * @param ctx             The context to change.
 * @param protocol_name   The protocol name, for example "http/1.1", at most
 *                        255 bytes long.
 * @param on_selected     An optional callback, fired when this protocol is
 *                        selected for a connection. It may be NULL.
 * @param alpn_udata      An opaque pointer passed to on_selected and to
 *                        on_cleanup.
 * @param on_cleanup      An optional cleanup for alpn_udata, called when
 *                        ctx is destroyed and when this registration is
 *                        replaced. It may be NULL.
 * @param err_str         Optional. Receives a static diagnostic string on
 *                        failure.
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory, or
 *         ccol_http_tls_cert_load_failed. ccol_invalid_args when ctx or
 *         protocol_name is NULL, and when protocol_name is empty or longer
 *         than 255 bytes. When ctx's SSL_CTX objects can't be rebuilt with
 *         the new registration, ccol_not_enough_memory if an allocation
 *         failed and ccol_http_tls_cert_load_failed otherwise. On every
 *         failure ctx is unchanged: the new registration isn't stored, so
 *         on_cleanup is never called for alpn_udata, and a registration of
 *         the same name that it would have replaced stays in effect.
 */
ccol_retval_t ctls_ctx_alpn_add(ctls_ctx_t *ctx, const char *protocol_name,
                                ctls_alpn_selected_fn on_selected,
                                void *alpn_udata,
                                ctls_alpn_cleanup_fn on_cleanup,
                                char **err_str);

/** @brief Returns how many ALPN protocols ctx holds. */
size_t ctls_ctx_alpn_count(const ctls_ctx_t *ctx);

/**
 * @brief Increments the reference count of ctx. Pair this call with
 *        ctls_ctx_release().
 *
 * A context is pinned with this call for the whole life of a connection or
 * a request in flight, which protects it against a concurrent configuration
 * change on the same ctx from another thread, such as a new
 * ctls_ctx_cert_add() call.
 *
 * A caller may also change a ctls_ctx_t's configuration while connections
 * that came from it are live. That works independently of this
 * reference count. Every such call rebuilds the context's default SSL_CTX
 * and the SSL_CTX of every named certificate from scratch, and then frees
 * the objects the new ones replace. A live connection holds a reference on
 * every SSL_CTX it can still reach (the SSL_CTX it was created from, and the
 * one an SNI dispatch installed on it, if any) until ctls_conn_destroy()
 * releases them. So a handshake already in flight completes against the
 * configuration it started with.
 */
void ctls_ctx_retain(ctls_ctx_t *ctx);

/**
 * @brief Decrements the reference count of ctx and frees ctx once the
 *        count reaches zero. Calling it with ctx == NULL is safe and does
 *        nothing.
 */
void ctls_ctx_release(ctls_ctx_t *ctx);

/* ========================================================================== */
/*                  CONNECTION (ctls_conn_t) - STEP-DRIVEN API                */
/* ========================================================================== */

/**
 * @brief Creates a client-mode TLS connection over a file descriptor that
 *        is already connected, or that still has a non-blocking connect in
 *        progress.
 *
 * It does no I/O and doesn't take ownership of fd: the caller closes fd
 * after calling ctls_conn_destroy(), exactly as with a plain socket.
 *
 * @param ctx          The context to use. It is retained internally (see
 *                      ctls_ctx_retain()); the caller's own reference
 *                      doesn't change and the caller can release it
 *                      independently.
 * @param fd           The socket file descriptor, connected or with the
 *                      connect still in progress.
 * @param hostname     The server's hostname, or an IP literal. It's used
 *                      for SNI (skipped for an IP literal, as RFC 6066
 *                      says) and, when verify_host is true, to verify the
 *                      certificate's hostname or IP. It may be NULL or
 *                      empty only when verify_host is false, and then no
 *                      SNI is sent.
 * @param verify_host  If true, the certificate's hostname or IP is verified
 *                      against hostname, on top of the chain verification
 *                      that ctx's trust store configuration implies. true
 *                      with a NULL or empty hostname is refused and the
 *                      function returns NULL, so it never connects without
 *                      a name check in that case. Pass false to skip the
 *                      name check on purpose. Hostname verification with no
 *                      chain trust at all gives no real security, since the
 *                      certificate could be a complete forgery, so a caller
 *                      must also call ctls_ctx_trust() or
 *                      ctls_ctx_trust_system().
 * @param err_str      Optional. Receives a static diagnostic string on
 *                      failure.
 * @return The new connection, or NULL on failure: an invalid ctx, an
 *         invalid fd, verify_host true with a NULL or empty hostname, an
 *         allocation failure, or a hostname or IP verification target that
 *         can't be configured.
 */
ctls_conn_t *ctls_conn_create_client(ctls_ctx_t *ctx, int fd,
                                     const char *hostname, bool verify_host,
                                     char **err_str);

/**
 * @brief Creates a server-mode TLS connection over a file descriptor that
 *        the caller has already accepted.
 *
 * @param ctx    The context to use. It must hold at least a default
 *               certificate from ctls_ctx_cert_add(), or the handshake
 *               fails. This function doesn't check that; only the handshake
 *               does, which is where a missing certificate shows up anyway.
 * @param fd     The accepted socket file descriptor.
 * @param udata  An opaque pointer for this connection, which
 *               ctls_conn_udata() gives back, from an ALPN on_selected
 *               callback for example.
 * @param err_str Optional. Receives a static diagnostic string on failure.
 * @return The new connection, or NULL on failure.
 */
ctls_conn_t *ctls_conn_create_server(ctls_ctx_t *ctx, int fd, void *udata,
                                     char **err_str);

/**
 * @brief Drives one non-blocking step of the TLS handshake.
 *
 * Call it again once the fd becomes readable or writable, as the last
 * CTLS_HANDSHAKE_WANT_* result says, and keep going until the result is
 * neither WANT_READ nor WANT_WRITE. It never blocks.
 *
 * By the time it returns CTLS_HANDSHAKE_DONE, the on_selected callback of
 * the agreed ALPN protocol has fired (see ctls_alpn_selected_fn), and the
 * caller can use ctls_conn_read() and ctls_conn_write().
 *
 * @param conn The connection to drive.
 * @return CTLS_HANDSHAKE_DONE, CTLS_HANDSHAKE_WANT_READ,
 *         CTLS_HANDSHAKE_WANT_WRITE, or CTLS_HANDSHAKE_ERROR (fatal;
 *         destroy conn).
 *
 * @note A known third-party limitation, not a bug in this module. When many
 *       concurrent client handshakes share one ctls_ctx_t's trust store (in
 *       the Tier 2 or Tier 3 async engine of chttpclient, for example, which
 *       sends several requests at once to HTTPS origins whose certificates
 *       it verifies), ThreadSanitizer reports a race inside OpenSSL's
 *       certificate machinery: ASN1_STRING_cmp reads a heap block that
 *       ASN1_STRING_set is writing at the same time, a block CRYPTO_malloc
 *       allocated. This module controls one set of objects per connection
 *       (the SSL*, its BIO, and its X509_VERIFY_PARAM from SSL_get0_param),
 *       allocates each of them separately per connection and shares none,
 *       and a mutex (ctls_ctx_t.lock) already protects the SSL_new() call
 *       on the shared SSL_CTX*. The race is inside libcrypto's shared,
 *       reference-counted certificate objects in the trust store, which
 *       OpenSSL's documentation says it protects internally. A program that
 *       links only libssl and libcrypto and runs concurrent client
 *       handshakes against one shared SSL_CTX reproduces the same report,
 *       and neither stack of the race has an application frame in it, so
 *       the calling side can't fix it. We add no workaround and no extra
 *       lock around the verification step on purpose: such a lock would
 *       serialize concurrent handshakes, which defeats the point of this
 *       engine (it drives several handshakes at once), and it would hide
 *       what is very probably somebody else's bug. In practice every
 *       concurrent HTTPS handshake in the sync_tls and async_tls test groups
 *       of tests/chttpclient/tests.c completes successfully, in plain builds
 *       and in -fsanitize=thread builds, including the runs that report
 *       this exact race. Only revisit this note if the race corrupts the
 *       outcome of a handshake, not just because the race detector reports
 *       it.
 */
ctls_handshake_result_t ctls_conn_handshake_step(ctls_conn_t *conn);

/**
 * @brief Reads decrypted application data from conn.
 *
 * It behaves like a non-blocking read(2), with one deliberate exception. A
 * real read(2) returning -1 with errno EWOULDBLOCK or EAGAIN means "call
 * again once the fd is readable", but here it does NOT always mean that.
 * OpenSSL may need to WRITE before this call can make progress, for example
 * to flush a deferred TLS 1.3 session ticket after the handshake, or during
 * a TLS 1.2 renegotiation, and then the same EWOULDBLOCK means "call again
 * once the fd is writable". Call ctls_conn_wants_write() right after this
 * function to tell the two apart before deciding which direction to wait
 * for.
 *
 * Returns the number of bytes read, which may be less than len, or 0 after
 * a clean TLS shutdown that the peer started with close_notify, or after
 * the peer closed the socket without close_notify on a record boundary,
 * which curl and Go also take as the end of the data. A close inside a
 * record is an error. A len of 0 returns 0 and does nothing. On a fatal I/O
 * or protocol error it returns -1 and sets errno, and ctls_conn_wants_write()
 * then means nothing.
 *
 * Only valid after ctls_conn_handshake_step() has returned
 * CTLS_HANDSHAKE_DONE.
 */
ssize_t ctls_conn_read(ctls_conn_t *conn, void *buf, size_t len);

/**
 * @brief Encrypts application data and writes it to conn.
 *
 * It behaves like a non-blocking write(2), with the same deliberate
 * exception ctls_conn_read() documents: -1 with errno EWOULDBLOCK can mean
 * "call again once the fd is READABLE" rather than writable, when OpenSSL
 * has to read before this call can make progress (during a TLS 1.2
 * renegotiation, for example). Call ctls_conn_wants_write() right after
 * this function to tell the two apart.
 *
 * Returns the number of bytes consumed, which may be less than len because
 * SSL_MODE_ENABLE_PARTIAL_WRITE is always set. A len of 0 returns 0 and does
 * nothing. On a fatal I/O or protocol error it returns -1 and sets errno.
 *
 * Only valid after ctls_conn_handshake_step() has returned
 * CTLS_HANDSHAKE_DONE.
 */
ssize_t ctls_conn_write(ctls_conn_t *conn, const void *buf, size_t len);

/**
 * @brief Reports whether conn holds received input that ctls_conn_read() can
 *        take without any new byte from the socket.
 *
 * OpenSSL reads a whole TLS record from the socket before decrypting it, and
 * one record carries up to 16 KB of application data. A ctls_conn_read()
 * with a smaller buffer leaves the rest of that record inside the TLS layer
 * rather than in the socket, so poll(2) and epoll(7) report nothing even
 * though the data is already there, and a caller that waits for the fd to
 * be readable before reading again waits for bytes that never come.
 *
 * Call this before every such wait. While it reports true, call
 * ctls_conn_read() instead of waiting, until that call returns -1 with
 * EWOULDBLOCK. A true answer can also cover the start of a record that
 * isn't complete yet; ctls_conn_read() then returns EWOULDBLOCK at once,
 * and from that point waiting for readiness is correct.
 *
 * @param conn The connection handle. A NULL handle is safe and reports
 *             false.
 * @return true if conn holds input that isn't in the socket.
 */
bool ctls_conn_has_pending_input(const ctls_conn_t *conn);

/**
 * @brief Reports which direction the last ctls_conn_read() or
 *        ctls_conn_write() call on conn needs, when that call returned -1
 *        with errno EWOULDBLOCK.
 *
 * @param conn The connection handle. A NULL handle is safe and reports
 *             false.
 * @return true if that call needs the fd to become WRITABLE before it is
 *         repeated, false if it needs the fd to become READABLE. READABLE is
 *         the ordinary case for ctls_conn_read(), and also the default for a
 *         conn on which no I/O call has been classified yet.
 *
 * It only means something right after a ctls_conn_read() or
 * ctls_conn_write() call that returned -1 with errno EWOULDBLOCK or EAGAIN.
 * A caller that ignores it and always waits for the "home" direction of the
 * function it called (read for ctls_conn_read(), write for
 * ctls_conn_write()) can leave a connection stalled forever the one time
 * OpenSSL needs the opposite direction. The doc comment of ctls_conn_read()
 * explains the mechanism behind this.
 */
bool ctls_conn_wants_write(const ctls_conn_t *conn);

/**
 * @brief Returns the result of the peer certificate verification, as an
 *        OpenSSL X509_V_OK or X509_V_ERR_* value (see <openssl/x509_vfy.h>).
 *
 * Call it after a failed handshake to tell a certificate verification
 * failure apart from any other handshake error.
 */
long ctls_conn_verify_result(const ctls_conn_t *conn);

/**
 * @brief Returns the ALPN protocol name the two sides agreed on for conn, or
 *        NULL if they agreed on none, which happens when the ctx owning conn
 *        holds no ALPN protocols or when the peer or the negotiation picked
 *        nothing.
 *
 * @param conn     The connection to ask.
 * @param len_out  Optional. Receives the byte length of the returned name,
 *                 which is NOT NUL-terminated. It may be NULL.
 */
const char *ctls_conn_alpn_selected(const ctls_conn_t *conn, size_t *len_out);

/**
 * @brief Reports whether conn's peer presented a certificate and that
 *        certificate verified against its context's trust store.
 *
 * It gives false for a NULL conn, before the handshake has completed, for a
 * context without a trust store, and for a peer that presented nothing,
 * which a server only accepts under ctls_ctx_peer_cert_optional(). A resumed
 * session gives the answer of the handshake that established it.
 */
bool ctls_conn_peer_cert_verified(const ctls_conn_t *conn);

/** Length of a SHA-256 digest in bytes. */
#define CTLS_SHA256_LEN 32

/**
 * @brief Returns the DER encoding of the certificate conn's peer presented
 *        and that verified, as ctls_conn_peer_cert_verified() reports it.
 *        Gives NULL, and 0 in *len_out, when that function reports false
 *        and when an allocation fails.
 *
 * The bytes belong to conn and stay valid until conn is destroyed. They're
 * made the first time a caller asks, so the call isn't safe from two threads
 * at once on one conn, which is true of every other call on one conn too.
 *
 * @param conn     The connection to ask.
 * @param len_out  Optional. Receives the byte length of the encoding.
 */
const unsigned char *ctls_conn_peer_cert_der(ctls_conn_t *conn,
                                             size_t *len_out);

/**
 * @brief Writes the SHA-256 digest of the DER encoding that
 *        ctls_conn_peer_cert_der() gives into out.
 *
 * @return true on success. false, with out set to zeroes, when the peer has
 *         no verified certificate, when an allocation fails, and when out
 *         is NULL.
 */
bool ctls_conn_peer_cert_sha256(ctls_conn_t *conn,
                                unsigned char out[CTLS_SHA256_LEN]);

/**
 * @brief Returns the subject of the peer's verified certificate as an RFC
 *        2253 distinguished name, for example "CN=client,O=Example".
 *
 * Every byte above 0x7F and every control character of the name is escaped,
 * so the string is printable ASCII. It belongs to conn and stays valid until
 * conn is destroyed. Gives NULL when the peer has no verified certificate,
 * as ctls_conn_peer_cert_verified() reports it, and when an allocation
 * fails.
 */
const char *ctls_conn_peer_cert_subject(ctls_conn_t *conn);

/** @brief Returns the udata pointer the caller gave to
 *         ctls_conn_create_server(). */
void *ctls_conn_udata(const ctls_conn_t *conn);

/**
 * @brief Destroys conn: frees its OpenSSL state and releases its reference
 *        on the ctls_ctx_t that owns it, without closing the file
 *        descriptor below it. Calling it with conn == NULL is safe and does
 *        nothing.
 *
 * When the handshake completed, it first writes a TLS close_notify to the
 * peer, and whether or not that write succeeds it leaves nothing on the
 * calling thread's OpenSSL error queue; no lock of this module is held while
 * it does that. Then it takes the owning context's lock briefly, and under
 * that lock frees the connection's OpenSSL state and releases the
 * references the connection holds on every SSL_CTX the context built for
 * it. That lock puts this connection's last use of a shared context and the
 * destruction of that same object by a concurrent configuration change in a
 * fixed order. No callback this module calls for the caller runs while that
 * lock is held, so this function can be called from anywhere, an ALPN
 * on_selected callback included, for any connection and for the connection
 * the callback reports.
 */
void ctls_conn_destroy(ctls_conn_t *conn);
