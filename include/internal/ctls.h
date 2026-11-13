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

// Internal only. An OpenSSL wrapper with no reactor of its own: It configures
// a TLS context (certificates, trust, ALPN, dispatch by server name) and
// drives one connection's handshake, reads and writes step by step, without
// blocking, in client and server mode. Only chttpclient.c and chttpserver.c
// include it.
//
// Every connection talks to its socket through our own BIO, which writes with
// send(2) and MSG_NOSIGNAL, so a write to a gone peer (the close_notify of
// ctls_conn_destroy() included) gives EPIPE and never SIGPIPE. The fd must be
// a socket. Failures come back as NULL or a ccol_retval; a bad configuration
// never stops the process.
//
// A ctls_ctx is a refcounted, mutex-protected configuration shared by many
// connections. Each call that changes it rebuilds every SSL_CTX from the
// stored configuration, so the live SSL_CTX objects are always a function of
// that configuration. A named certificate gets its own SSL_CTX, and a
// servername callback on the default SSL_CTX moves the connection there with
// SSL_set_SSL_CTX(). ALPN callbacks fire synchronously inside
// ctls_conn_handshake_step().
//
// Resumption works for TLS 1.2 and 1.3, with or without a client certificate
// and across an SNI dispatch: Every SSL_CTX gets the same session id context,
// which OpenSSL needs to resume on a context with SSL_VERIFY_PEER. Each SSL_CTX
// also gets partial and moving-buffer writes, TLS 1.2 as the minimum and no
// compression. A configuration change builds new SSL_CTX objects (new cache,
// new ticket keys), so an older session gets a full handshake, never a
// failure.

#include <common.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h>
#endif
#include <stddef.h>
#include <sys/types.h>

// A refcounted TLS configuration: Certificates, trust store, ALPN protocols.
typedef struct ctls_ctx ctls_ctx;

// One connection: An SSL object and its BIO.
typedef struct ctls_conn ctls_conn;

// The result of ctls_conn_handshake_step(). WANT_READ and WANT_WRITE say which
// readiness to wait for before the next step; after ERROR destroy the
// connection.
typedef enum ctls_handshake_result {
  CTLS_HANDSHAKE_DONE = 0,
  CTLS_HANDSHAKE_WANT_READ,
  CTLS_HANDSHAKE_WANT_WRITE,
  CTLS_HANDSHAKE_ERROR
} ctls_handshake_result;

// Called from the ctls_conn_handshake_step() that completes a handshake which
// agreed on this registration's protocol (a server when the client offered it,
// a client when the server picked it). The TLS library is done with conn by
// then, so the callback may destroy it, as long as the caller of the step stops
// using it. protocol_name is raw wire bytes, not NUL-terminated. alpn_udata is
// what ctls_ctx_alpn_add() got; ctls_conn_udata() gives the connection's own
// pointer. A handshake that agrees on nothing fires no callback, and
// ctls_conn_alpn_selected() gives NULL.
typedef void (*ctls_alpn_selected_fn)(ctls_conn *conn,
                                      const char *protocol_name,
                                      size_t protocol_len, void *alpn_udata);

// Frees an ALPN registration's alpn_udata when the context is destroyed or a
// later ctls_ctx_alpn_add() replaces it. No lock of this module is held, so it
// may call back into the module. It may be NULL.
typedef void (*ctls_alpn_cleanup_fn)(void *alpn_udata);

// Contexts
//
// Creates an empty context with a refcount of 1; both HTTP modules hold their
// TLS configuration in one. mp is the allocator (NULL for the default). It
// can serve client connections that present no certificate at once; a server
// needs ctls_ctx_cert_add() first. It gives NULL when the allocation fails,
// with a static message in *err_str.
ctls_ctx *ctls_ctx_new_mp(ccol_memmgmt_procs *mp, char **err_str);

// Creates a context like ctls_ctx_new_mp(), with the default allocator.
static inline __attribute__((always_inline)) ctls_ctx *ctls_ctx_new(
    char **err_str) {
  return ctls_ctx_new_mp(NULL, err_str);
}

