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

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h> /* C23 has bool, true and false as keywords */
#endif
#include <stddef.h>

#include "chashmap.h"

/* Everything that this header declares from here to the end is part of the
 * public Application Binary Interface (ABI) of libccollections, and the shared
 * library exports all of it. Because the library build uses
 * -fvisibility=hidden, a function or an object that is not inside one of these
 * blocks stays internal to the library: its name is not in the dynamic symbol
 * table of the library, the application that links against the library cannot
 * interpose it, and a symbol with the same name in that application cannot
 * collide with it. */
#pragma GCC visibility push(default)

/**
 * @file chttp.h
 * @brief The common types that chttpclient and chttpserver share.
 *
 * Include this header directly only if your code needs the shared types but
 * not one of the two sub-modules. Both chttpclient.h and chttpserver.h
 * include it automatically.
 */

/* ========================================================================== */
/*                         HTTP METHODS                                       */
/* ========================================================================== */

typedef enum chttp_method {
  CHTTP_GET = 0,
  CHTTP_POST,
  CHTTP_PUT,
  CHTTP_DELETE,
  CHTTP_PATCH,
  CHTTP_HEAD,
  CHTTP_OPTIONS,
  /**
   * A wildcard for the server side: it matches any of the seven concrete
   * methods above when it is the method argument to
   * chttpsvr_register_handler, chttpsvr_register_streaming_handler,
   * chttpsvr_router_on, or chttpsvr_router_on_stream. It is not a valid
   * method for a client request; if you give it to chttp_request_new or to
   * chttp_run_query, the behaviour is undefined. It is only a placeholder
   * that you use when you register a handler, and never the method of a
   * real incoming request: chttpsvr_req_method never gives CHTTP_ANY back,
   * and it never gives back a value outside the seven concrete constants
   * above. A request whose method chttpserver does not recognize at all (a
   * WebDAV verb, TRACE, CONNECT, a custom verb, and others) never reaches a
   * handler, even one that you register with CHTTP_ANY, because chttpserver
   * rejects it with 501 Not Implemented before the route match runs.
   */
  CHTTP_ANY
} chttp_method_t;

/**
 * @brief Give the canonical uppercase method string for a chttp_method_t.
 *
 * For CHTTP_ANY it gives "ANY", although CHTTP_ANY is a sentinel for a route
 * on the server side and not a real HTTP method.
 */
static inline const char *chttp_method_str(chttp_method_t m) {
  switch (m) {
    case CHTTP_GET:
      return "GET";
    case CHTTP_POST:
      return "POST";
    case CHTTP_PUT:
      return "PUT";
    case CHTTP_DELETE:
      return "DELETE";
    case CHTTP_PATCH:
      return "PATCH";
    case CHTTP_HEAD:
      return "HEAD";
    case CHTTP_OPTIONS:
      return "OPTIONS";
    case CHTTP_ANY:
      return "ANY";
    default:
      return "UNKNOWN";
  }
}

/* ========================================================================== */
/*                         HTTP STATUS CODES                                  */
/* ========================================================================== */

/* 1xx - Informational */
#define CHTTP_STATUS_CONTINUE 100
#define CHTTP_STATUS_SWITCHING_PROTOCOLS 101

/* 2xx - Success */
#define CHTTP_STATUS_OK 200
#define CHTTP_STATUS_CREATED 201
#define CHTTP_STATUS_ACCEPTED 202
#define CHTTP_STATUS_NO_CONTENT 204
#define CHTTP_STATUS_PARTIAL_CONTENT 206

/* 3xx - Redirection */
#define CHTTP_STATUS_MOVED_PERMANENTLY 301
#define CHTTP_STATUS_FOUND 302
#define CHTTP_STATUS_NOT_MODIFIED 304
#define CHTTP_STATUS_TEMPORARY_REDIRECT 307
#define CHTTP_STATUS_PERMANENT_REDIRECT 308

