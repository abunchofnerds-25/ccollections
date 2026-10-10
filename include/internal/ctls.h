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
#include <stdbool.h> /* C23 has bool, true and false as keywords */
#endif
#include <stddef.h>
#include <sys/types.h>

/**
 * @file ctls.h
 * @brief INTERNAL ONLY. An OpenSSL wrapper that does not depend on a
 *        reactor. It configures a TLS context (certificates, trust,
 *        Application-Layer Protocol Negotiation (ALPN) and dispatch by
 *        name) and gives a step-driven, non-blocking API for the handshake,
 *        the read and the write of one connection, in client mode and in
 *        server mode.
 *
 * This is not a public collections module and has no type-inferred macros.
 * No public header includes it (chttp.h, chttpclient.h and chttpserver.h do
 * not either); only src/chttpclient.c and src/chttpserver.c are meant to
 * #include this header.
 *
 * Design notes, for a person who extends this module later:
 *
 * - Every connection reads and writes its socket through a socket BIO of
 *   this module whose writes are send(2) calls with MSG_NOSIGNAL. A write
 *   to a peer that is gone, including the close_notify of
 *   ctls_conn_destroy(), fails with EPIPE and never raises SIGPIPE, so the
 *   module needs no particular disposition of SIGPIPE and changes none.
 *   The fd of a connection must be a socket.
 *
 * - Every function reports a failure with a return value (NULL, or a
 *   ccol_retval_t). Nothing in this module calls exit() or abort() when the
 *   caller gives a bad configuration, such as a certificate file that the
 *   module cannot read. This is deliberate: a bad TLS setup is an input
 *   error of the caller, who detects it and acts on it by writing a log,
 *   refusing to start, or trying again with a different path. It is not a
 *   reason for this library to stop the whole process, which can have
 *   other, unrelated work in flight.
 *
 * - ctls_ctx_t is a mutable TLS configuration object with a reference
 *   count, protected by a mutex and shared by many ctls_conn_t connections.
 *   Five functions change it: ctls_ctx_cert_add, ctls_ctx_trust,
 *   ctls_ctx_trust_system, ctls_ctx_peer_cert_optional and
 *   ctls_ctx_alpn_add. Each one starts a full internal rebuild of the
 *   OpenSSL SSL_CTX objects below it. The rebuild always starts from the
 *   stored configuration and never patches an SSL_CTX in place, which
 *   keeps the invariant simple: the live SSL_CTX is always a pure function
 *   of the stored configuration. The cost is one full rebuild for each
 *   change, which is acceptable because these calls happen one time at
 *   startup and are never a cost for each connection.
 *
 * - The module does a real certificate dispatch for each hostname with
 *   Server Name Indication (SNI). ctls_ctx_cert_add with a server_name that
 *   is not NULL dispatches by hostname instead of merely calling
 *   SSL_CTX_use_certificate a second time on the same SSL_CTX. Such a
 *   second call only overwrites the same OpenSSL certificate-type slot and,
 *   although it looks like a configuration for each name, never dispatches
 *   by hostname. Each named certificate gets its own SSL_CTX, which the
 *   module builds in full. The module installs an
 *   SSL_CTX_set_tlsext_servername_callback one time on the default SSL_CTX
 *   of the context, and that callback moves the live SSL object onto the
 *   matching named SSL_CTX with SSL_set_SSL_CTX() the moment the SNI
 *   extension of a ClientHello names that certificate. This is a deliberate
 *   capability, not a side effect of how the certificates are stored.
 *
 * - ALPN protocol selection (ctls_ctx_alpn_add) fires its on_selected
 *   callback synchronously, from inside the call that already drives the
 *   handshake, ctls_conn_handshake_step(). The module does not defer the
 *   callback onto a task queue of a reactor: it does not depend on a
 *   reactor, so it has no such queue and needs none. For the caller,
 *   ctls_conn_handshake_step is already a synchronous call, both from a
 *   blocking loop and from an on_readable or on_writable callback of a
 *   ccol_event_loop. This is a deliberate simplification, not a cut in
 *   functionality, because the caller learns the selected protocol at the
 *   same logical point in both designs. Also, no application code in this
 *   codebase drives ALPN selection asynchronously.
 *
 * - Session resumption covers the session cache of OpenSSL and TLS 1.3
 *   tickets, for TLS 1.2 and TLS 1.3, with and without a client
 *   certificate, and across an SNI dispatch to a named certificate. A
 *   configured trust store implies SSL_VERIFY_PEER, which makes a server
 *   ask the client for a certificate, and OpenSSL refuses to resume any
 *   session on such a context unless a session id context is set; so the
 *   module sets one on every SSL_CTX that it builds. The module applies
 *   five explicit settings in all (SSL_MODE_ENABLE_PARTIAL_WRITE,
 *   SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER, SSL_CTX_set_min_proto_version
 *   (TLS1_2_VERSION), SSL_OP_NO_COMPRESSION and
 *   SSL_CTX_set_session_id_context) and leaves everything else alone.
 *   Every change of the configuration of a context builds new SSL_CTX
 *   objects, with a new session cache and new ticket keys, so a session
 *   from before such a change ends in a full handshake, never in a failure.
 */

