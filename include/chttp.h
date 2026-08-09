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

#include <chashmap.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * @file chttp.h
 * @brief Common types shared between chttpclient and chttpserver.
 *
 * Include this header directly only when code needs the shared types without
 * pulling in either sub-module.  Both chttpclient.h and chttpserver.h include
 * it automatically.
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
   * Server-side wildcard: matches any of the seven concrete methods above
   * when used as the method argument to chttpsvr_register_handler,
   * chttpsvr_register_streaming_handler, chttpsvr_router_on, or
   * chttpsvr_router_on_stream.  Not a valid method for client requests; passing
   * it to chttp_request_new / chttp_run_query produces undefined behaviour.
   * A registration-time placeholder only, never a real incoming request's
   * method: chttpsvr_req_method never returns CHTTP_ANY, and never returns
   * anything outside the seven concrete constants above either.  A request
   * whose method chttpserver does not recognize at all (a WebDAV verb, TRACE,
   * CONNECT, a custom verb, ...) never reaches any handler, including one
   * registered with CHTTP_ANY; it is rejected with 501 Not Implemented before
   * routing ever runs.
   */
  CHTTP_ANY
} chttp_method_t;

/**
 * @brief Return the canonical uppercase method string for a chttp_method_t.
 *
 * Returns "ANY" for CHTTP_ANY (a server-side routing sentinel, not a real HTTP
 * method).
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

/* ========================================================================== */
/*                         TLS CONFIGURATION                                  */
/* ========================================================================== */

/**
 * @brief TLS configuration used by both chttpclient and chttpserver.
 *
 * Fields that are not applicable to a given sub-module are silently ignored.
 * Client-side fields: verify_peer, verify_host, ca_bundle_path,
 *                     cert_path/key_path (mutual TLS, deferred).
 * Server-side fields: cert_path, key_path, ca_bundle_path.
 */
typedef struct chttp_tls_config {
  const char *cert_path; /* server cert / client cert for mTLS (NULL = none) */
  const char *key_path;  /* server key  / client key  for mTLS (NULL = none) */
  const char *ca_bundle_path; /* custom CA bundle path; NULL = system default */
  bool verify_peer; /* client: verify server certificate (default: true) */
  /* client: verify server hostname against the certificate (default: true).
   * NOTE: verify_host always implies verify_peer in practice; hostname
   * matching against a certificate whose chain was never validated gives no
   * real security guarantee, since the certificate itself could be entirely
   * forged. Setting verify_peer=false, verify_host=true does NOT get you
   * "hostname-only checking with no chain trust"; it gets full verification
   * (using the system CA store, or ca_bundle_path if set), same as
   * verify_peer=true would. To genuinely disable all server certificate
   * checking, set both verify_peer=false AND verify_host=false. */
  bool verify_host;
} chttp_tls_config_t;

/**
 * @brief Initialise a chttp_tls_config_t with verification-on defaults.
 *
 * Equivalent to: chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
 */
#define CHTTP_TLS_DEFAULT ((chttp_tls_config_t){NULL, NULL, NULL, true, true})

/* ========================================================================== */
/*                         REQUEST BODY                                       */
/* ========================================================================== */

/**
 * @brief Describes an HTTP request body (used by the client API).
 *
 * data is NOT owned by this struct; ownership semantics depend on context.
 * chttp_request_new_mp() copies data into its own buffer, so the caller may
 * free the source after that call returns.
 */
typedef struct chttp_request_body {
  const void *data;         /* body bytes; NULL = no body */
  size_t len;               /* byte length of data */
  const char *content_type; /* if non-NULL, sets Content-Type; caller-owned */
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
 * @brief Base64-encode a buffer (custom allocator).
 *
 * Standard RFC 4648 base64 (the '+'/'/' alphabet, '=' padding); the
 * variant required by RFC 7617 Basic auth and used throughout HTTP.
 *
 * @param mp       Custom allocator, or NULL for malloc/free.
 * @param data     Buffer to encode. May be NULL only if len == 0.
 * @param len      Number of bytes in data.
 * @param out_len  Optional: receives the length of the returned string
 *                 (excluding the terminating NUL). May be NULL.
 * @return Newly allocated, NUL-terminated base64 string, or NULL on
 *         allocation failure, if data is NULL and len > 0, or if len is
 *         large enough that the encoded size would overflow size_t.
 */
char *chttp_base64_encode_mp(ccol_memmgmt_procs_t *mp, const void *data,
                             size_t len, size_t *out_len);

/**
 * @brief Base64-encode a buffer (default allocator).
 */
static inline __attribute__((always_inline)) char *chttp_base64_encode(
    const void *data, size_t len, size_t *out_len) {
  return chttp_base64_encode_mp(NULL, data, len, out_len);
}

/**
 * @brief Base64-decode a NUL-terminated base64 string (custom allocator).
 *
 * Accepts standard RFC 4648 base64 (the '+'/'/' alphabet) with '=' padding.
 * The input length is taken from strlen(b64_input); it must be a multiple
 * of 4 bytes, and any '=' padding must appear only as the final one or two
 * characters. Any other malformed input (invalid character, misplaced
 * padding, wrong length) is rejected.
 *
 * The returned buffer is NUL-terminated as a convenience for decoding text
 * payloads, but the decoded data may legitimately contain embedded NUL
 * bytes; always use out_len, never strlen(), to determine its real size.
 *
 * @param mp         Custom allocator, or NULL for malloc/free.
 * @param b64_input  NUL-terminated base64 string to decode. Must not be
 *                   NULL.
 * @param out_len    Optional: receives the decoded byte length. May be
 *                   NULL.
 * @return Newly allocated decoded buffer, or NULL on allocation failure or
 *         malformed input.
 */
void *chttp_base64_decode_mp(ccol_memmgmt_procs_t *mp, const char *b64_input,
                             size_t *out_len);

/**
 * @brief Base64-decode a NUL-terminated base64 string (default allocator).
 */
static inline __attribute__((always_inline)) void *chttp_base64_decode(
    const char *b64_input, size_t *out_len) {
  return chttp_base64_decode_mp(NULL, b64_input, out_len);
}

/**
 * @brief Build a "Basic <base64(username:password)>" header value (custom
 * allocator).
 *
 * Produces only the header VALUE (RFC 7617) (not the "Authorization: "
 * key part) ready to be passed to chttp_request_set_header(req,
 * "authorization", ...) or chttpsvr equivalent.
 *
 * @param mp        Custom allocator, or NULL for malloc/free.
 * @param username  Username. Must not be NULL; may be empty.
 * @param password  Password. Must not be NULL; may be empty.
 * @return Newly allocated "Basic <base64>" string, or NULL on allocation
 *         failure or if username/password is NULL.
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
