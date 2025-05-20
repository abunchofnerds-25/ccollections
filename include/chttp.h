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
   * Server-side wildcard: matches any incoming HTTP method when used as the
   * method argument to chttpsvr_register_handler,
   * chttpsvr_register_streaming_handler, chttpsvr_router_on, or
   * chttpsvr_router_on_stream.  Not a valid method for client requests; passing
   * it to chttp_request_new / chttp_run_query produces undefined behaviour.
   * chttpsvr_req_method never returns CHTTP_ANY -- it always returns the actual
   * method of the incoming request.
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
  bool verify_host; /* client: verify server hostname   (default: true) */
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