/**
 * @brief An opaque TLS configuration object with a reference count, which
 *        holds the certificates, the trust store and the ALPN protocols.
 */
typedef struct ctls_ctx ctls_ctx_t;

/**
 * @brief An opaque handle for one connection, which holds one OpenSSL SSL
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
 *        fires it synchronously, the moment the two sides agree on a
 *        protocol: in server mode when the offer of the client names this
 *        entry, and in client mode when the server selects the protocol of
 *        this entry.
 *
 * Some handshakes agree on no protocol at all, either because the peer
 * offers nothing that this context registered or because a server answers
 * with no ALPN extension. Such a handshake fires no callback, and
 * ctls_conn_alpn_selected() reports NULL, so "the two sides agreed on this
 * protocol" and "the two sides agreed on nothing" never look the same to
 * the caller.
 *
 * @param conn           The connection that the module selected the
 *                        protocol for. Use ctls_conn_udata() to get the
 *                        context that the caller attached to the connection
 *                        at creation time.
 * @param protocol_name  The selected protocol name. It is NOT
 *                        NUL-terminated, because these are raw TLS wire
 *                        bytes, so always use protocol_len.
 * @param protocol_len   The byte length of protocol_name.
 * @param alpn_udata     The udata pointer that the caller passed to
 *                        ctls_ctx_alpn_add() for this protocol
 *                        registration.
 */
typedef void (*ctls_alpn_selected_fn)(ctls_conn_t *conn,
                                      const char *protocol_name,
                                      size_t protocol_len, void *alpn_udata);

/**
 * @brief The cleanup callback for the alpn_udata of an ALPN registration.
 *        The module calls it when it destroys the ctls_ctx_t that owns the
 *        registration, and when a later ctls_ctx_alpn_add() call replaces
 *        the registration. The module holds none of its locks during the
 *        call, so the callback may call any function of this module on the
 *        same context. It may be NULL.
 */
typedef void (*ctls_alpn_cleanup_fn)(void *alpn_udata);

/* ========================================================================== */
/*                    CONTEXT (ctls_ctx_t) CONSTRUCTION                       */
/* ========================================================================== */

