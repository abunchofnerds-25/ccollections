/*
Copyright: Boaz Segev, 2017-2019
License: MIT
*/
#include <assert.h>
#include <errno.h>
#include <fio.h>
#include <fiobj.h>
#include <http1.h>
#include <http1_parser.h>
#include <http_internal.h>
#include <poll.h>
#include <stddef.h>

/* *****************************************************************************
The HTTP/1.1 Protocol Object
***************************************************************************** */

/* Size of the on-stack scratch buffer used by http1_stream_read for each raw
 * fio_read call. Must stay well under HTTP_MAX_HEADER_LENGTH, since any
 * unconsumed tail -- whether pipelined bytes belonging to the next request,
 * or an incomplete framing token to be retried with more data -- gets
 * stashed into the connection's own `buf[]`, which is HTTP_MAX_HEADER_LENGTH
 * bytes. */
#define HTTP1_STREAM_SCRATCH_SIZE 4096

typedef struct http1pr_s {
  http_fio_protocol_s p;
  http1_parser_s parser;
  http_s request;
  uintptr_t buf_len;
  uintptr_t max_header_size;
  uintptr_t header_size;
  uint8_t close;
  uint8_t is_client;
  uint8_t stop;
  /* --- worker-driven body ingestion (added for chttpserver) --- */
  uint8_t stream_diverted;  /* on_headers_complete handed this message off */
  uint8_t stream_body_done; /* the full body has been decoded */
  uint8_t stream_prepared;  /* http1_stream_read has run its one-time setup */
  int stream_err;           /* http1_stream_err_t reason for the last -1 */
  uint8_t *stream_primed;   /* leftover raw bytes captured at headers-complete
                                time, not yet fed through http1_parse */
  size_t stream_primed_len;
  size_t stream_primed_pos;
  uint8_t *stream_carry; /* decoded bytes that didn't fit the caller's
                             buffer on a previous http1_stream_read call */
  size_t stream_carry_len;
  size_t stream_carry_pos;
  uint8_t *stream_out_buf; /* current call's destination (valid only while
                               a http1_stream_read call is in progress) */
  size_t stream_out_cap;
  size_t stream_out_len;
  fio_lock_i stream_lock; /* guards stream_owned/stream_aborted below */
  uint8_t stream_owned;   /* a worker owns this request's ingestion, from
                              diversion until http1_stream_release() */
  uint8_t stream_aborted; /* http1_destroy ran while stream_owned was set;
                              http1_stream_release finishes the teardown */
  uint8_t buf[];
} http1pr_s;

struct http_vtable_s HTTP1_VTABLE; /* initialized later on */

/* *****************************************************************************
Internal Helpers
***************************************************************************** */

#define parser2http(x) \
  ((http1pr_s *)((uintptr_t)(x) - (uintptr_t)(&((http1pr_s *)0)->parser)))

inline static void h1_reset(http1pr_s *p) { p->header_size = 0; }

#define http1_pr2handle(pr) (((http1pr_s *)(pr))->request)
#define handle2pr(h) ((http1pr_s *)h->private_data.flag)

static fio_str_info_s http1pr_status2str(uintptr_t status);

/* cleanup an HTTP/1.1 handler object */
static inline void http1_after_finish(http_s *h) {
  http1pr_s *p = handle2pr(h);
  p->stop = p->stop & (~1UL);
  if (h != &p->request) {
    http_s_destroy(h, 0);
    fio_free(h);
  } else {
    http_s_clear(h, p->p.settings->log);
  }
  if (p->close) fio_close(p->p.uuid);
}

/* *****************************************************************************
HTTP Request / Response (Virtual) Functions
***************************************************************************** */
struct header_writer_s {
  FIOBJ dest;
  FIOBJ name;
  FIOBJ value;
};

static int write_header(FIOBJ o, void *w_) {
  struct header_writer_s *w = w_;
  if (!o) return 0;
  if (fiobj_hash_key_in_loop()) {
    w->name = fiobj_hash_key_in_loop();
  }
  if (FIOBJ_TYPE_IS(o, FIOBJ_T_ARRAY)) {
    fiobj_each1(o, 0, write_header, w);
    return 0;
  }
  fio_str_info_s name = fiobj_obj2cstr(w->name);
  fio_str_info_s str = fiobj_obj2cstr(o);
  if (!str.data) return 0;
  // fiobj_str_capa_assert(w->dest,
  //                       fiobj_obj2cstr(w->dest).len + name.len + str.len +
  //                       5);
  fiobj_str_write(w->dest, name.data, name.len);
  fiobj_str_write(w->dest, ":", 1);
  fiobj_str_write(w->dest, str.data, str.len);
  fiobj_str_write(w->dest, "\r\n", 2);
  return 0;
}

