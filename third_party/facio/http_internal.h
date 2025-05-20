/*
Copyright: Boaz Segev, 2016-2019
License: MIT

Feel free to copy, use and enjoy according to the license provided.
*/
#ifndef H_HTTP_INTERNAL_H
#define H_HTTP_INTERNAL_H

#include <fio.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fio.h>
#include <http.h>

/* *****************************************************************************
Types
***************************************************************************** */

typedef struct http_fio_protocol_s http_fio_protocol_s;
typedef struct http_vtable_s http_vtable_s;

struct http_vtable_s {
  /** Should send existing headers and data */
  int (*const http_send_body)(http_s *h, void *data, uintptr_t length);
  /** Should send existing headers and data and prepare for streaming */
  int (*const http_stream)(http_s *h, void *data, uintptr_t length);
  /** Should send existing headers or complete streaming */
  void (*const http_finish)(http_s *h);
  /** Pauses the request / response handling. */
  void (*http_on_pause)(http_s *, http_fio_protocol_s *);
  /** Resumes a request / response handling. */
  void (*http_on_resume)(http_s *, http_fio_protocol_s *);
};

struct http_fio_protocol_s {
  fio_protocol_s protocol;   /* facil.io protocol */
  intptr_t uuid;             /* socket uuid */
  http_settings_s *settings; /* pointer to HTTP settings */
};

#define http2protocol(h) ((http_fio_protocol_s *)h->private_data.flag)

/* *****************************************************************************
Constants that shouldn't be accessed by the users (`fiobj_dup` required).
***************************************************************************** */

extern FIOBJ HTTP_HEADER_ACCEPT_RANGES;
extern FIOBJ HTTP_HVALUE_BYTES;
extern FIOBJ HTTP_HVALUE_CLOSE;
extern FIOBJ HTTP_HVALUE_CONTENT_TYPE_DEFAULT;
extern FIOBJ HTTP_HVALUE_GZIP;
extern FIOBJ HTTP_HVALUE_KEEP_ALIVE;
extern FIOBJ HTTP_HVALUE_MAX_AGE;

/* *****************************************************************************
HTTP request/response object management
***************************************************************************** */

static inline void http_s_new(http_s *h, http_fio_protocol_s *owner,
                              http_vtable_s *vtbl) {
  *h = (http_s){
      .private_data =
          {
              .vtbl = vtbl,
              .flag = (uintptr_t)owner,
              .out_headers = fiobj_hash_new(),
          },
      .headers = fiobj_hash_new(),
      .received_at = fio_last_tick(),
      .status = 200,
  };
}

static inline void http_s_destroy(http_s *h, uint8_t log) {
  if (log && h->status && !h->status_str) {
    http_write_log(h);
  }
  fiobj_free(h->method);
  fiobj_free(h->status_str);
  fiobj_free(h->private_data.out_headers);
  fiobj_free(h->headers);
  fiobj_free(h->version);
  fiobj_free(h->query);
  fiobj_free(h->path);
  fiobj_free(h->cookies);
  fiobj_free(h->body);
  fiobj_free(h->params);

  *h = (http_s){
      .private_data.vtbl = h->private_data.vtbl,
      .private_data.flag = h->private_data.flag,
  };
}

static inline void http_s_clear(http_s *h, uint8_t log) {
  http_s_destroy(h, log);
  http_s_new(h, (http_fio_protocol_s *)h->private_data.flag,
             h->private_data.vtbl);
}

/** tests handle validity */
#define HTTP_INVALID_HANDLE(h) \
  (!(h) || (!(h)->method && !(h)->status_str && (h)->status))

/* *****************************************************************************
Request / Response Handlers
***************************************************************************** */

/** Use this function to handle HTTP requests.*/
void http_on_request_handler______internal(http_s *h,
                                           http_settings_s *settings);

void http_on_response_handler______internal(http_s *h,
                                            http_settings_s *settings);
int http_send_error2(size_t error, intptr_t uuid, http_settings_s *settings);

/* *****************************************************************************
Helpers
***************************************************************************** */

/** sets an outgoing header only if it doesn't exist */
static inline void set_header_if_missing(FIOBJ hash, FIOBJ name, FIOBJ value) {
  FIOBJ old = fiobj_hash_replace(hash, name, value);
  if (!old) return;
  fiobj_hash_replace(hash, name, old);
  fiobj_free(value);
}

/** sets an outgoing header, collecting duplicates in an Array (i.e. cookies)
 */
static inline void set_header_add(FIOBJ hash, FIOBJ name, FIOBJ value) {
  FIOBJ old = fiobj_hash_replace(hash, name, value);
  if (!old) return;
  if (!value) {
    fiobj_free(old);
    return;
  }
  if (!FIOBJ_TYPE_IS(old, FIOBJ_T_ARRAY)) {
    FIOBJ tmp = fiobj_ary_new();
    fiobj_ary_push(tmp, old);
    old = tmp;
  }
  if (FIOBJ_TYPE_IS(value, FIOBJ_T_ARRAY)) {
    for (size_t i = 0; i < fiobj_ary_count(value); ++i) {
      fiobj_ary_push(old, fiobj_dup(fiobj_ary_index(value, i)));
    }
    /* frees `value` */
    fiobj_hash_set(hash, name, old);
    return;
  }
  /* value will be owned by both hash and array */
  fiobj_ary_push(old, value);
  /* don't free `value` (leave in array) */
  fiobj_hash_replace(hash, name, old);
}

/* Explicit lifecycle: registers http_lib_init (ON_INITIALIZE) and
 * http_lib_cleanup (AT_EXIT) with facil.io's state callback system.
 * Must be called before fio_lib_init() so the ON_INITIALIZE callback
 * is queued before fio_lib_init fires it. */
void http_lib_constructor(void);

#endif /* H_HTTP_INTERNAL_H */