/**
 * @brief Creates a new, empty TLS context with a custom allocator.
 *
 * A new context has no certificate, no trust store and no ALPN protocols.
 * You can use it immediately to create client-mode connections that present
 * no certificate, which is the usual case for an outbound HTTPS client that
 * does not need mutual TLS. Add a certificate with ctls_ctx_cert_add()
 * before you use the context to create server-mode connections.
 *
 * @param mp       A custom allocator, or NULL for malloc and free.
 * @param err_str  Optional. It receives a static diagnostic string on a
 *                 failure.
 * @return The new context, with a reference count of 1, or NULL if the
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
 * A server_name of NULL or "" configures the DEFAULT certificate of the
 * context. The module uses the default certificate for client-mode
 * connections, which is how a client presents its own certificate for
 * mutual TLS. It also uses it for server-mode connections in two cases:
 * when the peer sends no SNI name, and when no named certificate below
 * matches the SNI name that the peer sent. A second call with a server_name
 * of NULL or "" replaces the default certificate.
 *
 * A server_name that is not NULL and not empty registers a named
 * certificate; if the same name is already registered, the call replaces that
 * entry. The comparison of the names ignores the case of the letters. The
 * module dispatches a named certificate on an exact hostname match, which
 * also ignores the case, against the SNI extension of the TLS ClientHello. A
 * leading "*." is a wildcard for one label: "*.example.com" matches
 * "foo.example.com" and "FOO.EXAMPLE.COM", but not "example.com" or
 * "a.b.example.com", which is the same behavior as the usual wildcard
 * certificates that a CA issues. An exact registration always wins over a
 * wildcard registration that also covers the same name, whatever the order
 * of the two registrations, so a host with a certificate of its own never
 * gets the certificate of the wildcard. The first call with a server_name
 * that is not empty installs the SNI dispatch callback on ctx, and each call
 * after that one only adds one more named entry.
 *
 * cert_path and key_path must both be NULL, or both not NULL.
 * - Both are not NULL: the module loads the certificate and the key from
 *   these PEM files. It reads the files one time, during this call, and ctx
 *   does not read them again later. The two files must belong together:
 *   the module rejects a certificate whose public key is not the public key
 *   of this private key, gives ccol_http_tls_cert_load_failed and leaves
 *   ctx exactly as it was, instead of committing a configuration that
 *   cannot serve even one handshake.
 * - Both are NULL: the module generates a self-signed certificate, as a
 *   convenience for development and for tests. If server_name is not NULL
 *   and not empty, the module uses it as the subject of the certificate,
 *   and because the SNI dispatch rules above apply, it also registers the
 *   certificate as a named entry. If server_name is NULL or "", the module
 *   uses a generic, fixed subject name, because the default slot carries no
 *   name of its own to take one from. This is how you get a self-signed
 *   DEFAULT certificate, which the module uses when no SNI name matches.
 *
 * @param ctx          The context to change.
 * @param server_name  See above. It may be NULL.
 * @param cert_path    The path of the PEM certificate file, or NULL. See
 *                     above.
 * @param key_path     The path of the PEM private key file, or NULL. See
 *                     above.
 * @param pk_password  An optional password that decrypts the private key,
 *                     or NULL.
 * @param err_str       Optional. It receives a static diagnostic string on
 *                      a failure.
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory, or
 *         ccol_http_tls_cert_load_failed. The module gives
 *         ccol_invalid_args if ctx is NULL, or if the combination of
 *         cert_path, key_path and server_name is not valid. It gives
 *         ccol_http_tls_cert_load_failed in three cases: when it cannot read
 *         cert_path or key_path (which includes a path that does not name a
 *         regular file of at most 16 MB; such a path is refused at once and
 *         never waited on), when a file is malformed, and when the two
 *         files name a certificate and a key that do not match.
 */
ccol_retval_t ctls_ctx_cert_add(ctls_ctx_t *ctx, const char *server_name,
                                const char *cert_path, const char *key_path,
                                const char *pk_password, char **err_str);