static FIOBJ headers2str(http_s *h, uintptr_t padding) {
  if (!h->method && !!h->status_str) return FIOBJ_INVALID;

  static uintptr_t connection_hash;
  if (!connection_hash) connection_hash = fiobj_hash_string("connection", 10);

  struct header_writer_s w;
  {
    const uintptr_t header_length_guess =
        fiobj_hash_count(h->private_data.out_headers) * 64;
    w.dest = fiobj_str_buf(header_length_guess + padding);
  }
  http1pr_s *p = handle2pr(h);

  if (p->is_client == 0) {
    fio_str_info_s t = http1pr_status2str(h->status);
    fiobj_str_write(w.dest, t.data, t.len);
    FIOBJ tmp = fiobj_hash_get2(h->private_data.out_headers, connection_hash);
    if (tmp) {
      t = fiobj_obj2cstr(tmp);
      if (t.data[0] == 'c' || t.data[0] == 'C') p->close = 1;
    } else {
      tmp = fiobj_hash_get2(h->headers, connection_hash);
      if (tmp) {
        t = fiobj_obj2cstr(tmp);
        if (!t.data || !t.len || t.data[0] == 'k' || t.data[0] == 'K')
          fiobj_str_write(w.dest, "connection:keep-alive\r\n", 23);
        else {
          fiobj_str_write(w.dest, "connection:close\r\n", 18);
          p->close = 1;
        }
      } else {
        t = fiobj_obj2cstr(h->version);
        if (!p->close && t.len > 7 && t.data && t.data[5] == '1' &&
            t.data[6] == '.' && t.data[7] == '1')
          fiobj_str_write(w.dest, "connection:keep-alive\r\n", 23);
        else {
          fiobj_str_write(w.dest, "connection:close\r\n", 18);
          p->close = 1;
        }
      }
    }
  } else {
    if (h->method) {
      fiobj_str_join(w.dest, h->method);
      fiobj_str_write(w.dest, " ", 1);
    } else {
      fiobj_str_write(w.dest, "GET ", 4);
    }
    fiobj_str_join(w.dest, h->path);
    if (h->query) {
      fiobj_str_write(w.dest, "?", 1);
      fiobj_str_join(w.dest, h->query);
    }
    fiobj_str_write(w.dest, " HTTP/1.1\r\n", 11);
    /* make sure we have a host header? */
    static uint64_t host_hash;
    if (!host_hash) host_hash = fiobj_hash_string("host", 4);
    FIOBJ tmp;
    if (!fiobj_hash_get2(h->private_data.out_headers, host_hash) &&
        (tmp = fiobj_hash_get2(h->headers, host_hash))) {
      fiobj_str_write(w.dest, "host:", 5);
      fiobj_str_join(w.dest, tmp);
      fiobj_str_write(w.dest, "\r\n", 2);
    }
    if (!fiobj_hash_get2(h->private_data.out_headers, connection_hash))
      fiobj_str_write(w.dest, "connection:keep-alive\r\n", 23);
  }

  fiobj_each1(h->private_data.out_headers, 0, write_header, &w);
  fiobj_str_write(w.dest, "\r\n", 2);
  return w.dest;
}

/** Should send existing headers and data */
static int http1_send_body(http_s *h, void *data, uintptr_t length) {
  FIOBJ packet = headers2str(h, length);
  if (!packet) {
    http1_after_finish(h);
    return -1;
  }
  fiobj_str_write(packet, data, length);
  fiobj_send_free((handle2pr(h)->p.uuid), packet);
  http1_after_finish(h);
  return 0;
}
/** Should send existing headers or complete streaming */
static void htt1p_finish(http_s *h) {
  FIOBJ packet = headers2str(h, 0);
  if (packet)
    fiobj_send_free((handle2pr(h)->p.uuid), packet);
  else {
    // fprintf(stderr, "WARNING: invalid call to `htt1p_finish`\n");
  }
  http1_after_finish(h);
}
/**
 * Called befor a pause task,
 */
static void http1_on_pause(http_s *h, http_fio_protocol_s *pr) {
  ((http1pr_s *)pr)->stop = 1;
  fio_suspend(pr->uuid);
  (void)h;
}

/**
 * called after the resume task had completed.
 */
static void http1_on_resume(http_s *h, http_fio_protocol_s *pr) {
  if (!((http1pr_s *)pr)->stop) {
    fio_force_event(pr->uuid, FIO_EVENT_ON_DATA);
  }
  (void)h;
}

/* *****************************************************************************
Virtual Table Decleration
***************************************************************************** */

struct http_vtable_s HTTP1_VTABLE = {
    .http_send_body = http1_send_body,
    .http_finish = htt1p_finish,
    .http_on_pause = http1_on_pause,
    .http_on_resume = http1_on_resume,
};

void *http1_vtable(void) { return (void *)&HTTP1_VTABLE; }

/* *****************************************************************************
Parser Callbacks
***************************************************************************** */

