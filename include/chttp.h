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
#include <stdbool.h> /* bool, true and false are keywords in C23 */
#endif
#include <stddef.h>

#include "chashmap.h"

/* Everything this header declares from here to the end belongs to the public
 * Application Binary Interface (ABI) of libccollections, and the shared
 * library exports all of it. The library is built with -fvisibility=hidden,
 * so a function or an object outside these blocks stays internal: its name
 * isn't in the library's dynamic symbol table, an application linking
 * against the library can't interpose it, and a symbol of the same name in
 * that application can't collide with it. */
#pragma GCC visibility push(default)

/**
 * @file chttp.h
 * @brief The common types that chttpclient and chttpserver share.
 *
 * You only need to include this header directly when your code uses the
 * shared types without either of the two sub-modules. chttpclient.h and
 * chttpserver.h both include it for you.
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
   * A wildcard for the server side. As the method argument of
   * chttpsvr_register_handler, chttpsvr_register_streaming_handler,
   * chttpsvr_router_on or chttpsvr_router_on_stream it matches any of the
   * seven concrete methods above. It isn't a valid method for a client
   * request: chttp_request_create and chttp_request_create_mp refuse it
   * (they return NULL), and chttp_run_query returns ccol_invalid_args for it,
   * as for any value outside the seven constants above. It's only a
   * placeholder for registering a handler and never the method of a real
   * incoming request, so chttpsvr_req_method never gives back CHTTP_ANY or
   * any value outside the seven concrete constants above. A request whose
   * method chttpserver doesn't recognize at all (a WebDAV verb, TRACE,
   * CONNECT, a custom verb and so on) never reaches a handler, even one
   * registered with CHTTP_ANY, because chttpserver rejects it with 501 Not
   * Implemented before the route match runs.
   */
  CHTTP_ANY
} chttp_method_t;

/**
 * @brief Give the canonical uppercase method string for a chttp_method_t.
 *
 * CHTTP_ANY gives "ANY", although CHTTP_ANY is a sentinel for server-side
 * routes and not a real HTTP method.
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
 * Each sub-module ignores the fields that don't apply to it.
 * Client-side fields: insecure_skip_verify, insecure_skip_hostname_check,
 *                     ca_bundle_path, cert_path/key_path (mutual TLS).
 * Server-side fields: cert_path, key_path, ca_bundle_path,
 *                     client_cert_optional.
 *
 * A struct whose bytes are all zero is the SAFE configuration. We did that
 * on purpose, and it's the reason each of the three verification fields is
 * named for what turning it ON gives up rather than for what it asks for:
 * every idiomatic way of building a struct in C leaves the fields the caller
 * didn't name as zero:
 *
 *     chttp_tls_config_t a = {0};
 *     chttp_tls_config_t b = {.ca_bundle_path = "/etc/ssl/my-ca.pem"};
 *     chttp_tls_config_t c = {.cert_path = crt, .key_path = key};
 *
 * On a client all three verify the server's certificate chain AND match its
 * hostname. On a server, a ca_bundle_path requires every client to present a
 * certificate that verifies against it. So you can never end up with no
 * verification by writing less than you meant to. Weakening a check takes an
 * explicit assignment, and the field's name says what it costs right where
 * it is read.
 *
 * cert_path, key_path and ca_bundle_path must each name a regular file of at
 * most 16 MB. A path naming a directory, a FIFO or a device is refused at
 * once as a load failure, before a byte is read.
 *
 * A ca_bundle_path holds one or more CA certificates in PEM form, and it may
 * hold certificate revocation lists (CRLs) beside them; a file with only
 * CRLs names no CA and is refused. When the bundle holds a CRL, revocation
 * is checked for the whole chain the peer presents, like the ssl_crl
 * directive of nginx does: every certificate of that chain below its trust
 * anchor needs a CRL of its issuer in the bundle. The handshake then fails
 * for a revoked certificate, for a certificate whose issuer has no CRL in the
 * bundle and for a CRL past its next update time, because we don't accept a
 * chain that can't be checked. A bundle without a CRL checks no revocation.
 * The bundle is read when the configuration is applied, so a renewed CRL
 * takes effect at a server's next chttpsvr_start(), or a client's next
 * chttpclient_set_tls().
 */