// Adds or replaces a certificate and key, which a server needs to serve and a
// client needs for mutual TLS. A NULL or empty server_name sets the default
// certificate: A client presents it, and a server uses it when the peer sends
// no SNI name or one that nothing matches. Any other name registers (or
// replaces) a named certificate, matched case-insensitively against the SNI
// name. "*.example.com" matches one label ("foo.example.com", not
// "example.com" or "a.b.example.com"), and an exact name always wins over a
// wildcard.
//
// cert_path and key_path must both be set or both be NULL. Set, the PEM files
// are read once during the call, and a key that doesn't match the certificate
// is refused. NULL generates a self-signed certificate whose subject is
// server_name, or a fixed name for the default slot. pk_password may decrypt
// the key.
//
// It gives ccol_success, ccol_invalid_args (NULL ctx, bad combination),
// ccol_http_tls_cert_load_failed (an unreadable path or one that isn't a
// regular file of at most 16 MB, a malformed file, a mismatched pair, or a
// pair OpenSSL won't serve, such as a weak key or digest) or
// ccol_not_enough_memory. A failed rebuild gives ccol_not_enough_memory for an
// allocation and ccol_http_tls_cert_load_failed otherwise. On any failure
// nothing changes: Every name keeps its old certificate and a new name isn't
// registered.
ccol_retval ctls_ctx_cert_add(ctls_ctx *ctx, const char *server_name,
                              const char *cert_path, const char *key_path,
                              const char *pk_password, char **err_str);

// Adds a PEM CA bundle to the trust store and turns on SSL_VERIFY_PEER, so a
// client can verify a server against a private CA and a server can require
// client certificates. A client verifies the server's chain with it; a server
// asks for a client certificate and by default requires one
// (SSL_VERIFY_FAIL_IF_NO_PEER_CERT), see ctls_ctx_peer_cert_optional(). Calls
// add up.
//
// The bundle must contribute at least one certificate: An empty file, a lone
// key, a file that isn't PEM or one with only CRLs is refused, so verification
// never runs against an empty store. CRLs in any bundle turn on revocation
// checks for the whole chain (X509_V_FLAG_CRL_CHECK and _ALL, as nginx
// ssl_crl does): Each certificate below the anchor needs its issuer's CRL, and
// a revoked certificate, a missing or an expired CRL fails. The path must be a
// regular file of at most 16 MB; anything else is refused at once.
//
// It gives ccol_success, ccol_invalid_args, ccol_not_enough_memory or
// ccol_http_tls_cert_load_failed. On failure the bundle isn't stored and the
// verify settings stay.
ccol_retval ctls_ctx_trust(ctls_ctx *ctx, const char *ca_bundle_path,
                           char **err_str);

// Trusts the system CA store as well and turns on SSL_VERIFY_PEER, which is
// what a client needs for public HTTPS servers. The store is OpenSSL's
// default CA file (SSL_CERT_FILE unless the process is setuid or setgid, else
// the built-in path), read through a close-on-exec descriptor and parsed once
// per process at the first call; later changes of the file or the variable
// have no effect, and a failed load isn't remembered. Only when that file is
// absent or holds no certificate does it use the hashed directory
// (SSL_CERT_DIR or the built-in one), whose files OpenSSL opens during
// handshakes without close-on-exec. It gives ccol_success, ccol_invalid_args,
// ccol_not_enough_memory or ccol_http_tls_cert_load_failed (no default store,
// for example); on failure the verify settings stay.
ccol_retval ctls_ctx_trust_system(ctls_ctx *ctx);

// Chooses whether a server with a trust store only asks for a client
// certificate (optional true; one that's sent must still verify) or requires
// one (false, the default), so a server can serve anonymous clients next to
// authenticated ones. It applies to every SSL_CTX of ctx, before or after
// ctls_ctx_trust(). Clients ignore it. A failed rebuild gives
// ccol_not_enough_memory or ccol_http_tls_cert_load_failed and keeps the old
// setting; NULL ctx gives ccol_invalid_args.
ccol_retval ctls_ctx_peer_cert_optional(ctls_ctx *ctx, bool optional);