/**
 * @brief Adds a CA bundle to the trust store of ctx, and turns on the
 *        verification of the certificate of the peer (SSL_VERIFY_PEER) for
 *        every connection that comes from ctx.
 *
 * A client-mode context verifies the certificate chain of the server
 * against this bundle. A server-mode context does more: it asks the client
 * for a certificate and, if the client presents one, verifies it against
 * this same bundle. This is what turns on mutual TLS on the server side.
 *
 * A server-mode context REQUIRES a client certificate by default. It sets
 * SSL_VERIFY_FAIL_IF_NO_PEER_CERT next to SSL_VERIFY_PEER, so a client that
 * presents no certificate at all fails the handshake, and so does a client
 * whose certificate does not verify against the trust store of ctx.
 * ctls_ctx_peer_cert_optional() relaxes the first rule: the server then
 * accepts a client that presents nothing, and still refuses one whose
 * certificate does not verify. ctls_conn_peer_cert_verified() tells the two
 * kinds of accepted client apart. A client-mode context is unaffected by
 * either setting, because a server always presents a certificate.
 *
 * You can call this function more than one time: each call adds to the
 * trust store instead of replacing it.
 *
 * The bundle must contribute at least one certificate. Some files are
 * readable and still parse to no certificate at all, such as an empty file,
 * a private key on its own, or a file that is not PEM at all. The module
 * rejects such a file with ccol_http_tls_cert_load_failed and leaves ctx
 * exactly as it was, so it never registers a trust store that trusts no
 * issuer while peer verification is on. A file of CRLs alone is refused in
 * the same way.
 *
 * The bundle can hold certificate revocation lists beside its certificates.
 * When any bundle of ctx holds one, every SSL_CTX that ctx builds checks
 * revocation for the whole chain of the peer (X509_V_FLAG_CRL_CHECK and
 * X509_V_FLAG_CRL_CHECK_ALL, as nginx sets for ssl_crl): every certificate
 * of the chain below its trust anchor needs a CRL of its issuer, and a
 * revoked certificate, a missing CRL or an expired CRL fails verification.
 *
 * The path must name a regular file of at most 16 MB. Any other kind of
 * file, a FIFO or a device included, is refused at once with
 * ccol_http_tls_cert_load_failed, and the call never waits on it.
 *
 * @param ctx            The context to change.
 * @param ca_bundle_path A PEM file that holds one or more trusted
 *                       certificates, and optionally CRLs.
 * @param err_str        Optional. It receives a static diagnostic string on
 *                        a failure.
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory, or
 *         ccol_http_tls_cert_load_failed.
 */
ccol_retval_t ctls_ctx_trust(ctls_ctx_t *ctx, const char *ca_bundle_path,
                             char **err_str);

/**
 * @brief Marks ctx to trust the default CA store of the operating system.
 *        ctx trusts that store in addition to the certificates that
 *        ctls_ctx_trust() added, or instead of them. This function also
 *        turns on SSL_VERIFY_PEER, in the same way as ctls_ctx_trust().
 *
 * @param ctx The context to change.
 * @return ccol_success, ccol_invalid_args, or
 *         ccol_http_tls_cert_load_failed (for example, on a platform that
 *         has no configured default CA store).
 */
ccol_retval_t ctls_ctx_trust_system(ctls_ctx_t *ctx);

/**
 * @brief Chooses whether a server-mode connection from ctx that has a trust
 *        store only requests a client certificate (optional true) or
 *        requires one (optional false, the default).
 *
 * The setting applies to every SSL_CTX that ctx builds, the ones of named
 * certificates included, whether this call comes before or after
 * ctls_ctx_trust(). Client-mode connections ignore it.
 *
 * @param ctx       The context to change.
 * @param optional  true to accept a client that presents no certificate.
 * @return ccol_success, ccol_invalid_args when ctx is NULL, or
 *         ccol_http_tls_cert_load_failed when the rebuild of the SSL_CTX
 *         objects fails. ctx then keeps its previous setting.
 */
ccol_retval_t ctls_ctx_peer_cert_optional(ctls_ctx_t *ctx, bool optional);

/**
 * @brief Registers an ALPN protocol on ctx.
 *
 * A server-mode context selects the registered protocol that the client
 * offers first: the preference order of the client decides, and the order
 * of the registrations carries no meaning of its own. A handshake that
 * finds no common protocol agrees on nothing at all instead of falling back
 * to a registered protocol; see the doc comment of ctls_alpn_selected_fn.
 *
 * @param ctx             The context to change.
 * @param protocol_name   The protocol name, for example "http/1.1". It is
 *                        255 bytes long at most.
 * @param on_selected     An optional callback. The module fires it when it
 *                        selects this protocol for a connection. It may be
 *                        NULL.
 * @param alpn_udata      An opaque pointer that the module passes to
 *                        on_selected and to on_cleanup.
 * @param on_cleanup      An optional cleanup for alpn_udata. The module
 *                        calls it when it destroys ctx, and when it
 *                        replaces this registration. It may be NULL.
 * @param err_str         Optional. It receives a static diagnostic string
 *                        on a failure.
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory, or
 *         ccol_http_tls_cert_load_failed. The module gives ccol_invalid_args
 *         if ctx or protocol_name is NULL, and if protocol_name is empty or
 *         longer than 255 bytes. It gives ccol_http_tls_cert_load_failed
 *         when it cannot rebuild the SSL_CTX objects of ctx with the new
 *         registration, in which case the registration stays recorded.
 */