/** called when a request was received. */
static int http1_on_request(http1_parser_s *parser) {
  http1pr_s *p = parser2http(parser);
  if (p->stream_diverted) {
    /* the message was already handed off to a worker thread via
     * on_headers_complete; just record that the body is fully decoded.
     * `http1_parse` resets `parser->state` right after this returns, and
     * `http1_stream_read` (running on the worker) is the one that notices
     * `stream_body_done` and eventually drives `http_resume`. */
    p->stream_body_done = 1;
    h1_reset(p);
    return 0;
  }
  http_on_request_handler______internal(&http1_pr2handle(p), p->p.settings);
  if (p->request.method && !p->stop) http_finish(&p->request);
  h1_reset(p);
  return fio_is_closed(p->p.uuid);
}
/** called when a response was received. */
static int http1_on_response(http1_parser_s *parser) {
  http1pr_s *p = parser2http(parser);
  http_on_response_handler______internal(&http1_pr2handle(p), p->p.settings);
  if (p->request.status_str && !p->stop) http_finish(&p->request);
  h1_reset(p);
  return fio_is_closed(p->p.uuid);
}
/** called when a request method is parsed. */
static int http1_on_method(http1_parser_s *parser, char *method,
                           size_t method_len) {
  http1_pr2handle(parser2http(parser)).method =
      fiobj_str_new(method, method_len);
  parser2http(parser)->header_size += method_len;
  return 0;
}

/** called when a response status is parsed. the status_str is the string
 * without the prefixed numerical status indicator.*/
static int http1_on_status(http1_parser_s *parser, size_t status,
                           char *status_str, size_t len) {
  http1_pr2handle(parser2http(parser)).status_str =
      fiobj_str_new(status_str, len);
  http1_pr2handle(parser2http(parser)).status = status;
  parser2http(parser)->header_size += len;
  return 0;
}

/** called when a request path (excluding query) is parsed. */
static int http1_on_path(http1_parser_s *parser, char *path, size_t len) {
  http1_pr2handle(parser2http(parser)).path = fiobj_str_new(path, len);
  parser2http(parser)->header_size += len;
  return 0;
}

/** called when a request path (excluding query) is parsed. */
static int http1_on_query(http1_parser_s *parser, char *query, size_t len) {
  http1_pr2handle(parser2http(parser)).query = fiobj_str_new(query, len);
  parser2http(parser)->header_size += len;
  return 0;
}
/** called when a the HTTP/1.x version is parsed. */
static int http1_on_version(http1_parser_s *parser, char *version, size_t len) {
  http1_pr2handle(parser2http(parser)).version = fiobj_str_new(version, len);
  parser2http(parser)->header_size += len;
/* start counting - occurs on the first line of both requests and responses */
#if FIO_HTTP_EXACT_LOGGING
  clock_gettime(CLOCK_REALTIME,
                &http1_pr2handle(parser2http(parser)).received_at);
#else
  http1_pr2handle(parser2http(parser)).received_at = fio_last_tick();
#endif
  return 0;
}
/** called when a header is parsed. */
static int http1_on_header(http1_parser_s *parser, char *name, size_t name_len,
                           char *data, size_t data_len) {
  FIOBJ sym;
  FIOBJ obj;
  if (!http1_pr2handle(parser2http(parser)).headers) {
    FIO_LOG_ERROR(
        "(http1 parse ordering error) missing HashMap for header "
        "%s: %s",
        name, data);
    http_send_error2(500, parser2http(parser)->p.uuid,
                     parser2http(parser)->p.settings);
    return -1;
  }
  parser2http(parser)->header_size += name_len + data_len;
  if (parser2http(parser)->header_size >=
          parser2http(parser)->max_header_size ||
      fiobj_hash_count(http1_pr2handle(parser2http(parser)).headers) >
          HTTP_MAX_HEADER_COUNT) {
    if (parser2http(parser)->p.settings->log) {
      FIO_LOG_WARNING("(HTTP) security alert - header flood detected.");
    }
    http_send_error(&http1_pr2handle(parser2http(parser)), 413);
    return -1;
  }
  sym = fiobj_str_new(name, name_len);
  obj = fiobj_str_new(data, data_len);
  set_header_add(http1_pr2handle(parser2http(parser)).headers, sym, obj);
  fiobj_free(sym);
  return 0;
}
/** called when a body chunk is parsed. */
static int http1_on_body_chunk(http1_parser_s *parser, char *data,
                               size_t data_len) {
  if (parser->state.content_length >
          (ssize_t)parser2http(parser)->p.settings->max_body_size ||
      parser->state.read >
          (ssize_t)parser2http(parser)->p.settings->max_body_size) {
    http1pr_s *pr = parser2http(parser);
    if (pr->stream_diverted) {
      /* `h` is paused and owned by a worker thread at this point -- calling
       * http_send_error (a normal http_* API call) on it here would be
       * unsafe. Just flag the reason; http1_stream_read's caller reports it,
       * and http1_on_error (invoked below like any other parse error) defers
       * the actual close until after the worker's own error response has
       * been sent -- see http1_on_error for why. */
      pr->stream_err = HTTP1_STREAM_ERR_TOO_LARGE;
    } else {
      http_send_error(&http1_pr2handle(pr), 413);
    }
    return -1; /* test every time, in case of chunked data */
  }
  {
    http1pr_s *p = parser2http(parser);
    if (p->stream_diverted) {
      /* copy as much as fits into the current http1_stream_read caller's
       * buffer; anything past that spills into a carry-over buffer for the
       * next call, since a single fio_read may decode into more bytes than
       * the caller asked for in one batch. */
      size_t room = p->stream_out_cap - p->stream_out_len;
      size_t to_copy = data_len < room ? data_len : room;
      if (to_copy) {
        memcpy(p->stream_out_buf + p->stream_out_len, data, to_copy);
        p->stream_out_len += to_copy;
      }
      if (to_copy < data_len) {
        size_t spill = data_len - to_copy;
        uint8_t *nc =
            (uint8_t *)realloc(p->stream_carry, p->stream_carry_len + spill);
        if (!nc) return -1;
        memcpy(nc + p->stream_carry_len, data + to_copy, spill);
        p->stream_carry = nc;
        p->stream_carry_len += spill;
      }
      return 0;
    }
  }
  if (!parser->state.read) {
    if (parser->state.content_length > 0 &&
        parser->state.content_length <= HTTP_MAX_HEADER_LENGTH) {
      http1_pr2handle(parser2http(parser)).body = fiobj_data_newstr();
    } else {
      http1_pr2handle(parser2http(parser)).body = fiobj_data_newtmpfile();
    }
  }
  fiobj_data_write(http1_pr2handle(parser2http(parser)).body, data, data_len);
  return 0;
}