typedef struct chttp_tls_config {
  const char *cert_path; /* server cert / client cert for mTLS (NULL = none) */
  const char *key_path;  /* server key  / client key  for mTLS (NULL = none) */
  const char *ca_bundle_path; /* custom CA bundle path; NULL = system default */
  /* Client only. False, the value of a zero-initialised struct, verifies the
   * server's certificate chain, against ca_bundle_path when one is set and
   * against the system trust store otherwise.
   *
   * True accepts ANY certificate from ANY peer, so an attacker who can
   * intercept the connection can read and rewrite every byte of it. Only set
   * it where that is really acceptable, like a test against a throwaway
   * self-signed server; it is never right in production.
   *
   * True also makes insecure_skip_hostname_check irrelevant. A hostname
   * matched against a certificate that nothing validated proves nothing,
   * since an attacker can forge the whole certificate. On purpose there's no
   * way to ask for the hostname check WITHOUT the chain check, because that
   * combination looks like a partial measure and gives no protection at all.
   *
   * Setting this true together with ca_bundle_path contradicts itself, since
   * the bundle is there to be verified against. chttpclient_set_tls()
   * refuses that combination with ccol_invalid_args instead of quietly
   * picking one of the two. */
  bool insecure_skip_verify;
  /* Client only. False, the value of a zero-initialised struct, matches the
   * hostname or the IP literal of the request against the subject of the
   * certificate the peer presented.
   *
   * True keeps the full chain verification of the field above and drops only
   * that match. It's the narrower of the two relaxations and it has real
   * uses, such as reaching a host by an address or an internal name that its
   * certificate doesn't carry, while still requiring the certificate to
   * chain to a CA you pinned. The connection is then open to any peer that
   * holds ANY certificate from that CA, so it's only as strong as the
   * narrowest trust store you can give ca_bundle_path.
   *
   * The field has no effect when insecure_skip_verify is true, because then
   * nothing is verified at all. */
  bool insecure_skip_hostname_check;
  /* Server only, and it only matters beside ca_bundle_path. False, the value
   * of a zero-initialised struct, is mutual TLS that the server enforces:
   * the handshake fails for a client that presents no certificate and for a
   * client whose certificate doesn't verify against ca_bundle_path.
   *
   * True only asks each client for a certificate. A client that presents one
   * must pass the verification, and a client that presents none is accepted
   * too. A handler tells the two apart with chttpsvr_req_peer_cert_verified()
   * and decides for itself what an anonymous client is allowed to do.
   *
   * chttpclient ignores this field like every other server-only setting,
   * since a server always presents a certificate. */
  bool client_cert_optional;
} chttp_tls_config_t;

/**
 * @brief A chttp_tls_config_t with the default settings.
 *
 * Use it as: chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
 *
 * Since every field defaults to zero, it's the same as a zero-initialised
 * struct. The name is there so a caller can write "the defaults, on
 * purpose".
 */
#define CHTTP_TLS_DEFAULT                                      \
  ((chttp_tls_config_t){.cert_path = NULL,                     \
                        .key_path = NULL,                      \
                        .ca_bundle_path = NULL,                \
                        .insecure_skip_verify = false,         \
                        .insecure_skip_hostname_check = false, \
                        .client_cert_optional = false})

/* ========================================================================== */
/*                         REQUEST BODY                                       */
/* ========================================================================== */

/**
 * @brief Describes an HTTP request body. The client API uses it.
 *
 * The struct does NOT own data, and who owns it depends on the context.
 * chttp_request_create_mp() copies data into its own buffer, so the caller
 * can free the source once that call has returned.
 */
