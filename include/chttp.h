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
#include <stdbool.h>
#endif
#include <stddef.h>

#include "chashmap.h"

// The types chttpclient and chttpserver share. chttpclient.h and chttpserver.h
// include this header for you; include it yourself only when you use these
// types without either of them.
//
// Everything from here to the matching pop is the public ABI and the shared
// library exports it. The library builds with -fvisibility=hidden, so anything
// outside these blocks stays internal.
#pragma GCC visibility push(default)

// The HTTP methods the library knows.
typedef enum chttp_method {
  CHTTP_GET = 0,
  CHTTP_POST,
  CHTTP_PUT,
  CHTTP_DELETE,
  CHTTP_PATCH,
  CHTTP_HEAD,
  CHTTP_OPTIONS,
  // A wildcard for registering a server handler: It matches all seven methods
  // above. It's never the method of a real request, so chttpsvr_req_method()
  // never gives it back. A client request refuses it (chttp_request_create()
  // gives NULL, chttp_perform() gives ccol_invalid_args), as any value
  // outside the seven. A method chttpserver doesn't know (TRACE, CONNECT, a
  // WebDAV or custom verb) gets 501 before routing, so even a CHTTP_ANY handler
  // never sees it.
  CHTTP_ANY
} chttp_method;

// Gives the uppercase name of method m, which the client puts on the request
// line and both modules use in log lines. CHTTP_ANY gives "ANY" and a value
// outside the enum gives "UNKNOWN".
static inline const char *chttp_method_str(chttp_method m) {
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

// HTTP status codes, named so handlers and callers needn't write bare numbers.
//
// 1xx: Informational
#define CHTTP_STATUS_CONTINUE 100
#define CHTTP_STATUS_SWITCHING_PROTOCOLS 101

// 2xx: Success
#define CHTTP_STATUS_OK 200
#define CHTTP_STATUS_CREATED 201
#define CHTTP_STATUS_ACCEPTED 202
#define CHTTP_STATUS_NO_CONTENT 204
#define CHTTP_STATUS_PARTIAL_CONTENT 206

// 3xx: Redirection
#define CHTTP_STATUS_MOVED_PERMANENTLY 301
#define CHTTP_STATUS_FOUND 302
#define CHTTP_STATUS_NOT_MODIFIED 304
#define CHTTP_STATUS_TEMPORARY_REDIRECT 307
#define CHTTP_STATUS_PERMANENT_REDIRECT 308

// 4xx: Client errors
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

// 5xx: Server errors
#define CHTTP_STATUS_INTERNAL_ERROR 500
#define CHTTP_STATUS_NOT_IMPLEMENTED 501
#define CHTTP_STATUS_BAD_GATEWAY 502
#define CHTTP_STATUS_SERVICE_UNAVAILABLE 503
#define CHTTP_STATUS_GATEWAY_TIMEOUT 504
#define CHTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED 505

// The TLS settings of chttpclient and chttpserver; each one ignores the fields
// that aren't for it. A struct of all zeros is the strict configuration, and
// that's why each relaxation is named for what turning it on gives up. So
// {0}, {.ca_bundle_path = p} and {.cert_path = c, .key_path = k} all verify:
// a client checks the server's chain and hostname, and a server with a
// ca_bundle_path requires a client certificate that verifies against it.
//
// cert_path, key_path and ca_bundle_path must each name a regular file of at
// most 16 MB; a directory, FIFO or device is refused as a load failure before
// anything is read.
//
// A ca_bundle_path holds one or more PEM CA certificates and may hold CRLs
// beside them (a file with CRLs only is refused). With a CRL in the bundle,
// revocation is checked for the whole chain the peer presents, as nginx
// ssl_crl does: Each certificate below the anchor needs a CRL of its issuer,
// and a revoked certificate, a missing CRL or an expired CRL fails the
// handshake. Without a CRL nothing is checked for revocation. The bundle is
// read when the configuration is applied, so a renewed CRL takes effect at the
// server's next chttpsvr_start() or the client's next chttpcli_set_tls().
typedef struct chttp_tls_config {
  // The server certificate and key, or the client's for mutual TLS (NULL for
  // none), and a CA bundle (NULL for the system trust store).
  const char *cert_path;
  const char *key_path;
  const char *ca_bundle_path;
  // Client only. False verifies the server's chain, against ca_bundle_path when
  // it's set and the system store otherwise. True accepts any certificate from
  // any peer, so whoever can intercept the connection can read and change it;
  // it's for tests against throwaway servers, never for production. True also
  // turns off the hostname check: A hostname matched against an unverified
  // certificate proves nothing, so there's no way to ask for the hostname check
  // without the chain check. chttpcli_set_tls() refuses true together with a
  // ca_bundle_path (ccol_invalid_args).
  bool insecure_skip_verify;
  // Client only. False matches the request's hostname or IP literal against the
  // certificate. True keeps the chain check and drops only that match, for
  // example to reach a host by an address its certificate doesn't name. Any
  // peer with any certificate from the trusted CAs is then accepted, so pin the
  // narrowest ca_bundle_path you can. No effect when insecure_skip_verify is
  // true.
  bool insecure_skip_hostname_check;
  // Server only, and only beside ca_bundle_path. False enforces mutual TLS: A
  // client with no certificate, or one that doesn't verify, fails the
  // handshake. True only asks for one: A certificate that's presented must
  // verify, but a client without one is accepted, and a handler tells the cases
  // apart with chttpsvr_req_peer_cert_verified(). chttpclient ignores it.
  bool client_cert_optional;
} chttp_tls_config;

// The default TLS settings: chttp_tls_config tls = CHTTP_TLS_DEFAULT;. It's
// the same as a zeroed struct; the name lets you say you want the defaults on
// purpose.
#define CHTTP_TLS_DEFAULT                                    \
  ((chttp_tls_config){.cert_path = NULL,                     \
                      .key_path = NULL,                      \
                      .ca_bundle_path = NULL,                \
                      .insecure_skip_verify = false,         \
                      .insecure_skip_hostname_check = false, \
                      .client_cert_optional = false})

// A request body for the client API. It doesn't own data;
// chttp_request_create_mp() copies the bytes, so you may free the source once
// that call returns.
typedef struct chttp_request_body {
  // The body bytes (NULL for none) and their length, and the Content-Type to
  // send when content_type isn't NULL (the caller owns the string).
  const void *data;
  size_t len;
  const char *content_type;
} chttp_request_body;

// A request body that holds nothing and sets no Content-Type, for client
// requests that send no body.
#define CHTTP_NO_BODY \
  ((chttp_request_body){.data = NULL, .len = 0, .content_type = NULL})

// Builds a chttp_request_body compound literal from data d, length n and
// content type ct (NULL sends no Content-Type), so a body can be passed inline
// to the client calls. The request copies the bytes, so d may be freed once
// the request is created. Each argument is evaluated once.
#define CHTTP_BODY(d, n, ct) \
  ((chttp_request_body){.data = (d), .len = (n), .content_type = (ct)})

// Builds a request body of n bytes at d with Content-Type application/json,
// for the client calls. The request copies the bytes, so d may be freed once
// the request is created. Each argument is evaluated once.
#define CHTTP_JSON_BODY(d, n) CHTTP_BODY((d), (n), "application/json")

// Builds a request body of n bytes at d with Content-Type text/plain, for the
// client calls. The request copies the bytes, so d may be freed once the
// request is created. Each argument is evaluated once.
#define CHTTP_TEXT_BODY(d, n) CHTTP_BODY((d), (n), "text/plain")

// Builds a request body of n bytes at d with Content-Type
// application/x-www-form-urlencoded (an HTML form), for the client calls. The
// request copies the bytes, so d may be freed once the request is created.
// Each argument is evaluated once.
#define CHTTP_FORM_BODY(d, n) \
  CHTTP_BODY((d), (n), "application/x-www-form-urlencoded")

// Base64 and Basic auth.

// Encodes len bytes of data as RFC 4648 base64 ('+' and '/', '=' padding), the
// variant HTTP and Basic auth use, for callers that build header values. mp
// is your allocator or NULL. data may be NULL only when len is 0. It gives a
// new NUL-terminated string, with its length (without the NUL) in *out_len
// when out_len isn't NULL, or NULL when the allocation fails, data is NULL
// with len > 0, or the encoded size would overflow size_t. The caller frees
// the string with the free function of mp (free() when mp is NULL).
char *chttp_base64_encode_mp(ccol_memmgmt_procs *mp, const void *data,
                             size_t len, size_t *out_len);

// Encodes len bytes of data as RFC 4648 base64 ('+' and '/', '=' padding), the
// variant HTTP and Basic auth use. data may be NULL only when len is 0. Gives a
// new NUL-terminated string that the caller frees with free(), with its length
// (without the NUL) in *out_len when out_len isn't NULL, or NULL when the
// allocation fails, data is NULL with len > 0, or the encoded size would
// overflow size_t.
static inline __attribute__((always_inline)) char *chttp_base64_encode(
    const void *data, size_t len, size_t *out_len) {
  return chttp_base64_encode_mp(NULL, data, len, out_len);
}

// Decodes the NUL-terminated RFC 4648 base64 string b64_input (not NULL), for
// callers that read encoded header values such as Basic credentials. Its
// length must be a multiple of 4 and '=' may only be the last one or two
// characters. The unused bits of a padded final group must be zero (RFC 4648
// section 3.5), so each byte string has one encoding: "QQ==" gives "A" and
// "QR==" is malformed. A bad character, misplaced padding, nonzero padding
// bits or a wrong length is refused. mp is your allocator or NULL. It gives a
// new buffer, or NULL when the input is malformed or the allocation fails. The
// buffer ends with a NUL for convenience, but the data may hold NULs of its
// own, so take the size from *out_len (out_len may be NULL), never from
// strlen(). The caller frees the buffer with the free function of mp (free()
// when mp is NULL).
void *chttp_base64_decode_mp(ccol_memmgmt_procs *mp, const char *b64_input,
                             size_t *out_len);

// Decodes the NUL-terminated RFC 4648 base64 string b64_input (not NULL). Its
// length must be a multiple of 4, '=' may only be the last one or two
// characters, and the unused bits of a padded final group must be zero, so
// "QQ==" gives "A" and "QR==" is refused. Gives a new buffer that the caller
// frees with free(), or NULL when the input is malformed or the allocation
// fails. The buffer ends with a NUL, but the data may hold NULs of its own, so
// take the size from *out_len (out_len may be NULL), never from strlen().
static inline __attribute__((always_inline)) void *chttp_base64_decode(
    const char *b64_input, size_t *out_len) {
  return chttp_base64_decode_mp(NULL, b64_input, out_len);
}

// Builds the RFC 7617 header value "Basic <base64(username:password)>",
// without the "Authorization: " part, so a caller can authenticate a request
// with chttp_request_set_header(req, "authorization", ...) or its chttpsvr
// equivalent. Neither string may be NULL; both may be empty. The username
// must not hold a colon: The recipient cuts at the first colon, so such a
// user-id can't be sent unambiguously and is refused rather than turned into
// another identity. The password may hold colons. A control byte (0x00 to
// 0x1F, or 0x7F) in either is refused, as RFC 7617 section 2 requires; other
// bytes, UTF-8 included, are encoded as they are. mp is your allocator or
// NULL. It gives a new string that the caller frees with the free function of
// mp (free() when mp is NULL), or NULL for a failed allocation or a refused
// input.
char *chttp_basic_auth_mp(ccol_memmgmt_procs *mp, const char *username,
                          const char *password);

// Builds the RFC 7617 header value "Basic <base64(username:password)>",
// without the "Authorization: " part, for chttp_request_set_header(req,
// "authorization", ...). Neither string may be NULL; both may be empty. A colon
// in the username, or a control byte (0x00 to 0x1F, or 0x7F) in either, is
// refused; the password may hold colons and UTF-8 is encoded as it is. Gives a
// new string that the caller frees with free(), or NULL for a failed allocation
// or a refused input.
static inline __attribute__((always_inline)) char *chttp_basic_auth(
    const char *username, const char *password) {
  return chttp_basic_auth_mp(NULL, username, password);
}

#pragma GCC visibility pop
