/*
Copyright: Boaz Segev, 2018-2019
License: MIT

Feel free to copy, use and enjoy according to the license provided.
*/
#ifndef H_FIO_TLS

/**
 * This is an SSL/TLS extension for the facil.io library.
 */
#define H_FIO_TLS

#include <stdint.h>

#ifndef FIO_TLS_PRINT_SECRET
/* if true, the master key secret should be printed using FIO_LOG_DEBUG */
#define FIO_TLS_PRINT_SECRET 0
#endif

/** An opaque type used for the SSL/TLS functions. */
typedef struct fio_tls_s fio_tls_s;

/**
 * Creates a new SSL/TLS context / settings object with a default certificate
 * (if any).
 *
 * If no server name is provided and no private key and public certificate are
 * provided, an empty TLS object will be created, (maybe okay for clients).
 *
 *      fio_tls_s * tls = fio_tls_new("www.example.com",
 *                                    "public_key.pem",
 *                                    "private_key.pem", NULL );
 */
fio_tls_s *fio_tls_new(const char *server_name, const char *public_cert_file,
                       const char *private_key_file, const char *pk_password);

/**
 * Adds a certificate a new SSL/TLS context / settings object (SNI support).
 *
 *      fio_tls_cert_add(tls, "www.example.com",
 *                            "public_key.pem",
 *                            "private_key.pem", NULL );
 */
void fio_tls_cert_add(fio_tls_s *, const char *server_name,
                      const char *public_cert_file,
                      const char *private_key_file, const char *pk_password);

/**
 * Adds an ALPN protocol callback to the SSL/TLS context.
 *
 * The first protocol added will act as the default protocol to be selected.
 *
 * The `on_selected` callback should accept the `uuid`, the user data pointer
 * passed to either `fio_tls_accept` or `fio_tls_connect` (here:
 * `udata_connetcion`) and the user data pointer passed to the
 * `fio_tls_alpn_add` function (`udata_tls`).
 *
 * The `on_cleanup` callback will be called when the TLS object is destroyed (or
 * `fio_tls_alpn_add` is called again with the same protocol name). The
 * `udata_tls` argument will be passed along, as is, to the callback (if set).
 *
 * Except for the `tls` and `protocol_name` arguments, all arguments can be
 * NULL.
 */
void fio_tls_alpn_add(fio_tls_s *tls, const char *protocol_name,
                      void (*on_selected)(intptr_t uuid, void *udata_connection,
                                          void *udata_tls),
                      void *udata_tls, void (*on_cleanup)(void *udata_tls));

/**
 * Returns the number of registered ALPN protocol names.
 *
 * This could be used when deciding if protocol selection should be delegated to
 * the ALPN mechanism, or whether a protocol should be immediately assigned.
 *
 * If no ALPN protocols are registered, zero (0) is returned.
 */
uintptr_t fio_tls_alpn_count(fio_tls_s *tls);

/**
 * Adds a certificate to the "trust" list, which automatically adds a peer
 * verification requirement.
 *
 * Note, when the fio_tls_s object is used for server connections, this will
 * limit connections to clients that connect using a trusted certificate.
 *
 *      fio_tls_trust(tls, "google-ca.pem" );
 */
void fio_tls_trust(fio_tls_s *, const char *public_cert_file);

/**
 * Establishes an SSL/TLS connection as an SSL/TLS Server, using the specified
 * context / settings object.
 *
 * The `uuid` should be a socket UUID that is already connected to a peer (i.e.,
 * the result of `fio_accept`).
 *
 * The `udata` is an opaque user data pointer that is passed along to the
 * protocol selected (if any protocols were added using `fio_tls_alpn_add`).
 */
void fio_tls_accept(intptr_t uuid, fio_tls_s *tls, void *udata);

/**
 * Increase the reference count for the TLS object.
 *
 * Decrease with `fio_tls_destroy`.
 */
void fio_tls_dup(fio_tls_s *tls);

/**
 * Destroys the SSL/TLS context / settings object and frees any related
 * resources / memory.
 */
void fio_tls_destroy(fio_tls_s *tls);

/**
 * Marks this TLS context as trusting the system's default CA store (in
 * addition to any certificates added via `fio_tls_trust`). Rebuilds the
 * context, same as `fio_tls_trust` does.
 */
void fio_tls_trust_system(fio_tls_s *tls);

/* *****************************************************************************
Client-mode TLS connections over a raw file descriptor

These entry points are for synchronous, thread-per-connection clients (e.g.
c_collections' chttpclient) that drive their own read/write/poll loop and do
NOT run facil.io's reactor (`fio_start`). Unlike `fio_tls_accept`, they never
touch facil.io's uuid/fd table or its reactor primitives -- they operate
directly on a connection object the caller owns and passes back in.
***************************************************************************** */

/** An opaque client-mode TLS connection object. */
typedef struct fio_tls_connection_s fio_tls_connection_s;

typedef enum {
  FIO_TLS_HANDSHAKE_DONE = 0,
  FIO_TLS_HANDSHAKE_WANT_READ,
  FIO_TLS_HANDSHAKE_WANT_WRITE,
  FIO_TLS_HANDSHAKE_ERROR,
} fio_tls_handshake_result_e;

/**
 * Creates a client-mode TLS connection object bound to an already-connected
 * file descriptor `fd`. Does NOT take ownership of `fd` -- the caller must
 * close it only after calling `fio_tls_connection_destroy`.
 *
 * `hostname` (may be NULL) is used for SNI and, if `verify_host` is nonzero,
 * for X.509 hostname verification.
 *
 * Returns NULL on allocation / SSL_new / BIO_new_socket failure.
 */
fio_tls_connection_s *fio_tls_connect_create(fio_tls_s *tls, int fd,
                                             const char *hostname,
                                             uint8_t verify_host);

/**
 * Drives one (non-blocking) step of the client handshake. On
 * FIO_TLS_HANDSHAKE_WANT_READ / _WANT_WRITE, wait for the fd to become
 * readable / writable (e.g. via `poll`) and call this again.
 */
fio_tls_handshake_result_e fio_tls_client_handshake_step(
    fio_tls_connection_s *c);

/**
 * Returns the X.509 verification result (0 / X509_V_OK on success) for a
 * connection whose handshake step returned FIO_TLS_HANDSHAKE_ERROR, so the
 * caller can distinguish a certificate-verification failure from any other
 * TLS error.
 */
long fio_tls_connection_verify_result(fio_tls_connection_s *c);

/**
 * Synchronous read/write, semantics matching a raw `read`/`write` syscall:
 * >0 bytes read/written; 0 = clean EOF; -1 with errno set to EWOULDBLOCK =
 * retry after the fd is ready for the corresponding direction.
 */
ssize_t fio_tls_connection_read(fio_tls_connection_s *c, void *buf, size_t len);
ssize_t fio_tls_connection_write(fio_tls_connection_s *c, const void *buf,
                                 size_t len);

/**
 * Shuts down and frees the SSL object and the connection handle. Does NOT
 * close the underlying fd -- the caller owns the socket's lifecycle.
 */
void fio_tls_connection_destroy(fio_tls_connection_s *c);

#endif