/** called once, right after headers are parsed and before any body byte is
 * consumed -- see the declaration in http1_parser.h for the return-value
 * contract. */
static int http1_on_headers_complete(http1_parser_s *parser, void *leftover,
                                     size_t leftover_len) {
  http1pr_s *p = parser2http(parser);
  if (!p->p.settings->on_headers_complete) return 0;

  /* `p` is reused across every request on a keep-alive connection -- reset
   * all per-message ingestion state left over from a previous message
   * before deciding anything about this one. */
  free(p->stream_primed);
  free(p->stream_carry);
  p->stream_primed = NULL;
  p->stream_primed_len = p->stream_primed_pos = 0;
  p->stream_carry = NULL;
  p->stream_carry_len = p->stream_carry_pos = 0;
  p->stream_diverted = 0;
  p->stream_body_done = 0;
  p->stream_prepared = 0;
  p->stream_err = HTTP1_STREAM_ERR_NONE;
  /* stream_owned/stream_aborted are intentionally not touched here: they are
   * only ever set once diversion is decided below, and http1_destroy/
   * http1_stream_release already guarantee they're back to a clean 0/0
   * state before a next message's headers can possibly complete (the
   * connection cannot advance to a new request while a previous one is
   * still owned). */

  /* Default to closing the connection after this response. http_send_error
   * (called synchronously by the settings hook below, or right here on an
   * OOM) sends its response through the normal headers2str path *before*
   * this function returns -- so close must already be set by the time
   * either of those runs, not after. It's cleared below only once dispatch
   * is confirmed, restoring normal keep-alive behavior for a matched
   * route; headers2str can still independently force it back to 1 later
   * from the client's own Connection header / HTTP version, since it only
   * ever sets this flag, never clears it. */
  p->close = 1;

  if (leftover_len) {
    uint8_t *primed = (uint8_t *)malloc(leftover_len);
    if (!primed) {
      http_send_error(&http1_pr2handle(p), 500);
      return 1;
    }
    memcpy(primed, leftover, leftover_len);
    p->stream_primed = primed;
    p->stream_primed_len = leftover_len;
    p->stream_primed_pos = 0;
  }
  if (p->p.settings->on_headers_complete(&http1_pr2handle(p))) {
    p->stream_diverted = 1;
    p->stream_owned = 1;
    p->close = 0;
    /* A request with no body at all (no Content-Length/Transfer-Encoding,
     * or Content-Length: 0) is already "complete" per http1_consume_body's
     * own rules -- but that function is never reached for a diverted
     * message (http1_parse returns immediately, right below, without
     * running the body-consumption switch case). Run the same check here
     * with a zero-length span so http1_stream_read doesn't block forever
     * waiting for bytes that will never arrive; http1_consume_body is a
     * no-op for any case that genuinely still expects body bytes (verified
     * safe: with length 0 neither http1_consume_body_streamed nor
     * _chunked ever dereferences the dummy pointer, they only compare it
     * against itself). */
    uint8_t dummy;
    uint8_t *dummy_start = &dummy;
    http1_consume_body(&p->parser, &dummy, 0, &dummy_start);
    if (p->parser.state.reserved & HTTP1_P_FLAG_COMPLETE) {
      p->stream_body_done = 1;
      /* http1_stream_read will never call http1_parse for this message now
       * (there's nothing to read), so it will never reach the reset that
       * normally happens right after http1_on_request runs. Do it here --
       * otherwise the next request parsed on this (kept-alive) connection
       * inherits stale reserved/content_length bits and gets misparsed. */
      p->parser.state = (struct http1_parser_protected_read_only_state_s){0};
    }
  } else {
    /* not diverted: either the callback rejected the request synchronously
     * (e.g. no matching route) or chose not to take it over. Either way the
     * primed bytes were never consumed by anyone, and `close` is already 1
     * from above -- the client's still-arriving body would otherwise be
     * misread as the start of a new pipelined request. */
    free(p->stream_primed);
    p->stream_primed = NULL;
    p->stream_primed_len = 0;
    p->stream_primed_pos = 0;
  }
  return 1;
}