typedef struct chttp_request_body {
  const void *data;         /* body bytes; NULL = no body */
  size_t len;               /* byte length of data */
  const char *content_type; /* owned by the caller; sets Content-Type
                             * when it is not NULL. */
} chttp_request_body_t;

/** @brief Shorthand for a request with no body. */
#define CHTTP_NO_BODY \
  ((chttp_request_body_t){.data = NULL, .len = 0, .content_type = NULL})

/** @brief Inline request body with explicit content type. */
#define CHTTP_BODY(d, n, ct) \
  ((chttp_request_body_t){.data = (d), .len = (n), .content_type = (ct)})

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
 * Standard RFC 4648 base64 with the '+' and '/' alphabet and '=' padding,
 * which is the variant RFC 7617 Basic auth needs and the one HTTP uses
 * everywhere.
 *
 * @param mp       A custom allocator, or NULL for malloc/free.
 * @param data     The buffer to encode. It can be NULL only if len == 0.
 * @param len      The number of bytes in data.
 * @param out_len  Optional: receives the length of the returned string, not
 *                 counting the terminating NUL. It can be NULL.
 * @return A new NUL-terminated base64 string, or NULL. NULL comes back when
 *         the allocation fails, when data is NULL and len > 0, or when len
 *         is so large that the encoded size would overflow size_t.
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
 * Accepts standard RFC 4648 base64 (the '+' and '/' alphabet) with '='
 * padding. The input length is strlen(b64_input) and it must be a multiple
 * of 4 bytes. Any '=' padding must be the last character or the last two
 * characters. In a padded final group the bits that no decoded byte holds
 * must be zero (RFC 4648 SS3.5), so every byte string has exactly one
 * encoding: "QQ==" decodes to "A" and "QR==" is malformed. Every malformed
 * input is rejected, meaning one with an invalid character, padding in the
 * wrong place, nonzero padding bits or the wrong length.
 *
 * The returned buffer ends with a NUL, which is handy when you decode text,
 * but the decoded data may contain NUL bytes of its own. Always take the
 * real size from out_len, never from strlen().
 *
 * @param mp         A custom allocator, or NULL for malloc/free.
 * @param b64_input  The NUL-terminated base64 string to decode. It must not
 *                   be NULL.
 * @param out_len    Optional: receives the decoded byte length. It can be
 *                   NULL.
 * @return A new buffer with the decoded bytes, or NULL when the allocation
 *         fails or the input is malformed.
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
 * Only the header VALUE (RFC 7617) is built, not the "Authorization: " key
 * part. You can pass the result straight to
 * chttp_request_set_header(req, "authorization", ...) or to the equivalent
 * function of chttpsvr.
 *
 * @param mp        A custom allocator, or NULL for malloc/free.
 * @param username  The username. It must not be NULL, it can be empty, and
 *                  it must not contain a colon (':'). RFC 7617 SS2 makes the
 *                  colon the one delimiter of the encoded "user-id:password"
 *                  string, and a recipient cuts that string at its FIRST
 *                  colon, so a user-id with a colon in it can't be sent
 *                  without ambiguity. Such a user-id is rejected instead of
 *                  being cut silently into a different identity.
 * @param password  The password. It must not be NULL and it can be empty.
 *                  It may contain colons, and they need no escaping (the
 *                  password is everything after the first colon).
 *
 * RFC 7617 SS2 also forbids a control character (a byte from 0x00 to 0x1F,
 * or 0x7F) in the user-id and in the password, so either one is refused
 * when it holds such a byte. Every other byte, UTF-8 included, is encoded as
 * it is.
 *
 * @return A new "Basic <base64>" string, or NULL. NULL comes back when the
 *         allocation fails, when username or password is NULL, when
 *         username contains a colon, or when username or password contains
 *         a control character.
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