/* 4xx - Client Errors */
#define CHTTP_STATUS_BAD_REQUEST 400
#define CHTTP_STATUS_UNAUTHORIZED 401
#define CHTTP_STATUS_FORBIDDEN 403
#define CHTTP_STATUS_NOT_FOUND 404
#define CHTTP_STATUS_METHOD_NOT_ALLOWED 405
#define CHTTP_STATUS_NOT_ACCEPTABLE 406
#define CHTTP_STATUS_REQUEST_TIMEOUT 408
#define CHTTP_STATUS_CONFLICT 409
#define CHTTP_STATUS_GONE 410
#define CHTTP_STATUS_PAYLOAD_TOO_LARGE 413
#define CHTTP_STATUS_UNSUPPORTED_MEDIA 415
#define CHTTP_STATUS_UNPROCESSABLE 422
#define CHTTP_STATUS_TOO_MANY_REQUESTS 429

/* 5xx - Server Errors */
#define CHTTP_STATUS_INTERNAL_ERROR 500
#define CHTTP_STATUS_NOT_IMPLEMENTED 501
#define CHTTP_STATUS_BAD_GATEWAY 502
#define CHTTP_STATUS_SERVICE_UNAVAILABLE 503
#define CHTTP_STATUS_GATEWAY_TIMEOUT 504
#define CHTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED 505

/* ========================================================================== */
/*                         TLS CONFIGURATION                                  */
/* ========================================================================== */

/**
 * @brief The TLS configuration that chttpclient and chttpserver both use.
 *
 * A sub-module ignores each field that does not apply to it.
 * Client-side fields: insecure_skip_verify, insecure_skip_hostname_check,
 *                     ca_bundle_path, cert_path/key_path (mutual TLS).
 * Server-side fields: cert_path, key_path, ca_bundle_path,
 *                     client_cert_optional.
 *
 * A struct with every byte zero is the SAFE configuration. That is deliberate,
 * and it is why each of the three verification fields is named for what
 * turning it ON gives up rather than for what it asks for: the idiomatic ways
 * to build a struct in C all leave a field that the caller did not name as
 * zero:
 *
 *     chttp_tls_config_t a = {0};
 *     chttp_tls_config_t b = {.ca_bundle_path = "/etc/ssl/my-ca.pem"};
 *     chttp_tls_config_t c = {.cert_path = crt, .key_path = key};
 *
 * On a client, all three verify the certificate chain of the server AND
 * match its hostname; on a server, a ca_bundle_path requires every client to
 * present a certificate that verifies against it. So a caller can never end
 * up with no verification by writing less than they meant to: weakening a
 * check takes an explicit assignment, and the name of the field says what it
 * costs where it is read.
 *
 * cert_path, key_path and ca_bundle_path must each name a regular file of at
 * most 16 MB. A path that names a directory, a FIFO or a device is refused at
 * once, before a byte is read, as a load failure.
 *
 * A ca_bundle_path holds one or more CA certificates in PEM form, and may
 * hold certificate revocation lists (CRLs) beside them; a file of CRLs alone
 * names no CA and is refused. When the bundle holds a CRL, revocation is
 * checked for the whole chain that the peer presents, as the ssl_crl
 * directive of nginx does: every certificate of that chain below its trust
 * anchor needs a CRL of its issuer in the bundle. The handshake then fails
 * for a revoked certificate, and also for a certificate whose issuer has no
 * CRL in the bundle and for a CRL past its next update time, because a chain
 * that cannot be checked is not accepted. A bundle with no CRL checks no
 * revocation. The bundle is read when the configuration is applied, so a
 * renewed CRL takes effect at the next chttpsvr_start() of a server, or the
 * next chttpclient_set_tls() of a client.
 */