// Registers an ALPN protocol (1 to 255 bytes), so the two sides can agree on
// the application protocol during the handshake. A server selects the
// registered protocol the client offers first; registration order doesn't
// matter, and no common protocol means nothing is agreed. on_selected (may be
// NULL) fires with alpn_udata when it's chosen, and on_cleanup (may be NULL)
// frees alpn_udata later. It gives ccol_invalid_args for a NULL ctx or name
// and a bad length, and ccol_not_enough_memory or
// ccol_http_tls_cert_load_failed when the rebuild fails. On failure nothing is
// stored, on_cleanup isn't called and a registration of the same name stays.
ccol_retval ctls_ctx_alpn_add(ctls_ctx *ctx, const char *protocol_name,
                              ctls_alpn_selected_fn on_selected,
                              void *alpn_udata, ctls_alpn_cleanup_fn on_cleanup,
                              char **err_str);

// Gives the number of ALPN protocols ctx holds, so a caller can tell whether
// ALPN is in use.
size_t ctls_ctx_alpn_count(const ctls_ctx *ctx);

// Adds a reference to ctx, so it stays alive for a connection or a request in
// flight; pair it with ctls_ctx_release(). Changing the configuration while
// connections are live is fine independently of this count: The rebuild
// replaces the SSL_CTX objects, and each live connection holds a reference on
// every SSL_CTX it can reach until ctls_conn_destroy(), so a handshake in
// flight finishes with the configuration it started with.
void ctls_ctx_retain(ctls_ctx *ctx);

// Drops a reference to ctx and frees it when the last one goes. NULL is
// fine.
void ctls_ctx_release(ctls_ctx *ctx);

// Connections
//
// Creates a client connection on fd, connected or with a non-blocking connect
// in progress, for an HTTPS request. It does no I/O, retains ctx and doesn't
// own fd (close it after ctls_conn_destroy()). hostname is used for SNI (not
// for an IP literal, as RFC 6066 says) and, with verify_host, for the name
// check. verify_host with a NULL or empty hostname is refused. A name check
// without a trusted chain proves nothing, so call ctls_ctx_trust() or
// ctls_ctx_trust_system() too. It gives NULL, with a message in *err_str, for
// a bad ctx or fd, a refused hostname, an allocation failure or a
// verification target that can't be set.
ctls_conn *ctls_conn_create_client(ctls_ctx *ctx, int fd, const char *hostname,
                                   bool verify_host, char **err_str);

// Creates a server connection on an accepted fd, for an HTTPS server. ctx
// needs a default certificate or the handshake fails (only the handshake
// checks). udata comes back from ctls_conn_udata(). It gives NULL on failure,
// with a message in *err_str.
ctls_conn *ctls_conn_create_server(ctls_ctx *ctx, int fd, void *udata,
                                   char **err_str);

// Drives one non-blocking handshake step, so a reactor or a blocking loop can
// run the handshake without this module waiting. Call it again after the
// readiness the result asks for. On DONE the agreed ALPN callback has fired
// and reads and writes may start.
//
// ThreadSanitizer reports a race inside OpenSSL (ASN1_STRING_cmp against
// ASN1_STRING_set) when many client handshakes share one trust store. Every
// object this module owns is per connection and SSL_new() runs under the
// context lock; a program with only libssl reproduces the report and neither
// stack has a frame of ours. We add no lock around verification on purpose,
// since it would serialize the handshakes. Revisit it only if a handshake
// outcome is ever wrong.
ctls_handshake_result ctls_conn_handshake_step(ctls_conn *conn);

// Reads decrypted data into buf, like a non-blocking read(2), for the HTTP
// modules' receive paths. -1 with EWOULDBLOCK may mean OpenSSL needs to write
// first (a deferred session ticket, a TLS 1.2 renegotiation), so ask
// ctls_conn_wants_write() which way to wait. It gives the bytes read, 0 after
// close_notify or a socket close on a record boundary (as curl and Go
// accept), 0 for a len of 0, or -1 with errno on an error; a close inside a
// record is an error. Valid only after the handshake.
ssize_t ctls_conn_read(ctls_conn *conn, void *buf, size_t len);

