/*
Copyright: Boaz Segev, 2017-2019
License: MIT
*/
#ifndef H_HTTP1_H
#define H_HTTP1_H

#include <http.h>

#ifndef HTTP1_READ_BUFFER
/**
 * The size of a single `read` command, it sets the limit for an HTTP/1.1
 * header line.
 */
#define HTTP1_READ_BUFFER (8 * 1024) /* ~8kb */
#endif

/** Creates an HTTP1 protocol object and handles any unread data in the buffer
 * (if any). */
fio_protocol_s *http1_new(uintptr_t uuid, http_settings_s *settings,
                          void *unread_data, size_t unread_length);

/** Manually destroys the HTTP1 protocol object. */
void http1_destroy(fio_protocol_s *);

/** returns the HTTP/1.1 protocol's VTable. */
void *http1_vtable(void);

/* *****************************************************************************
Worker-driven body ingestion

These let a caller (chttpserver's ctpool workers) read a request's body
directly off the socket, batch by batch, from a thread other than the
reactor thread -- used only after `http_settings_s.on_headers_complete` has
paused the request and taken ownership of it. Content-Length/chunked framing
is decoded by reusing the same parser state machine the reactor uses for the
non-diverted path; no data is buffered ahead of what the caller asks for.
***************************************************************************** */

typedef enum {
  HTTP1_STREAM_ERR_NONE = 0,
  HTTP1_STREAM_ERR_CLOSED,    /* the connection was closed/reset */
  HTTP1_STREAM_ERR_TIMEOUT,   /* no data arrived within the given timeout */
  HTTP1_STREAM_ERR_TOO_LARGE, /* max_body_size was exceeded */
  HTTP1_STREAM_ERR_PROTOCOL,  /* malformed Content-Length/chunked framing */
} http1_stream_err_t;

/**
 * Reads and decodes up to `buflen` bytes of the request body into `buf`,
 * blocking (waiting on the socket, not the calling thread's CPU) for up to
 * `timeout_ms` milliseconds if no data is immediately available. A
 * `timeout_ms` of 0 waits indefinitely.
 *
 * Returns >0 for the number of bytes written to `buf`, 0 once the body has
 * been fully consumed, or -1 on error/timeout/abort (see
 * `http1_stream_last_error`).
 *
 * May only be called after `on_headers_complete` has paused `h` and handed
 * it to a worker; never call this from the reactor thread.
 */
ssize_t http1_stream_read(http_s *h, void *buf, size_t buflen,
                          unsigned timeout_ms);

/** Returns the reason for the last `http1_stream_read` call's -1 return. */
http1_stream_err_t http1_stream_last_error(http_s *h);

/**
 * Marks `h` as diverted to worker-driven body ingestion. MUST be called
 * synchronously, on the reactor thread, from inside an `on_headers_complete`
 * callback that intends to return non-zero -- and it MUST be called before
 * that callback calls `http_pause`. `http_pause` defers the actual handoff
 * via `fio_defer`, which can be picked up and run by a worker thread on
 * another core before the callback that called it even returns; any state
 * this function establishes must therefore already be in place before
 * `http_pause` runs, not after `on_headers_complete` returns.
 */
void http1_stream_prepare(http_s *h);

/**
 * Must be called exactly once per diverted request, after the caller is done
 * calling `http1_stream_read` for it (whether because it reached end of body,
 * hit an error, or simply chose to stop reading early) and before calling
 * `http_resume`. Releases the connection back to normal teardown rules --
 * if the connection died while ingestion was in progress, this is where the
 * deferred cleanup actually happens.
 */
void http1_stream_release(http_s *h);

#endif