/** called when a protocol error occurred. */
static int http1_on_error(http1_parser_s *parser) {
  http1pr_s *p = parser2http(parser);
  if (p->close) return -1;
  if (p->stream_diverted) {
    /* `h` is paused and owned by a worker thread at this point -- do not
     * force-close the socket synchronously here, or any error response the
     * worker later computes (e.g. 413 for a too-large body, via
     * http1_on_body_chunk's HTTP1_STREAM_ERR_TOO_LARGE) would have nowhere
     * to go: fio_close's immediate/no-pending-packet branch would tear down
     * the connection before http_resume ever runs, making that response
     * silently unreachable (a real bug this fixes -- verified by a raw
     * socket test that observed a bare connection reset with zero response
     * bytes before this change). Record a reason if one hasn't already been
     * set more specifically, force the response that follows to carry
     * Connection: close (mirroring the "reject synchronously" path in
     * http1_on_headers_complete, and matching headers2str's existing
     * once-set-never-cleared handling of this flag), and let
     * http1_after_finish's existing `if (p->close) fio_close(...)` close the
     * connection gracefully -- discarding any excess unread bytes -- once
     * that response has actually been sent. */
    if (p->stream_err == HTTP1_STREAM_ERR_NONE)
      p->stream_err = HTTP1_STREAM_ERR_PROTOCOL;
    p->close = 1;
    return -1;
  }
  FIO_LOG_DEBUG("HTTP parser error.");
  fio_close(p->p.uuid);
  return -1;
}

/* *****************************************************************************
Connection Callbacks
***************************************************************************** */

static inline void http1_consume_data(intptr_t uuid, http1pr_s *p) {
  if (fio_pending(uuid) > 4) {
    goto throttle;
  }
  ssize_t i = 0;
  size_t org_len = p->buf_len;
  int pipeline_limit = 8;
  if (!p->buf_len) return;
  do {
    i = http1_parse(&p->parser, p->buf + (org_len - p->buf_len), p->buf_len);
    p->buf_len -= i;
    --pipeline_limit;
  } while (i && p->buf_len && pipeline_limit && !p->stop);

  if (p->buf_len && org_len != p->buf_len) {
    memmove(p->buf, p->buf + (org_len - p->buf_len), p->buf_len);
  }

  if (p->buf_len == HTTP_MAX_HEADER_LENGTH) {
    /* no room to read... parser not consuming data */
    if (p->request.method)
      http_send_error(&p->request, 413);
    else {
      p->request.method = fiobj_str_tmp();
      http_send_error(&p->request, 413);
    }
  }

  if (!pipeline_limit) {
    fio_force_event(uuid, FIO_EVENT_ON_DATA);
  }
  return;

throttle:
  /* throttle busy clients (slowloris) */
  p->stop |= 4;
  fio_suspend(uuid);
  FIO_LOG_DEBUG("(HTTP/1,1) throttling client at %.*s",
                (int)fio_peer_addr(uuid).len, fio_peer_addr(uuid).data);
}

/** called when a data is available, but will not run concurrently */
static void http1_on_data(intptr_t uuid, fio_protocol_s *protocol) {
  http1pr_s *p = (http1pr_s *)protocol;
  if (p->stop) {
    fio_suspend(uuid);
    return;
  }
  ssize_t i = 0;
  if (HTTP_MAX_HEADER_LENGTH - p->buf_len)
    i = fio_read(uuid, p->buf + p->buf_len,
                 HTTP_MAX_HEADER_LENGTH - p->buf_len);
  if (i > 0) {
    p->buf_len += i;
  }
  http1_consume_data(uuid, p);
}

/** called when the connection was closed, but will not run concurrently */
static void http1_on_close(intptr_t uuid, fio_protocol_s *protocol) {
  http1_destroy(protocol);
  (void)uuid;
}

/** called when the connection was closed, but will not run concurrently */
static void http1_on_ready(intptr_t uuid, fio_protocol_s *protocol) {
  /* resume slow clients from suspension */
  http1pr_s *p = (http1pr_s *)protocol;
  if (p->stop & 4) {
    p->stop ^= 4; /* flip back the bit, so it's zero */
    fio_force_event(uuid, FIO_EVENT_ON_DATA);
  }
  (void)protocol;
}

/** called when a data is available for the first time */
static void http1_on_data_first_time(intptr_t uuid, fio_protocol_s *protocol) {
  http1pr_s *p = (http1pr_s *)protocol;
  ssize_t i;

  i = fio_read(uuid, p->buf + p->buf_len, HTTP_MAX_HEADER_LENGTH - p->buf_len);

  if (i <= 0) return;
  p->buf_len += i;

  /* ensure future reads skip this first time HTTP/2.0 test */
  p->p.protocol.on_data = http1_on_data;
  if (i >= 24 && !memcmp(p->buf, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24)) {
    FIO_LOG_WARNING("client claimed unsupported HTTP/2 prior knowledge.");
    fio_close(uuid);
    return;
  }

  /* Finish handling the same way as the normal `on_data` */
  http1_consume_data(uuid, p);
}

/* *****************************************************************************
Public API
***************************************************************************** */

/** Creates an HTTP1 protocol object and handles any unread data in the buffer
 * (if any). */