ccol_retval_t ctls_ctx_alpn_add(ctls_ctx_t *ctx, const char *protocol_name,
                                ctls_alpn_selected_fn on_selected,
                                void *alpn_udata,
                                ctls_alpn_cleanup_fn on_cleanup,
                                char **err_str);

/** @brief Returns the number of ALPN protocols that ctx holds. */
size_t ctls_ctx_alpn_count(const ctls_ctx_t *ctx);

/**
 * @brief Increments the reference count of ctx. Pair this call with
 *        ctls_ctx_release().
 *
 * The module pins a context with this call for the whole life of a
 * connection or a request that is in flight, which protects the context
 * against a concurrent change of the configuration on the same ctx from
 * another thread, such as a new call to ctls_ctx_cert_add().
 *
 * A caller can also change the configuration of a ctls_ctx_t while
 * connections that come from it are live. The module supports this use case
 * on its own, and the support does not depend on this reference count.
 * Every such call rebuilds the default SSL_CTX of the context, and the
 * SSL_CTX of every named certificate, from the start, and then frees the
 * objects that the new ones replace. A live connection holds a reference on
 * each SSL_CTX that it can still reach (the SSL_CTX that it was created
 * from, and the SSL_CTX that an SNI dispatch installed on it, if there is
 * one) until ctls_conn_destroy() releases them. As a result, a handshake
 * that is already in flight completes against the configuration that it
 * started on.
 */
void ctls_ctx_retain(ctls_ctx_t *ctx);

/**
 * @brief Decrements the reference count of ctx, and frees ctx when the
 *        count becomes zero. A call with ctx == NULL is safe and does
 *        nothing.
 */
void ctls_ctx_release(ctls_ctx_t *ctx);

/* ========================================================================== */
/*                  CONNECTION (ctls_conn_t) - STEP-DRIVEN API                */
/* ========================================================================== */

/**
 * @brief Creates a client-mode TLS connection over a file descriptor that
 *        is already connected, or on which a non-blocking connect is still
 *        in progress.
 *
 * This function does no I/O and does not take ownership of fd: the caller
 * closes fd after it calls ctls_conn_destroy(), exactly as with a plain
 * socket.
 *
 * @param ctx          The context to use. The module retains it internally
 *                      (see ctls_ctx_retain()); the reference of the caller
 *                      does not change, and the caller can release it on
 *                      its own.
 * @param fd           The socket file descriptor, which is connected or on
 *                      which the connect is still in progress.
 * @param hostname     The hostname of the server, or an IP literal. The
 *                      module uses it for SNI (which it skips for an IP
 *                      literal, as RFC 6066 says) and, if verify_host is
 *                      true, to verify the hostname or the IP of the
 *                      certificate. It may be NULL or empty only when
 *                      verify_host is false, and the module then sends no
 *                      SNI.
 * @param verify_host  If true, the module verifies the hostname or the IP
 *                      of the certificate against hostname, in addition to
 *                      the chain verification that the trust store
 *                      configuration of ctx implies. A true value with a
 *                      NULL or empty hostname is refused and the function
 *                      returns NULL, so it never connects with no check of
 *                      the name in that case. Pass false to skip the check
 *                      of the name on purpose. Note: hostname verification
 *                      with no chain trust at all gives no real security,
 *                      because the certificate can be a complete forgery,
 *                      so a caller must also call ctls_ctx_trust() or
 *                      ctls_ctx_trust_system().
 * @param err_str      Optional. It receives a static diagnostic string on a
 *                      failure.
 * @return The new connection, or NULL on a failure: an invalid ctx, an
 *         invalid fd, a verify_host of true with a NULL or empty hostname,
 *         an allocation failure, or a hostname or IP verification target
 *         that the module cannot configure.
 */