// Encrypts and writes len bytes of buf, like a non-blocking write(2), for the
// HTTP modules' send paths. EWOULDBLOCK may mean OpenSSL needs to read first,
// so ask ctls_conn_wants_write(). It gives the bytes taken (partial writes are
// on), 0 for a len of 0, or -1 with errno. Valid only after the handshake.
ssize_t ctls_conn_write(ctls_conn *conn, const void *buf, size_t len);

// Tells whether conn holds input ctls_conn_read() can take without a new
// socket byte. It's needed because OpenSSL reads whole records (up to 16 KB),
// so a small read leaves the rest in the TLS layer where poll(2) can't see it.
// Call this before every wait for readability and read while it's true; it
// can also cover the start of an incomplete record, and then the read gives
// EWOULDBLOCK and waiting is correct. NULL gives false.
bool ctls_conn_has_pending_input(const ctls_conn *conn);

// Tells which readiness the last read or write that gave EWOULDBLOCK needs:
// true for writable, false for readable (also the answer before any I/O).
// Always waiting for the call's own direction can stall a connection forever
// when OpenSSL needs the other one. NULL gives false.
bool ctls_conn_wants_write(const ctls_conn *conn);

// Gives the peer verification result (X509_V_OK or X509_V_ERR_*), so a caller
// can tell a certificate failure from another handshake error.
long ctls_conn_verify_result(const ctls_conn *conn);

// Gives the agreed ALPN protocol, so a caller can pick the protocol to speak
// (not NUL-terminated; its length goes to *len_out, which may be NULL), or
// NULL when nothing was agreed.
const char *ctls_conn_alpn_selected(const ctls_conn *conn, size_t *len_out);

// Tells whether the peer presented a certificate and it verified against the
// trust store, so a server handler can tell an authenticated client from an
// anonymous one. False for NULL, before the handshake is done, without a
// trust store, and for a peer that sent nothing. A resumed session gives the
// answer of the handshake that made it.
bool ctls_conn_peer_cert_verified(const ctls_conn *conn);

// The length of a SHA-256 digest, the size of the buffer
// ctls_conn_peer_cert_sha256() fills.
#define CTLS_SHA256_LEN 32

// Gives the DER encoding of the verified peer certificate, for handlers that
// identify a client by its certificate. The bytes are owned by conn until
// it's destroyed and the length goes to *len_out; NULL and 0 when there's none
// or an allocation fails. It's made on first use, so (like every call on one
// conn) it isn't safe from two threads at once.
const unsigned char *ctls_conn_peer_cert_der(ctls_conn *conn, size_t *len_out);

// Writes the SHA-256 of that DER encoding to out, a fingerprint for pinning or
// logging a client. False, with out zeroed, when there's no verified
// certificate or an allocation fails, and for a NULL out.
bool ctls_conn_peer_cert_sha256(ctls_conn *conn,
                                unsigned char out[CTLS_SHA256_LEN]);

// Gives the verified peer certificate's subject as an RFC 2253 name
// ("CN=client,O=Example"), for authorization and logs. It's escaped to
// printable ASCII and owned by conn. NULL when there's no verified
// certificate or an allocation fails.
const char *ctls_conn_peer_cert_subject(ctls_conn *conn);

// Gives the udata given to ctls_conn_create_server(), so an ALPN callback can
// find the caller's connection object.
void *ctls_conn_udata(const ctls_conn *conn);

// Frees conn and its reference on ctx when a connection ends; it never closes
// the fd. NULL is fine. After a finished handshake it first sends
// close_notify, without holding a lock, and leaves the thread's OpenSSL error
// queue empty either way. Then, briefly under the context lock, it frees the
// SSL state and the references on every SSL_CTX it held, which orders its
// last use of a shared SSL_CTX before a concurrent rebuild can free it. No
// callback runs under that lock, so it may be called from anywhere, an ALPN
// callback included.
void ctls_conn_destroy(ctls_conn *conn);