fio_protocol_s *http1_new(uintptr_t uuid, http_settings_s *settings,
                          void *unread_data, size_t unread_length) {
  if (unread_data && unread_length > HTTP_MAX_HEADER_LENGTH) return NULL;
  http1pr_s *p = fio_malloc(sizeof(*p) + HTTP_MAX_HEADER_LENGTH);
  // FIO_LOG_DEBUG("Allocated HTTP/1.1 protocol at. %p", (void *)p);
  FIO_ASSERT_ALLOC(p);
  *p = (http1pr_s){
      .p.protocol =
          {
              .on_data = http1_on_data_first_time,
              .on_close = http1_on_close,
              .on_ready = http1_on_ready,
          },
      .p.uuid = uuid,
      .p.settings = settings,
      .max_header_size = settings->max_header_size,
      .is_client = settings->is_client,
  };
  http_s_new(&p->request, &p->p, &HTTP1_VTABLE);
  if (unread_data && unread_length <= HTTP_MAX_HEADER_LENGTH) {
    memcpy(p->buf, unread_data, unread_length);
    p->buf_len = unread_length;
  }
  fio_attach(uuid, &p->p.protocol);
  if (unread_data && unread_length <= HTTP_MAX_HEADER_LENGTH) {
    fio_force_event(uuid, FIO_EVENT_ON_DATA);
  }
  return &p->p.protocol;
}

/** Manually destroys the HTTP1 protocol object. */
void http1_destroy(fio_protocol_s *pr) {
  http1pr_s *p = (http1pr_s *)pr;
  fio_lock(&p->stream_lock);
  if (p->stream_owned) {
    /* a worker thread still owns this request's body ingestion (it may be
     * blocked inside http1_stream_read, or between calls to it) -- freeing
     * `p` here would race with its use of `p->parser`/`p->buf`/`p->stream_*`.
     * Defer: http1_stream_release will notice `stream_aborted` and finish
     * the teardown once the worker is done touching `p`. */
    p->stream_aborted = 1;
    fio_unlock(&p->stream_lock);
    return;
  }
  fio_unlock(&p->stream_lock);
  free(p->stream_primed);
  free(p->stream_carry);
  http1_pr2handle(p).status = 0;
  http_s_destroy(&http1_pr2handle(p), 0);
  fio_free(p);
  // FIO_LOG_DEBUG("Deallocated HTTP/1.1 protocol at. %p", (void *)p);
}

/* *****************************************************************************
Worker-driven body ingestion
***************************************************************************** */

/* Stashes unconsumed bytes (belonging to a pipelined next request) into the
 * connection's own recv buffer, so the normal on_data path picks them up
 * once the connection is resumed. `len` is bounded by
 * HTTP1_STREAM_SCRATCH_SIZE, which is sized well under the buffer's actual
 * capacity (HTTP_MAX_HEADER_LENGTH). */
static void http1_stash_pipelined(http1pr_s *p, const void *data, size_t len) {
  if (!len) return;
  memmove(p->buf, data, len);
  p->buf_len = len;
}

/**
 * Reads and decodes up to `buflen` bytes of the request body -- see the
 * declaration in http1.h for the full contract.
 */