ctls_conn_t *ctls_conn_create_client(ctls_ctx_t *ctx, int fd,
                                     const char *hostname, bool verify_host,
                                     char **err_str);

/**
 * @brief Creates a server-mode TLS connection over a file descriptor that
 *        the caller already accepted.
 *
 * @param ctx    The context to use. It must hold at least a default
 *               certificate from ctls_ctx_cert_add(), or the handshake
 *               fails. This function does not check this; only the
 *               handshake does, which is where an absent certificate shows
 *               itself in any case.
 * @param fd     The accepted socket file descriptor.
 * @param udata  An opaque pointer for this connection. ctls_conn_udata()
 *               gives it back, for example from an ALPN on_selected
 *               callback.
 * @param err_str Optional. It receives a static diagnostic string on a
 *                failure.
 * @return The new connection, or NULL on a failure.
 */
ctls_conn_t *ctls_conn_create_server(ctls_ctx_t *ctx, int fd, void *udata,
                                     char **err_str);

/**
 * @brief Drives one non-blocking step of the TLS handshake.
 *
 * The caller calls this function again after the fd becomes readable or
 * writable, as the last CTLS_HANDSHAKE_WANT_* result says, and repeats this
 * until the result is neither WANT_READ nor WANT_WRITE. This function never
 * blocks.
 *
 * By the time this function returns CTLS_HANDSHAKE_DONE, the on_selected
 * callback of the agreed ALPN protocol has fired (see
 * ctls_alpn_selected_fn), and the caller can use ctls_conn_read() and
 * ctls_conn_write().
 *
 * @param conn The connection to drive.
 * @return CTLS_HANDSHAKE_DONE, CTLS_HANDSHAKE_WANT_READ,
 *         CTLS_HANDSHAKE_WANT_WRITE, or CTLS_HANDSHAKE_ERROR (fatal;
 *         destroy conn).
 *
 * @note This is a known limitation of a third party, not a bug in this
 *       module. When many concurrent client handshakes share the trust
 *       store of one ctls_ctx_t (for example in the Tier 2 or Tier 3 async
 *       engine of chttpclient, which sends several requests at one time to
 *       HTTPS origins whose certificates it verifies), ThreadSanitizer
 *       reports a race inside the certificate machinery of OpenSSL: in that
 *       report, ASN1_STRING_cmp reads a heap block that ASN1_STRING_set
 *       writes at the same time, and CRYPTO_malloc allocated that block.
 *       This module controls one set of objects for each connection (the
 *       SSL*, its BIO, and its X509_VERIFY_PARAM, which SSL_get0_param
 *       gives), allocates all of them separately for each connection and
 *       shares none of them, and a mutex (ctls_ctx_t.lock) already protects
 *       the SSL_new() call on the shared SSL_CTX*. The race is inside the
 *       shared certificate objects of libcrypto in the trust store, which
 *       have a reference count and which, as the documents of OpenSSL say,
 *       OpenSSL protects internally. A program that links only libssl and
 *       libcrypto and runs concurrent client handshakes against one shared
 *       SSL_CTX reproduces the same report, and neither stack in the race
 *       holds an application frame, so the calling side cannot fix this.
 *       This module deliberately adds no work-around and no extra lock
 *       around the verification step: such a lock makes concurrent
 *       handshakes serial, which defeats the purpose of this engine (it
 *       drives several handshakes at one time), and it hides what is very
 *       probably a bug of somebody else. In practice, every concurrent
 *       HTTPS handshake in the sync_tls and async_tls test groups of
 *       tests/chttpclient/tests.c completes successfully, in plain builds
 *       and in -fsanitize=thread builds, including the runs that report
 *       this exact race. Look at this note again only if the race corrupts
 *       the outcome of a handshake, not merely because the race detector
 *       reports the race.
 */
ctls_handshake_result_t ctls_conn_handshake_step(ctls_conn_t *conn);

