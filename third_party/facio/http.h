#ifndef H_HTTP_H
/*
Copyright: Boaz Segev, 2016-2019
License: MIT

Feel free to copy, use and enjoy according to the license provided.
*/
#define H_HTTP_H

#include <fio.h>
#include <fiobj.h>
#include <time.h>

/* support C++ */
#ifdef __cplusplus
extern "C" {
#endif

/* *****************************************************************************
Compile Time Settings
***************************************************************************** */

/** When a new connection is accepted, it will be immediately declined with a
 * 503 service unavailable (server busy) response unless the following number of
 * file descriptors is available.*/
#ifndef HTTP_BUSY_UNLESS_HAS_FDS
#define HTTP_BUSY_UNLESS_HAS_FDS 64
#endif

#ifndef HTTP_DEFAULT_BODY_LIMIT
#define HTTP_DEFAULT_BODY_LIMIT (1024 * 1024 * 50)
#endif

#ifndef HTTP_MAX_HEADER_COUNT
#define HTTP_MAX_HEADER_COUNT 128
#endif

#ifndef HTTP_MAX_HEADER_LENGTH
/** the default maximum length for a single header line */
#define HTTP_MAX_HEADER_LENGTH 8192
#endif

#ifndef FIO_HTTP_EXACT_LOGGING
/**
 * By default, facil.io logs the HTTP request cycle using a fuzzy starting point
 * (a close enough timestamp).
 *
 * The fuzzy timestamp includes delays that aren't related to the HTTP request,
 * sometimes including time that was spent waiting on the client. On the other
 * hand, `FIO_HTTP_EXACT_LOGGING` excludes time that the client might have been
 * waiting for facil.io to read data from the network.
 *
 * Due to the preference to err on the side of causion, fuzzy time-stamping is
 * the default.
 */
#define FIO_HTTP_EXACT_LOGGING 0
#endif

/** the `http_listen settings, see details in the struct definition. */
typedef struct http_settings_s http_settings_s;

/* *****************************************************************************
The Request / Response type and functions
***************************************************************************** */

/**
 * A generic HTTP handle used for HTTP request/response data.
 *
 * The `http_s` data can only be accessed safely from within the `on_request`
 * HTTP callback OR an `http_defer` callback.
 */
typedef struct {
  /** the HTTP request's "head" starts with a private data used by facil.io */
  struct {
    /** the function touting table - used by facil.io, don't use directly! */
    void *vtbl;
    /** the connection's owner / uuid - used by facil.io, don't use directly! */
    uintptr_t flag;
    /** The response headers, if they weren't sent. Don't access directly. */
    FIOBJ out_headers;
  } private_data;
  /** a time merker indicating when the request was received. */
  struct timespec received_at;
  /** a String containing the method data (supports non-standard methods. */
  FIOBJ method;
  /** The status string, for response objects (client mode response). */
  FIOBJ status_str;
  /** The HTTP version string, if any. */
  FIOBJ version;
  /** The status used for the response (or if the object is a response).
   *
   * When sending a request, the status should be set to 0.
   */
  uintptr_t status;
  /** The request path, if any. */
  FIOBJ path;
  /** The request query, if any. */
  FIOBJ query;
  /** a hash of general header data. When a header is set multiple times (such
   * as cookie headers), an Array will be used instead of a String. */
  FIOBJ headers;
  /**
   * a placeholder for a hash of cookie data.
   * the hash will be initialized when parsing the request.
   */
  FIOBJ cookies;
  /**
   * a placeholder for a hash of request data.
   * the hash will be initialized when parsing the request.
   */
  FIOBJ params;
  /**
   * a reader for body data (might be a temporary file or a string or NULL).
   * see fiobj_data.h for details.
   */
  FIOBJ body;
  /** an opaque user data pointer, to be used BEFORE calling `http_defer`. */
  void *udata;
} http_s;

/**
 * Sets a response header, taking ownership of the value object, but NOT the
 * name object (so name objects could be reused in future responses).
 *
 * Returns -1 on error and 0 on success.
 */
int http_set_header(http_s *h, FIOBJ name, FIOBJ value);

/**
 * Sets a response header.
 *
 * Returns -1 on error and 0 on success.
 */
int http_set_header2(http_s *h, fio_str_info_s name, fio_str_info_s value);

/**
 * Sends the response headers and body.
 *
 * **Note**: The body is *copied* to the HTTP stream and it's memory should be
 * freed by the calling function.
 *
 * Returns -1 on error and 0 on success.
 *
 * AFTER THIS FUNCTION IS CALLED, THE `http_s` OBJECT IS NO LONGER VALID.
 */
int http_send_body(http_s *h, void *data, uintptr_t length);

/**
 * Sends an HTTP error response.
 *
 * Returns -1 on error and 0 on success.
 *
 * AFTER THIS FUNCTION IS CALLED, THE `http_s` OBJECT IS NO LONGER VALID.
 *
 * The `uuid` and `settings` arguments are only required if the `http_s` handle
 * is NULL.
 */
int http_send_error(http_s *h, size_t error_code);

/**
 * Sends the response headers for a header only response.
 *
 * AFTER THIS FUNCTION IS CALLED, THE `http_s` OBJECT IS NO LONGER VALID.
 */
void http_finish(http_s *h);

/* *****************************************************************************
HTTP evented API (pause / resume HTTp handling)
***************************************************************************** */

typedef struct http_pause_handle_s http_pause_handle_s;
/**
 * Pauses the request / response handling and INVALIDATES the current `http_s`
 * handle (no `http` functions can be called).
 *
 * The `http_resume` function MUST be called (at some point) using the opaque
 * `http` pointer given to the callback `task`.
 *
 * The opaque `http` pointer is only valid for a single call to `http_resume`
 * and can't be used by any other `http` function (it's a different data type).
 *
 * Note: the current `http_s` handle will become invalid once this function is
 *    called and it's data might be deallocated, invalid or used by a different
 *    thread.
 */
void http_pause(http_s *h, void (*task)(http_pause_handle_s *http));

/**
 * Resumes a request / response handling within a task and INVALIDATES the
 * current `http_s` handle.
 *
 * The `task` MUST call one of the `http_send_*`, `http_finish`, or
 * `http_pause`functions.
 *
 * The (optional) `fallback` will receive the opaque `udata` that was stored in
 * the HTTP handle and can be used for cleanup.
 *
 * Note: `http_resume` can only be called after calling `http_pause` and
 * entering it's task.
 *
 * Note: the current `http_s` handle will become invalid once this function is
 *    called and it's data might be deallocated, invalidated or used by a
 *    different thread.
 */
void http_resume(http_pause_handle_s *http, void (*task)(http_s *h),
                 void (*fallback)(void *udata));

/** Returns the `udata` associated with the paused opaque handle */
void *http_paused_udata_get(http_pause_handle_s *http);

/* *****************************************************************************
HTTP Connections - Listening / Connecting / Hijacking
***************************************************************************** */

/** The HTTP settings. */
struct http_settings_s {
  /** Callback for normal HTTP requests. */
  void (*on_request)(http_s *request);
  /**
   * (optional) Called once, right after headers are parsed and before any
   * body byte is read, for HTTP/1.1 requests. Return non-zero to take over
   * the request -- in that case `on_request` will NOT be called for it, and
   * the caller becomes responsible for reading the body itself via
   * `http1_stream_read` and for eventually calling `http_resume`. Return 0
   * to let normal parsing and `on_request` dispatch proceed unchanged (the
   * default behavior when this callback is left NULL).
   *
   * IMPORTANT: an implementation that intends to return non-zero MUST call
   * `http1_stream_prepare(request)` (declared in http1.h) BEFORE calling
   * `http_pause`, not after. `http_pause` defers the actual handoff via
   * `fio_defer`, and a worker thread may start running `http1_stream_read`
   * on another core as soon as that deferred task is queued -- possibly
   * before this callback even returns. Any state a worker depends on must
   * therefore already be established before `http_pause` runs.
   */
  int (*on_headers_complete)(http_s *request);
  /**
   * Callback for Upgrade and EventSource (SSE) requests.
   *
   * SSE/EventSource requests set the `requested_protocol` string to `"sse"`.
   */
  void (*on_upgrade)(http_s *request, char *requested_protocol, size_t len);
  /** CLIENT REQUIRED: a callback for the HTTP response. */
  void (*on_response)(http_s *response);
  /** (optional) the callback to be performed when the HTTP service closes. */
  void (*on_finish)(struct http_settings_s *settings);
  /** Opaque user data. Facil.io will ignore this field, but you can use it. */
  void *udata;
  /**
   * A public folder for file transfers - allows to circumvent any application
   * layer logic and simply serve static files.
   *
   * Supports automatic `gz` pre-compressed alternatives.
   */
  const char *public_folder;
  /**
   * The length of the public_folder string.
   */
  size_t public_folder_length;
  /**
   * The maximum number of bytes allowed for the request string (method, path,
   * query), header names and fields.
   *
   * Defaults to 32Kib (which is about 4 times more than I would recommend).
   *
   * This reflects the total overall size. On HTTP/1.1, each header line (name +
   * value pair) is also limitied to a hardcoded HTTP_MAX_HEADER_LENGTH bytes.
   */
  size_t max_header_size;
  /**
   * The maximum size of an HTTP request's body (posting / downloading).
   *
   * Defaults to ~ 50Mb.
   */
  size_t max_body_size;
  /**
   * The maximum number of clients that are allowed to connect concurrently.
   *
   * This value's default setting is usually for the best.
   *
   * The default value is computed according to the server's capacity, leaving
   * some breathing room for other network and disk operations.
   *
   * Note: clients, by the nature of socket programming, are counted according
   *       to their internal file descriptor (`fd`) value. Open files and other
   *       sockets count towards a server's limit.
   */
  intptr_t max_clients;
  /** SSL/TLS support. */
  void *tls;
  /**
   * Internal use only -- do not set. Reference count gating when this
   * settings object is actually freed: initialized to 1 (representing the
   * listener's own baseline hold) by `http_listen`, incremented once per
   * accepted connection (`http1_new`) and decremented once that connection
   * is fully torn down (`http1_destroy`), so that a connection still being
   * read by a worker thread (per the worker-driven body ingestion design;
   * see `on_headers_complete`) always keeps this settings object alive.
   * The baseline hold is released via `http_on_finish` for non-TLS
   * listeners, or via `_http_settings_on_tls_cleanup` for TLS listeners
   * (which additionally covers the window before a deferred ALPN dispatch
   * has even called `http1_new` yet). Whichever release brings this to 0
   * actually frees the settings object.
   */
  intptr_t reserved1;
  /** reserved for future use. */
  intptr_t reserved2;
  /** reserved for future use. */
  intptr_t reserved3;
  /**
   * The maximum websocket message size/buffer (in bytes) for Websocket
   * connections. Defaults to ~250KB.
   */
  size_t ws_max_msg_size;
  /**
   * An HTTP/1.x connection timeout.
   *
   * `http_listen` defaults to ~40s and `http_connect` defaults to ~30s.
   *
   * Note: the connection might be closed (by other side) before timeout occurs.
   */
  uint8_t timeout;
  /**
   * Timeout for the websocket connections, a ping will be sent whenever the
   * timeout is reached. Defaults to 40 seconds.
   *
   * Connections are only closed when a ping cannot be sent (the network layer
   * fails). Pongs are ignored.
   */
  uint8_t ws_timeout;
  /** Logging flag - set to TRUE to log HTTP requests. */
  uint8_t log;
  /** a read only flag set automatically to indicate the protocol's mode. */
  uint8_t is_client;
};

/**
 * Listens to HTTP connections at the specified `port`.
 *
 * Leave as NULL to ignore IP binding.
 *
 * Returns -1 on error and the socket's uuid on success.
 *
 * the `on_finish` callback is always called.
 */
intptr_t http_listen(const char *port, const char *binding,
                     struct http_settings_s);
/** Listens to HTTP connections at the specified `port` and `binding`. */
#define http_listen(port, binding, ...) \
  http_listen((port), (binding), (struct http_settings_s){__VA_ARGS__})

/** Returns the HTTP settings associated with a request/response handle. */
struct http_settings_s *http_settings(http_s *h);

/* *****************************************************************************
HTTP Status Strings and Mime-Type helpers
***************************************************************************** */

/** Returns a human readable string related to the HTTP status number. */
fio_str_info_s http_status2str(uintptr_t status);

/** Registers a Mime-Type to be associated with the file extension. */
void http_mimetype_register(char *file_ext, size_t file_ext_len,
                            FIOBJ mime_type_str);

/**
 * Finds the mime-type associated with the file extension, returning a String on
 * success and FIOBJ_INVALID on failure.
 *
 * Remember to call `fiobj_free`.
 */
FIOBJ http_mimetype_find(char *file_ext, size_t file_ext_len);

/** Clears the Mime-Type registry (it will be empty after this call). */
void http_mimetype_clear(void);

/* *****************************************************************************
Commonly used headers (fiobj Symbol objects)
***************************************************************************** */

extern FIOBJ HTTP_HEADER_ACCEPT;
extern FIOBJ HTTP_HEADER_CACHE_CONTROL;
extern FIOBJ HTTP_HEADER_CONNECTION;
extern FIOBJ HTTP_HEADER_CONTENT_ENCODING;
extern FIOBJ HTTP_HEADER_CONTENT_LENGTH;
extern FIOBJ HTTP_HEADER_CONTENT_RANGE;
extern FIOBJ HTTP_HEADER_CONTENT_TYPE;
extern FIOBJ HTTP_HEADER_COOKIE;
extern FIOBJ HTTP_HEADER_DATE;
extern FIOBJ HTTP_HEADER_ETAG;
extern FIOBJ HTTP_HEADER_HOST;
extern FIOBJ HTTP_HEADER_LAST_MODIFIED;
extern FIOBJ HTTP_HEADER_ORIGIN;
extern FIOBJ HTTP_HEADER_SET_COOKIE;

/* *****************************************************************************
HTTP General Helper functions that could be used globally
***************************************************************************** */

/**
 * Writes a log line to `stderr` about the request / response object.
 *
 * This function is called automatically if the `.log` setting is enabled.
 */
void http_write_log(http_s *h);
/* *****************************************************************************
HTTP Time related helper functions that could be used globally
***************************************************************************** */

/**
A faster (yet less localized) alternative to `gmtime_r`.

See the libc `gmtime_r` documentation for details.

Falls back to `gmtime_r` for dates before epoch.
*/
struct tm *http_gmtime(time_t timer, struct tm *tmbuf);

/** Writes an RFC 7231 date representation (HTTP date format) to target. */
size_t http_date2rfc7231(char *target, struct tm *tmbuf);
/**
Writes an HTTP date string to the `target` buffer.

This requires ~32 bytes of space to be available at the target buffer (unless
it's a super funky year, 32 bytes is about 3 more than you need).

Returns the number of bytes actually written.
*/
static inline size_t http_date2str(char *target, struct tm *tmbuf) {
  return http_date2rfc7231(target, tmbuf);
}

/**
 * Prints Unix time to a HTTP time formatted string.
 *
 * This variation implements cached results for faster processing, at the
 * price of a less accurate string.
 */
size_t http_time2str(char *target, const time_t t);

/* *****************************************************************************
HTTP URL decoding helper functions that might be used globally
***************************************************************************** */

/** Decodes a URL encoded string, no buffer overflow protection. */
ssize_t http_decode_url_unsafe(char *dest, const char *url_data);

/** Decodes the "path" part of a request, no buffer overflow protection. */
ssize_t http_decode_path_unsafe(char *dest, const char *url_data);

/* support C++ */
#ifdef __cplusplus
}
#endif

#endif /* H_HTTP_H */