typedef struct chttp_tls_config {
  const char *cert_path; /* server cert / client cert for mTLS (NULL = none) */
  const char *key_path;  /* server key  / client key  for mTLS (NULL = none) */
  const char *ca_bundle_path; /* custom CA bundle path; NULL = system default */
  /* Client only. False, which is what a zero-initialised struct carries,
   * verifies the certificate chain of the server, against ca_bundle_path when
   * one is set and against the system trust store otherwise.
   *
   * True accepts ANY certificate from ANY peer, so an attacker who can
   * intercept the connection can read and rewrite every byte of it. Set it
   * only where that is genuinely acceptable, such as a test against a
   * throwaway self-signed server; it is never right in production.
   *
   * True also makes insecure_skip_hostname_check irrelevant: a hostname
   * matched against a certificate that nothing validated proves nothing,
   * because an attacker can forge that whole certificate. There is
   * deliberately no way to ask for the hostname check WITHOUT the chain
   * check; that combination reads as a partial measure and provides none.
   *
   * Setting this true together with ca_bundle_path is a contradiction: the
   * bundle exists to be verified against. chttpclient_set_tls() refuses that
   * combination with ccol_invalid_args rather than silently picking one. */
  bool insecure_skip_verify;
  /* Client only. False, which is what a zero-initialised struct carries,
   * matches the hostname or the IP literal of the request against the subject
   * of the certificate that the peer presented.
   *
   * True keeps the full chain verification of the field above and drops only
   * that match. It is the narrower of the two relaxations, and it has real
   * uses: reaching a host by an address or an internal name that its
   * certificate does not carry, while still requiring that the certificate
   * chains to a CA you pinned. It still leaves the connection open to any
   * peer that holds ANY certificate from that CA, so it is only as strong as
   * the narrowest trust store you can give ca_bundle_path.
   *
   * This field has no effect when insecure_skip_verify is true, because
   * nothing is verified there at all. */
  bool insecure_skip_hostname_check;
  /* Server only, and it matters only beside ca_bundle_path. False, which is
   * what a zero-initialised struct carries, is mutual TLS that the server
   * enforces: the handshake of a client that presents no certificate fails,
   * and so does the handshake of a client whose certificate does not verify
   * against ca_bundle_path.
   *
   * True only asks each client for a certificate: a client that presents one
   * must pass the verification, and a client that presents none is accepted
   * as well. A handler then tells the two apart with
   * chttpsvr_req_peer_cert_verified() and decides for itself what an
   * anonymous client may do.
   *
   * chttpclient ignores this field, as it ignores every other server-only
   * setting: a server always presents a certificate. */
  bool client_cert_optional;
} chttp_tls_config_t;

/**
 * @brief A chttp_tls_config_t with the default settings.
 *
 * It is the same as: chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
 *
 * It is also the same as a zero-initialised struct, because the default of
 * every field IS zero; it exists as a name that a caller can write to say
 * "the defaults, on purpose".
 */
#define CHTTP_TLS_DEFAULT \
  ((chttp_tls_config_t){NULL, NULL, NULL, false, false, false})

/* ========================================================================== */
/*                         REQUEST BODY                                       */
/* ========================================================================== */

/**
 * @brief This struct describes an HTTP request body. The client API uses it.
 *
 * This struct does NOT own data, and the rules for ownership depend on the
 * context. chttp_request_new_mp() copies data into its own buffer, so the
 * caller can free the source after that call returns.
 */
typedef struct chttp_request_body {
  const void *data;         /* body bytes; NULL = no body */
  size_t len;               /* byte length of data */
  const char *content_type; /* the caller owns this; when it is not NULL,
                             * it sets Content-Type. */
} chttp_request_body_t;

/** @brief Shorthand for a request with no body. */
#define CHTTP_NO_BODY ((chttp_request_body_t){NULL, 0, NULL})

/** @brief Inline request body with explicit content type. */
#define CHTTP_BODY(d, n, ct) ((chttp_request_body_t){(d), (n), (ct)})

/** @brief Inline JSON request body. */
#define CHTTP_JSON_BODY(d, n) CHTTP_BODY((d), (n), "application/json")

/** @brief Inline plain-text request body. */
#define CHTTP_TEXT_BODY(d, n) CHTTP_BODY((d), (n), "text/plain")

/** @brief Inline URL-encoded form request body. */
#define CHTTP_FORM_BODY(d, n) \
  CHTTP_BODY((d), (n), "application/x-www-form-urlencoded")

/* ========================================================================== */
/*                         BASE64 AND BASIC AUTH                              */
/* ========================================================================== */