/**
 * @brief Reads decrypted application data from conn.
 *
 * This function behaves like a non-blocking read(2), with one deliberate
 * exception. A real read(2) that returns -1 with errno EWOULDBLOCK or
 * EAGAIN means "call again after the fd becomes readable", but here it does
 * NOT always mean that: OpenSSL can need to WRITE before this call can make
 * progress, for example to flush a deferred TLS 1.3 session ticket that
 * comes after the handshake, or during a TLS 1.2 renegotiation. In these
 * cases the same EWOULDBLOCK return means "call again after the fd becomes
 * writable". Call ctls_conn_wants_write() immediately after this function
 * to tell the two cases apart, before you decide which direction to wait
 * for.
 *
 * The function returns the number of bytes that it read, which can be less
 * than len, or 0 after a clean TLS shutdown that the peer started with
 * close_notify. It returns -1 and sets errno for a fatal error of the I/O or
 * of the protocol, and in that case the return value of
 * ctls_conn_wants_write() has no meaning.
 *
 * This function is valid only after ctls_conn_handshake_step() returns
 * CTLS_HANDSHAKE_DONE.
 */
ssize_t ctls_conn_read(ctls_conn_t *conn, void *buf, size_t len);

/**
 * @brief Encrypts application data and writes it to conn.
 *
 * This function behaves like a non-blocking write(2), with the same
 * deliberate exception that ctls_conn_read() documents: a return of -1 with
 * errno EWOULDBLOCK can mean "call again after the fd becomes READABLE"
 * instead of writable, when OpenSSL must read before this call can make
 * progress (for example during a TLS 1.2 renegotiation). Call
 * ctls_conn_wants_write() immediately after this function to tell the two
 * cases apart.
 *
 * The function returns the number of bytes that it consumed, which can be
 * less than len because SSL_MODE_ENABLE_PARTIAL_WRITE is always set. A len
 * of 0 returns 0 and does nothing. The function returns -1 and sets errno
 * for a fatal error of the I/O or of the protocol.
 *
 * This function is valid only after ctls_conn_handshake_step() returns
 * CTLS_HANDSHAKE_DONE.
 */
ssize_t ctls_conn_write(ctls_conn_t *conn, const void *buf, size_t len);

/**
 * @brief Reports whether conn holds received input that a ctls_conn_read()
 *        can take without any new byte from the socket.
 *
 * OpenSSL reads a whole TLS record from the socket before it decrypts it,
 * and one record carries up to 16 KB of application data. A ctls_conn_read()
 * with a smaller buffer leaves the rest of that record inside the TLS layer
 * instead of in the socket, so poll(2) and epoll(7) report nothing although
 * the data is already there, and a caller that waits for readiness of the
 * fd before it reads again waits for bytes that never come.
 *
 * Call this function before every such wait. While it reports true, call
 * ctls_conn_read() instead of waiting, until that call returns -1 with
 * EWOULDBLOCK. A true answer can also cover the start of a record that is
 * not complete yet; ctls_conn_read() then returns EWOULDBLOCK at once, and
 * a wait for readiness is correct from that point on.
 *
 * @param conn The connection handle. A NULL handle is safe and reports
 *             false.
 * @return true if conn holds input that is not in the socket.
 */
bool ctls_conn_has_pending_input(const ctls_conn_t *conn);

/**
 * @brief Reports the direction that the last ctls_conn_read() or
 *        ctls_conn_write() call on conn needs, when that call returned -1
 *        with errno EWOULDBLOCK.
 *
 * @param conn The connection handle. A NULL handle is safe and reports
 *             false.
 * @return true if that call needs the fd to become WRITABLE before the
 *         caller repeats it, and false if it needs the fd to become
 *         READABLE. The READABLE case is the ordinary one for
 *         ctls_conn_read(), and also the default for a conn on which no I/O
 *         call is classified yet.
 *
 * This function has a meaning only immediately after a ctls_conn_read() or
 * ctls_conn_write() call that returned -1 with errno EWOULDBLOCK or EAGAIN.
 * A caller that ignores this report and always waits for the "home"
 * direction of the function that it called (the read direction for
 * ctls_conn_read() and the write direction for ctls_conn_write()) can leave
 * a connection stalled forever, the one time that OpenSSL needs the
 * opposite direction. See the doc comment of ctls_conn_read() for the
 * mechanism below this.
 */