ssize_t http1_stream_read(http_s *h, void *buf, size_t buflen,
                          unsigned timeout_ms) {
  http1pr_s *p = handle2pr(h);

  fio_lock(&p->stream_lock);
  int already_aborted = p->stream_aborted;
  fio_unlock(&p->stream_lock);
  if (already_aborted) {
    p->stream_err = HTTP1_STREAM_ERR_CLOSED;
    return -1;
  }

  if (!p->stream_prepared) {
    /* One-time setup, guaranteed to run strictly after the reactor-thread
     * call chain that diverted this request (http1_consume_data/http1_parse)
     * has fully unwound, since the worker only starts via a deferred ctpool
     * dispatch. http1_consume_data redundantly re-compacts the same leftover
     * bytes we already copied into stream_primed back to the front of
     * p->buf after we divert (its own bookkeeping has no notion of
     * diversion) -- discard that stale duplicate here rather than in
     * on_headers_complete itself, where p->buf_len is still involved in the
     * caller's own in-progress arithmetic. */
    p->stream_prepared = 1;
    p->buf_len = 0;
  }

  if (!buflen) return 0;
  if (p->stream_body_done && p->stream_carry_pos >= p->stream_carry_len)
    return 0;

  p->stream_out_buf = (uint8_t *)buf;
  p->stream_out_cap = buflen;
  p->stream_out_len = 0;

  /* hand out leftover decoded bytes from a previous call first -- a single
   * raw read can decode into more bytes than one caller-supplied buffer can
   * hold. */
  if (p->stream_carry_len > p->stream_carry_pos) {
    size_t n = p->stream_carry_len - p->stream_carry_pos;
    if (n > p->stream_out_cap) n = p->stream_out_cap;
    memcpy(p->stream_out_buf, p->stream_carry + p->stream_carry_pos, n);
    p->stream_out_len = n;
    p->stream_carry_pos += n;
    if (p->stream_carry_pos == p->stream_carry_len) {
      free(p->stream_carry);
      p->stream_carry = NULL;
      p->stream_carry_len = p->stream_carry_pos = 0;
    }
  }

  if (p->stream_out_len) return (ssize_t)p->stream_out_len;

  ssize_t ret;
  for (;;) {
    uint8_t scratch[HTTP1_STREAM_SCRATCH_SIZE];
    uint8_t *raw;
    size_t raw_len;
    int from_primed = 0;
    if (p->stream_primed_len > p->stream_primed_pos) {
      raw = p->stream_primed + p->stream_primed_pos;
      raw_len = p->stream_primed_len - p->stream_primed_pos;
      from_primed = 1;
    } else {
      /* p->buf_len may hold a framing fragment (e.g. an incomplete
       * chunk-size line or inter-chunk CRLF) that a previous iteration of
       * this same loop could not finish parsing -- see the from_primed and
       * consumed < raw_len handling below. Prepend it so http1_parse sees it
       * joined with fresh data, instead of being discarded here (silently
       * dropped, corrupting the chunked decode) or handed the same
       * insufficient bytes forever (an infinite loop). */
      size_t carry_over = p->buf_len;
      if (carry_over >= sizeof(scratch)) {
        /* A single framing token can't legitimately exceed the scratch size
         * (see the HTTP1_STREAM_SCRATCH_SIZE comment above) -- treat this as
         * a protocol violation rather than loop forever with no room left to
         * read fresh bytes into. */
        p->stream_err = HTTP1_STREAM_ERR_PROTOCOL;
        return -1;
      }
      if (carry_over) memcpy(scratch, p->buf, carry_over);
      ssize_t n = fio_read(p->p.uuid, scratch + carry_over,
                           sizeof(scratch) - carry_over);
      if (n > 0) {
        p->buf_len = 0; /* carry_over is now folded into `raw` below */
        raw = scratch;
        raw_len = carry_over + (size_t)n;
      } else if (n == 0) {
        struct pollfd pfd;
        pfd.fd = (int)fio_uuid2fd(p->p.uuid);
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, timeout_ms ? (int)timeout_ms : -1);
        if (pr == 0) {
          p->stream_err = HTTP1_STREAM_ERR_TIMEOUT;
          return -1;
        }
        /* poll()'s readiness is only a wake-up hint -- always re-validate by
         * calling fio_read again rather than trusting revents, since the fd
         * number could in principle have been recycled for an unrelated
         * connection by the time we wake. carry_over (if any) is untouched
         * in p->buf and will be retried on the next iteration. */
        continue;
      } else {
        p->stream_err = HTTP1_STREAM_ERR_CLOSED;
        return -1;
      }
    }

    size_t consumed = http1_parse(&p->parser, raw, raw_len);
    if (from_primed) p->stream_primed_pos += consumed;
    if (p->stream_err) {
      /* http1_on_body_chunk flagged a fatal condition (e.g. too-large body)
       * during this call; http1_parse already force-closed the connection
       * via http1_on_error. Don't loop again waiting on a dead socket. */
      return -1;
    }
    if (consumed < raw_len) {
      http1_stash_pipelined(p, raw + consumed, raw_len - consumed);
      if (from_primed)
        /* Primed data alone couldn't complete this framing token (e.g. the
         * connection's very first read contained headers plus a partial
         * chunk-size line). The unconsumed remainder was just moved into
         * p->buf above -- mark primed fully drained so the next iteration
         * falls through to the buf-carry + fresh-read path instead of
         * re-presenting these same insufficient bytes forever. */
        p->stream_primed_pos = p->stream_primed_len;
    }

    if (p->stream_out_len) {
      ret = (ssize_t)p->stream_out_len;
      break;
    }
    if (p->stream_body_done) {
      ret = 0;
      break;
    }
    /* else: this raw read only produced framing overhead (e.g. a chunk-size
     * line, possibly still incomplete) with no payload bytes yet, and the
     * body isn't done -- loop for more raw input. Any incomplete fragment
     * was just stashed into p->buf above and will be prepended to the next
     * raw read at the top of this loop. */
  }
  return ret;
}

http1_stream_err_t http1_stream_last_error(http_s *h) {
  return (http1_stream_err_t)handle2pr(h)->stream_err;
}

void http1_stream_release(http_s *h) {
  http1pr_s *p = handle2pr(h);
  if (!p->stream_body_done) {
    /* The body was never fully drained (a streaming handler stopped early,
     * or ingestion hit an error/timeout/abort before EOF). Whatever bytes
     * the client still has in flight for this body would otherwise be
     * misread as the start of the next request on this connection -- force
     * it closed after the response instead of attempting keep-alive. */
    p->close = 1;
  }
  fio_lock(&p->stream_lock);
  p->stream_owned = 0;
  int aborted = p->stream_aborted;
  fio_unlock(&p->stream_lock);
  if (aborted) http1_destroy(&p->p.protocol);
}

/* *****************************************************************************
Protocol Data
***************************************************************************** */

// clang-format off
#define HTTP_SET_STATUS_STR(status, str) [((status)-100)] = { .data = (char*)("HTTP/1.1 " #status " " str "\r\n"), .len = (sizeof("HTTP/1.1 " #status " " str "\r\n") - 1) }
// #undef HTTP_SET_STATUS_STR
// clang-format on