/**
 * @brief Encode a buffer as base64 (custom allocator).
 *
 * This is the standard RFC 4648 base64, with the '+' and '/' alphabet and
 * '=' padding: the variant that RFC 7617 Basic auth needs and that HTTP uses
 * everywhere.
 *
 * @param mp       A custom allocator, or NULL for malloc/free.
 * @param data     The buffer to encode. It can be NULL only if len == 0.
 * @param len      The number of bytes in data.
 * @param out_len  Optional: it gets the length of the string that this
 *                 function gives back, not counting the terminating NUL.
 *                 It can be NULL.
 * @return A new base64 string that ends with a NUL, or NULL. The function
 *         gives NULL if the allocation fails, if data is NULL and len > 0,
 *         or if len is so large that the encoded size overflows size_t.
 */
char *chttp_base64_encode_mp(ccol_memmgmt_procs_t *mp, const void *data,
                             size_t len, size_t *out_len);

/**
 * @brief Encode a buffer as base64 (default allocator).
 */
static inline __attribute__((always_inline)) char *chttp_base64_encode(
    const void *data, size_t len, size_t *out_len) {
  return chttp_base64_encode_mp(NULL, data, len, out_len);
}

/**
 * @brief Decode a base64 string that ends with a NUL (custom allocator).
 *
 * The function accepts the standard RFC 4648 base64 (the '+' and '/'
 * alphabet) with '=' padding. It takes the input length from
 * strlen(b64_input), and that length must be a multiple of 4 bytes. Any '='
 * padding must be the last character or the last two characters. In a
 * padded final group, the bits that no decoded byte holds must be zero
 * (RFC 4648 SS3.5), so that every byte string has exactly one encoding:
 * "QQ==" decodes to "A", and "QR==" is malformed. The function rejects
 * every malformed input, that is, one with an invalid character, padding in
 * the wrong place, nonzero padding bits, or the wrong length.
 *
 * The buffer that the function gives back ends with a NUL, which helps when
 * you decode text, but the decoded data can also contain NUL bytes inside
 * it. Always use out_len to get the real size, and never strlen().
 *
 * @param mp         A custom allocator, or NULL for malloc/free.
 * @param b64_input  The base64 string to decode. It ends with a NUL. It
 *                   must not be NULL.
 * @param out_len    Optional: it gets the decoded byte length. It can be
 *                   NULL.
 * @return A new buffer with the decoded bytes, or NULL. The function gives
 *         NULL if the allocation fails or if the input is malformed.
 */
void *chttp_base64_decode_mp(ccol_memmgmt_procs_t *mp, const char *b64_input,
                             size_t *out_len);

/**
 * @brief Decode a base64 string that ends with a NUL (default allocator).
 */
static inline __attribute__((always_inline)) void *chttp_base64_decode(
    const char *b64_input, size_t *out_len) {
  return chttp_base64_decode_mp(NULL, b64_input, out_len);
}

/**
 * @brief Build a "Basic <base64(username:password)>" header value (custom
 * allocator).
 *
 * The function makes only the header VALUE (RFC 7617), not the
 * "Authorization: " key part. You can give the result directly to
 * chttp_request_set_header(req, "authorization", ...) or to the equivalent
 * function of chttpsvr.
 *
 * @param mp        A custom allocator, or NULL for malloc/free.
 * @param username  The username. It must not be NULL, it can be empty, and
 *                  it must not contain a colon (':'). RFC 7617 SS2 makes the
 *                  colon the one delimiter of the encoded
 *                  "user-id:password" string, and a recipient cuts that
 *                  string at its FIRST colon, so a user-id with a colon in
 *                  it is not transmittable without ambiguity. The function
 *                  rejects such a user-id instead of cutting it silently
 *                  into a different identity.
 * @param password  The password. It must not be NULL and it can be empty.
 *                  It can contain colons, and they need no escape (the
 *                  password is everything after the first colon).
 * @return A new "Basic <base64>" string, or NULL. The function gives NULL
 *         if the allocation fails, if username or password is NULL, or if
 *         username contains a colon.
 */
char *chttp_basic_auth_mp(ccol_memmgmt_procs_t *mp, const char *username,
                          const char *password);

/**
 * @brief Build a "Basic <base64(username:password)>" header value (default
 * allocator).
 */
static inline __attribute__((always_inline)) char *chttp_basic_auth(
    const char *username, const char *password) {
  return chttp_basic_auth_mp(NULL, username, password);
}

#pragma GCC visibility pop