bool ctls_conn_wants_write(const ctls_conn_t *conn);

/**
 * @brief Returns the result of the verification of the certificate of the
 *        peer, as an OpenSSL X509_V_OK or X509_V_ERR_* value (see
 *        <openssl/x509_vfy.h>).
 *
 * Call this function after a handshake fails, to tell a failure of the
 * certificate verification apart from another handshake error.
 */
long ctls_conn_verify_result(const ctls_conn_t *conn);

/**
 * @brief Returns the ALPN protocol name that the two sides agreed on for
 *        conn, or NULL if they agreed on no protocol, which happens when
 *        the ctx that owns conn holds no ALPN protocols or when the peer or
 *        the negotiation selected nothing.
 *
 * @param conn     The connection to ask.
 * @param len_out  Optional. It receives the byte length of the name that
 *                 this function returns, which is NOT NUL-terminated. It
 *                 may be NULL.
 */
const char *ctls_conn_alpn_selected(const ctls_conn_t *conn, size_t *len_out);

/**
 * @brief Reports whether the peer of conn presented a certificate and that
 *        certificate verified against the trust store of its context.
 *
 * It gives false for a NULL conn, before the handshake completes, for a
 * context with no trust store, and for a peer that presented nothing, which
 * a server accepts only under ctls_ctx_peer_cert_optional(). The answer of a
 * resumed session is the answer of the handshake that established it.
 */
bool ctls_conn_peer_cert_verified(const ctls_conn_t *conn);

/** The length of a SHA-256 digest, in bytes. */
#define CTLS_SHA256_LEN 32

/**
 * @brief Returns the DER encoding of the certificate that the peer of conn
 *        presented and that verified, as ctls_conn_peer_cert_verified()
 *        reports it. Gives NULL, and 0 in *len_out, when that function
 *        reports false, and when an allocation fails.
 *
 * The bytes belong to conn and stay valid until conn is destroyed. The
 * module makes them the first time that a caller asks, so the call is not
 * safe from two threads at the same time on one conn, as no other call on
 * one conn is.
 *
 * @param conn     The connection to ask.
 * @param len_out  Optional. It receives the byte length of the encoding.
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
 * @brief Returns the subject of the verified certificate of the peer as an
 *        RFC 2253 distinguished name, for example "CN=client,O=Example".
 *
 * Every byte above 0x7F and every control character of the name is escaped,
 * so the string is printable ASCII. It belongs to conn and stays valid until
 * conn is destroyed. Gives NULL when the peer has no verified certificate,
 * as ctls_conn_peer_cert_verified() reports it, and when an allocation
 * fails.
 */
const char *ctls_conn_peer_cert_subject(ctls_conn_t *conn);

/** @brief Returns the udata pointer that the caller gave to
 *         ctls_conn_create_server(). */
void *ctls_conn_udata(const ctls_conn_t *conn);

/**
 * @brief Destroys conn: frees the OpenSSL state of conn and releases the
 *        reference of conn on the ctls_ctx_t that owns it, without closing
 *        the file descriptor below conn. A call with conn == NULL is safe
 *        and does nothing.
 *
 * The function first writes a TLS close_notify to the peer when the
 * handshake completed, and it leaves nothing on the OpenSSL error queue of
 * the calling thread, whether or not that write succeeds; it holds no lock
 * of this module while it does this. Then it takes the lock of the context
 * that owns conn for a short time, and under that lock frees the OpenSSL
 * state of the connection and releases the references that the connection
 * holds on every SSL_CTX that the context built for it. That lock puts the
 * last use of a shared context by this connection and the destruction of
 * that same object by a concurrent change of the configuration in a fixed
 * order. No callback that this module calls for the caller runs while the
 * module holds that lock, so a caller can call this function from
 * anywhere, including an ALPN callback.
 */
void ctls_conn_destroy(ctls_conn_t *conn);