static fio_str_info_s http1pr_status2str(uintptr_t status) {
  static fio_str_info_s status2str[] = {
      HTTP_SET_STATUS_STR(100, "Continue"),
      HTTP_SET_STATUS_STR(101, "Switching Protocols"),
      HTTP_SET_STATUS_STR(102, "Processing"),
      HTTP_SET_STATUS_STR(103, "Early Hints"),
      HTTP_SET_STATUS_STR(200, "OK"),
      HTTP_SET_STATUS_STR(201, "Created"),
      HTTP_SET_STATUS_STR(202, "Accepted"),
      HTTP_SET_STATUS_STR(203, "Non-Authoritative Information"),
      HTTP_SET_STATUS_STR(204, "No Content"),
      HTTP_SET_STATUS_STR(205, "Reset Content"),
      HTTP_SET_STATUS_STR(206, "Partial Content"),
      HTTP_SET_STATUS_STR(207, "Multi-Status"),
      HTTP_SET_STATUS_STR(208, "Already Reported"),
      HTTP_SET_STATUS_STR(226, "IM Used"),
      HTTP_SET_STATUS_STR(300, "Multiple Choices"),
      HTTP_SET_STATUS_STR(301, "Moved Permanently"),
      HTTP_SET_STATUS_STR(302, "Found"),
      HTTP_SET_STATUS_STR(303, "See Other"),
      HTTP_SET_STATUS_STR(304, "Not Modified"),
      HTTP_SET_STATUS_STR(305, "Use Proxy"),
      HTTP_SET_STATUS_STR(306, "(Unused), "),
      HTTP_SET_STATUS_STR(307, "Temporary Redirect"),
      HTTP_SET_STATUS_STR(308, "Permanent Redirect"),
      HTTP_SET_STATUS_STR(400, "Bad Request"),
      HTTP_SET_STATUS_STR(403, "Forbidden"),
      HTTP_SET_STATUS_STR(404, "Not Found"),
      HTTP_SET_STATUS_STR(401, "Unauthorized"),
      HTTP_SET_STATUS_STR(402, "Payment Required"),
      HTTP_SET_STATUS_STR(405, "Method Not Allowed"),
      HTTP_SET_STATUS_STR(406, "Not Acceptable"),
      HTTP_SET_STATUS_STR(407, "Proxy Authentication Required"),
      HTTP_SET_STATUS_STR(408, "Request Timeout"),
      HTTP_SET_STATUS_STR(409, "Conflict"),
      HTTP_SET_STATUS_STR(410, "Gone"),
      HTTP_SET_STATUS_STR(411, "Length Required"),
      HTTP_SET_STATUS_STR(412, "Precondition Failed"),
      HTTP_SET_STATUS_STR(413, "Payload Too Large"),
      HTTP_SET_STATUS_STR(414, "URI Too Long"),
      HTTP_SET_STATUS_STR(415, "Unsupported Media Type"),
      HTTP_SET_STATUS_STR(416, "Range Not Satisfiable"),
      HTTP_SET_STATUS_STR(417, "Expectation Failed"),
      HTTP_SET_STATUS_STR(421, "Misdirected Request"),
      HTTP_SET_STATUS_STR(422, "Unprocessable Entity"),
      HTTP_SET_STATUS_STR(423, "Locked"),
      HTTP_SET_STATUS_STR(424, "Failed Dependency"),
      HTTP_SET_STATUS_STR(425, "Unassigned"),
      HTTP_SET_STATUS_STR(426, "Upgrade Required"),
      HTTP_SET_STATUS_STR(427, "Unassigned"),
      HTTP_SET_STATUS_STR(428, "Precondition Required"),
      HTTP_SET_STATUS_STR(429, "Too Many Requests"),
      HTTP_SET_STATUS_STR(430, "Unassigned"),
      HTTP_SET_STATUS_STR(431, "Request Header Fields Too Large"),
      HTTP_SET_STATUS_STR(500, "Internal Server Error"),
      HTTP_SET_STATUS_STR(501, "Not Implemented"),
      HTTP_SET_STATUS_STR(502, "Bad Gateway"),
      HTTP_SET_STATUS_STR(503, "Service Unavailable"),
      HTTP_SET_STATUS_STR(504, "Gateway Timeout"),
      HTTP_SET_STATUS_STR(505, "HTTP Version Not Supported"),
      HTTP_SET_STATUS_STR(506, "Variant Also Negotiates"),
      HTTP_SET_STATUS_STR(507, "Insufficient Storage"),
      HTTP_SET_STATUS_STR(508, "Loop Detected"),
      HTTP_SET_STATUS_STR(509, "Unassigned"),
      HTTP_SET_STATUS_STR(510, "Not Extended"),
      HTTP_SET_STATUS_STR(511, "Network Authentication Required"),
  };
  fio_str_info_s ret = (fio_str_info_s){.len = 0, .data = NULL};
  if (status >= 100 &&
      (status - 100) < sizeof(status2str) / sizeof(status2str[0]))
    ret = status2str[status - 100];
  if (!ret.data) {
    ret = status2str[400];
  }
  return ret;
}
#undef HTTP_SET_STATUS_STR
