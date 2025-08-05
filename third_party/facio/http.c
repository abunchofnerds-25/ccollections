/*
Copyright: Boaz Segev, 2016-2019
License: MIT

Feel free to copy, use and enjoy according to the license provided.
*/

#include <ctype.h>
#include <fcntl.h>
#include <fio.h>
#include <http1.h>
#include <http_internal.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef HAVE_TM_TM_ZONE
#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
    defined(__DragonFly__) || defined(__bsdi__) || defined(__ultrix) ||    \
    (defined(__APPLE__) && defined(__MACH__)) ||                           \
    (defined(__sun) && !defined(__SVR4))
/* Known BSD systems */
#define HAVE_TM_TM_ZONE 1
#elif defined(__GLIBC__) && defined(_BSD_SOURCE)
/* GNU systems with _BSD_SOURCE */
#define HAVE_TM_TM_ZONE 1
#else
#define HAVE_TM_TM_ZONE 0
#endif
#endif

/* *****************************************************************************
SSL/TLS patch
***************************************************************************** */

/**
 * Adds an ALPN protocol callback to the SSL/TLS context.
 *
 * The first protocol added will act as the default protocol to be selected.
 */
void __attribute__((weak)) fio_tls_alpn_add(
    void *tls, const char *protocol_name,
    void (*callback)(intptr_t uuid, void *udata_connection, void *udata_tls),
    void *udata_tls, void (*on_cleanup)(void *udata_tls)) {
  FIO_LOG_FATAL("HTTP SSL/TLS required but unavailable!");
  exit(-1);
  (void)tls;
  (void)protocol_name;
  (void)callback;
  (void)on_cleanup;
  (void)udata_tls;
}
#pragma weak fio_tls_alpn_add

/* *****************************************************************************
Small Helpers
***************************************************************************** */
static inline int hex2byte(uint8_t *dest, const uint8_t *source);

static inline void add_content_length(http_s *r, uintptr_t length) {
  static uint64_t cl_hash = 0;
  if (!cl_hash) cl_hash = fiobj_hash_string("content-length", 14);
  if (!fiobj_hash_get2(r->private_data.out_headers, cl_hash)) {
    fiobj_hash_set(r->private_data.out_headers, HTTP_HEADER_CONTENT_LENGTH,
                   fiobj_num_new(length));
  }
}

static FIOBJ current_date;
static time_t last_date_added;
static fio_lock_i date_lock;
static inline void add_date(http_s *r) {
  static uint64_t date_hash = 0;
  if (!date_hash) date_hash = fiobj_hash_string("date", 4);
  static uint64_t mod_hash = 0;
  if (!mod_hash) mod_hash = fiobj_hash_string("last-modified", 13);

  if (fio_last_tick().tv_sec > last_date_added) {
    fio_lock(&date_lock);
    if (fio_last_tick().tv_sec > last_date_added) { /* retest inside lock */
      FIOBJ tmp = fiobj_str_buf(32);
      FIOBJ old = current_date;
      fiobj_str_resize(
          tmp, http_time2str(fiobj_obj2cstr(tmp).data, fio_last_tick().tv_sec));
      last_date_added = fio_last_tick().tv_sec;
      current_date = tmp;
      fiobj_free(old);
    }
    fio_unlock(&date_lock);
  }

  if (!fiobj_hash_get2(r->private_data.out_headers, date_hash)) {
    fiobj_hash_set(r->private_data.out_headers, HTTP_HEADER_DATE,
                   fiobj_dup(current_date));
  }
  if (r->status_str == FIOBJ_INVALID &&
      !fiobj_hash_get2(r->private_data.out_headers, mod_hash)) {
    fiobj_hash_set(r->private_data.out_headers, HTTP_HEADER_LAST_MODIFIED,
                   fiobj_dup(current_date));
  }
}

/* *****************************************************************************
The Request / Response type and functions
***************************************************************************** */
/**
 * Sets a response header, taking ownership of the value object, but NOT the
 * name object (so name objects could be reused in future responses).
 *
 * Returns -1 on error and 0 on success.
 */
int http_set_header(http_s *r, FIOBJ name, FIOBJ value) {
  if (HTTP_INVALID_HANDLE(r) || !name) {
    fiobj_free(value);
    return -1;
  }
  set_header_add(r->private_data.out_headers, name, value);
  return 0;
}
/**
 * Sets a response header.
 *
 * Returns -1 on error and 0 on success.
 */
int http_set_header2(http_s *r, fio_str_info_s n, fio_str_info_s v) {
  if (HTTP_INVALID_HANDLE(r) || !n.data || !n.len || (v.data && !v.len))
    return -1;
  FIOBJ tmp = fiobj_str_new(n.data, n.len);
  int ret = http_set_header(r, tmp, fiobj_str_new(v.data, v.len));
  fiobj_free(tmp);
  return ret;
}

int http_send_body(http_s *r, void *data, uintptr_t length) {
  if (HTTP_INVALID_HANDLE(r)) return -1;
  if (!length || !data) {
    http_finish(r);
    return 0;
  }
  add_content_length(r, length);
  // add_content_type(r);
  add_date(r);
  return ((http_vtable_s *)r->private_data.vtbl)
      ->http_send_body(r, data, length);
}
/**
 * Sends an HTTP error response.
 *
 * Returns -1 on error and 0 on success.
 *
 * AFTER THIS FUNCTION IS CALLED, THE `http_s` OBJECT IS NO LONGER VALID.
 *
 * The `uuid` argument is optional and will be used only if the `http_s`
 * argument is set to NULL.
 */
int http_send_error(http_s *r, size_t error) {
  if (!r || !r->private_data.out_headers) {
    return -1;
  }
  if (error < 100 || error >= 1000) error = 500;
  r->status = error;
  http_set_header(r, HTTP_HEADER_CONTENT_TYPE,
                  http_mimetype_find((char *)"txt", 3));
  fio_str_info_s t = http_status2str(error);
  http_send_body(r, t.data, t.len);
  return 0;
}

/**
 * Sends the response headers for a header only response.
 *
 * AFTER THIS FUNCTION IS CALLED, THE `http_s` OBJECT IS NO LONGER VALID.
 */
void http_finish(http_s *r) {
  if (!r || !r->private_data.vtbl) {
    return;
  }
  add_content_length(r, 0);
  add_date(r);
  ((http_vtable_s *)r->private_data.vtbl)->http_finish(r);
}
/* *****************************************************************************
Pause / Resume
***************************************************************************** */
struct http_pause_handle_s {
  uintptr_t uuid;
  http_s *h;
  void *udata;
  void (*task)(http_s *);
  void (*fallback)(void *);
};

/** Returns the `udata` associated with the paused opaque handle */
void *http_paused_udata_get(http_pause_handle_s *http) { return http->udata; }

/* perform the pause task outside of the connection's lock */
static void http_pause_wrapper(void *h_, void *task_) {
  void (*task)(void *h) = (void (*)(void *h))((uintptr_t)task_);
  task(h_);
}

/* perform the resume task within of the connection's lock */
static void http_resume_wrapper(intptr_t uuid, fio_protocol_s *p_, void *arg) {
  http_fio_protocol_s *p = (http_fio_protocol_s *)p_;
  http_pause_handle_s *http = arg;
  http_s *h = http->h;
  h->udata = http->udata;
  http_vtable_s *vtbl = (http_vtable_s *)h->private_data.vtbl;
  if (http->task) http->task(h);
  vtbl->http_on_resume(h, p);
  fio_free(http);
  (void)uuid;
}

/* perform the resume task fallback */
static void http_resume_fallback_wrapper(intptr_t uuid, void *arg) {
  http_pause_handle_s *http = arg;
  if (http->fallback) http->fallback(http->udata);
  fio_free(http);
  (void)uuid;
}

/**
 * Defers the request / response handling for later.
 */
void http_pause(http_s *h, void (*task)(http_pause_handle_s *http)) {
  if (HTTP_INVALID_HANDLE(h)) {
    return;
  }
  http_fio_protocol_s *p = (http_fio_protocol_s *)h->private_data.flag;
  http_vtable_s *vtbl = (http_vtable_s *)h->private_data.vtbl;
  http_pause_handle_s *http = fio_malloc(sizeof(*http));
  *http = (http_pause_handle_s){
      .uuid = p->uuid,
      .h = h,
      .udata = h->udata,
  };
  vtbl->http_on_pause(h, p);
  fio_defer(http_pause_wrapper, http, (void *)((uintptr_t)task));
}

/**
 * Defers the request / response handling for later.
 */
void http_resume(http_pause_handle_s *http, void (*task)(http_s *h),
                 void (*fallback)(void *udata)) {
  if (!http) return;
  http->task = task;
  http->fallback = fallback;
  fio_defer_io_task(http->uuid, .udata = http, .type = FIO_PR_LOCK_TASK,
                    .task = http_resume_wrapper,
                    .fallback = http_resume_fallback_wrapper);
}

/* *****************************************************************************
Setting the default settings and allocating a persistent copy
***************************************************************************** */

static void http_on_request_fallback(http_s *h) { http_send_error(h, 404); }
static void http_on_upgrade_fallback(http_s *h, char *p, size_t i) {
  http_send_error(h, 400);
  (void)p;
  (void)i;
}
static void http_on_response_fallback(http_s *h) { http_send_error(h, 400); }

static http_settings_s *http_settings_new(http_settings_s arg_settings) {
  /* TODO: improve locality by unifying malloc to a single call */
  if (!arg_settings.on_request)
    arg_settings.on_request = http_on_request_fallback;
  if (!arg_settings.on_response)
    arg_settings.on_response = http_on_response_fallback;
  if (!arg_settings.on_upgrade)
    arg_settings.on_upgrade = http_on_upgrade_fallback;

  if (!arg_settings.max_body_size)
    arg_settings.max_body_size = HTTP_DEFAULT_BODY_LIMIT;
  if (!arg_settings.timeout) arg_settings.timeout = 40;
  if (!arg_settings.ws_max_msg_size)
    arg_settings.ws_max_msg_size = 262144; /** defaults to ~250KB */
  if (!arg_settings.ws_timeout)
    arg_settings.ws_timeout = 40; /* defaults to 40 seconds */
  if (!arg_settings.max_header_size)
    arg_settings.max_header_size = 32 * 1024; /* defaults to 32Kib seconds */
  if (arg_settings.max_clients <= 0 ||
      (size_t)(arg_settings.max_clients + HTTP_BUSY_UNLESS_HAS_FDS) >
          fio_capa()) {
    arg_settings.max_clients = fio_capa();
    if ((ssize_t)arg_settings.max_clients - HTTP_BUSY_UNLESS_HAS_FDS > 0)
      arg_settings.max_clients -= HTTP_BUSY_UNLESS_HAS_FDS;
  }

  http_settings_s *settings = fio_malloc(sizeof(*settings) + sizeof(void *));
  *settings = arg_settings;
  /* Baseline hold; see the field's doc comment in http.h. Released via
   * http_on_finish (non-TLS) or _http_settings_on_tls_cleanup (TLS). */
  settings->reserved1 = 1;

  if (settings->public_folder) {
    settings->public_folder_length = strlen(settings->public_folder);
    if (settings->public_folder[0] == '~' &&
        settings->public_folder[1] == '/' && getenv("HOME")) {
      char *home = getenv("HOME");
      size_t home_len = strlen(home);
      char *tmp = fio_malloc(settings->public_folder_length + home_len + 1);
      memcpy(tmp, home, home_len);
      if (home[home_len - 1] == '/') --home_len;
      memcpy(tmp + home_len, settings->public_folder + 1,
             settings->public_folder_length);  // copy also the NULL
      settings->public_folder = tmp;
      settings->public_folder_length = strlen(settings->public_folder);
    } else {
      settings->public_folder = fio_malloc(settings->public_folder_length + 1);
      memcpy((void *)settings->public_folder, arg_settings.public_folder,
             settings->public_folder_length);
      ((uint8_t *)settings->public_folder)[settings->public_folder_length] = 0;
    }
  }
  return settings;
}

static void http_settings_free(http_settings_s *s) {
  fio_free((void *)s->public_folder);
  fio_free(s);
}

/* Releases one hold on `settings` (see the reserved1 field's doc comment in
 * http.h) and frees it once the count reaches zero. Called once per
 * accepted-connection teardown (http1_destroy), and once to release each
 * listener's own baseline hold (http_on_finish for non-TLS, or
 * _http_settings_on_tls_cleanup for TLS). Declared in http_internal.h so
 * http1.c can call it too. */
void http_settings_release(http_settings_s *settings) {
  if (fio_atomic_sub(&settings->reserved1, 1) == 0)
    http_settings_free(settings);
}
/* *****************************************************************************
Listening to HTTP connections
***************************************************************************** */

static uint8_t fio_http_at_capa = 0;

static void http_on_server_protocol_http1(intptr_t uuid, void *set,
                                          void *ignr_) {
  fio_timeout_set(uuid, ((http_settings_s *)set)->timeout);
  if (fio_uuid2fd(uuid) >= ((http_settings_s *)set)->max_clients) {
    if (!fio_http_at_capa) FIO_LOG_WARNING("HTTP server at capacity");
    fio_http_at_capa = 1;
    http_send_error2(uuid, 503, set);
    fio_close(uuid);
    return;
  }
  fio_http_at_capa = 0;
  fio_protocol_s *pr = http1_new(uuid, set, NULL, 0);
  if (!pr) fio_close(uuid);
  (void)ignr_;
}

static void http_on_open(intptr_t uuid, void *set) {
  http_on_server_protocol_http1(uuid, set, NULL);
}

/*
 * fio_tls's ALPN dispatch (fio_tls_openssl.c: alpn_select/alpn_select___task)
 * defers the actual on_selected callback (here, http_on_server_protocol_http1)
 * via fio_defer, since it runs from within a TLS handshake r/w-hook context
 * where re-entering facio is unsafe. That deferred task reads this listener's
 * http_settings_s via its udata_connection argument -- before http1_new has
 * even run for that connection, so before it can hold its own reference (see
 * the reserved1 field's doc comment in http.h). If the listener closes while
 * such a task is still queued for an in-flight connection, releasing
 * `settings`' baseline hold synchronously at listener-close time (as
 * http_on_finish does for the non-TLS case) could free it before that queued
 * task runs.
 *
 * For TLS listeners, http_listen (below) registers this as the "http/1.1"
 * ALPN entry's on_cleanup hook instead of releasing the baseline hold in
 * http_on_finish. on_cleanup fires exactly when fio_tls_destroy releases the
 * LAST reference to settings->tls -- which fio_tls_attach2uuid/fio_tls_cleanup
 * already bump and drop correctly for every accepted connection (including
 * ones still only pending ALPN dispatch), plus the listener's own reference.
 * That covers the pre-http1_new window; releasing the baseline hold here
 * (rather than freeing unconditionally) additionally still requires every
 * connection that DID reach http1_new, and any worker thread still reading
 * its body, to have released its own hold first -- see http_settings_release.
 *
 * Note: on_cleanup also fires if a second, distinct fio_tls_s object's ALPN
 * table ever registers "http/1.1" again after this one's last connection
 * closes coincidentally -- not applicable here, since http_listen always
 * creates a fresh `tls` object per call (see src/chttpserver.c), never
 * shares one across two listeners.
 */
static void _http_settings_on_tls_cleanup(void *set) {
  http_settings_release((http_settings_s *)set);
}

static void http_on_finish(intptr_t uuid, void *set) {
  http_settings_s *settings = set;

  if (settings->on_finish) settings->on_finish(settings);

  if (!settings->tls) {
    /* No TLS/ALPN involved, so there is no pre-http1_new dispatch window to
     * worry about: release the listener's own baseline hold. Any connection
     * that reached http1_new still holds its own reference (released in
     * http1_destroy once that connection -- and any worker thread still
     * reading its body -- is fully done with it), so this does not free
     * `settings` out from under them. */
    http_settings_release(settings);
  }
  /* else: freed via _http_settings_on_tls_cleanup, see comment above. */
  (void)uuid;
}

/**
 * Listens to HTTP connections at the specified `port`.
 *
 * Leave as NULL to ignore IP binding.
 *
 * Returns -1 on error and 0 on success.
 */
#undef http_listen
intptr_t http_listen(const char *port, const char *binding,
                     struct http_settings_s arg_settings) {
  if (arg_settings.on_request == NULL) {
    FIO_LOG_ERROR(
        "http_listen requires the .on_request parameter "
        "to be set\n");
    kill(0, SIGINT);
    exit(11);
  }

  http_settings_s *settings = http_settings_new(arg_settings);
  settings->is_client = 0;
  if (settings->tls) {
    fio_tls_alpn_add(settings->tls, "http/1.1", http_on_server_protocol_http1,
                     settings, _http_settings_on_tls_cleanup);
  }

  return fio_listen(.port = port, .address = binding, .tls = arg_settings.tls,
                    .on_finish = http_on_finish, .on_open = http_on_open,
                    .udata = settings);
}
/** Listens to HTTP connections at the specified `port` and `binding`. */
#define http_listen(port, binding, ...) \
  http_listen((port), (binding), (struct http_settings_s)(__VA_ARGS__))

/**
 * Returns the settings used to setup the connection.
 *
 * Returns NULL on error (i.e., connection was lost).
 */
struct http_settings_s *http_settings(http_s *r) {
  return ((http_fio_protocol_s *)r->private_data.flag)->settings;
}

/* *****************************************************************************
HTTP Helper functions that could be used globally
***************************************************************************** */

void http_write_log(http_s *h) {
  FIOBJ l = fiobj_str_buf(128);

  intptr_t bytes_sent = fiobj_obj2num(fiobj_hash_get2(
      h->private_data.out_headers, fiobj_obj2hash(HTTP_HEADER_CONTENT_LENGTH)));

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &end);
  start = h->received_at;

  {
    // TODO Guess IP address from headers (forwarded) where possible
    fio_str_info_s peer = fio_peer_addr(http2protocol(h)->uuid);
    fiobj_str_write(l, peer.data, peer.len);
  }
  fio_str_info_s buff = fiobj_obj2cstr(l);

  if (buff.len == 0) {
    memcpy(buff.data, "[unknown]", 9);
    buff.len = 9;
  }
  memcpy(buff.data + buff.len, " - - [", 6);
  buff.len += 6;
  fiobj_str_resize(l, buff.len);
  {
    FIOBJ date;
    fio_lock(&date_lock);
    date = fiobj_dup(current_date);
    fio_unlock(&date_lock);
    fiobj_str_join(l, current_date);
    fiobj_free(date);
  }
  fiobj_str_write(l, "] \"", 3);
  fiobj_str_join(l, h->method);
  fiobj_str_write(l, " ", 1);
  fiobj_str_join(l, h->path);
  fiobj_str_write(l, " ", 1);
  fiobj_str_join(l, h->version);
  fiobj_str_write(l, "\" ", 2);
  if (bytes_sent > 0) {
    fiobj_str_write_i(l, h->status);
    fiobj_str_write(l, " ", 1);
    fiobj_str_write_i(l, bytes_sent);
    fiobj_str_write(l, "b ", 2);
  } else {
    fiobj_str_join(l, fiobj_num_tmp(h->status));
    fiobj_str_write(l, " -- ", 4);
  }

  bytes_sent = ((end.tv_sec - start.tv_sec) * 1000) +
               ((end.tv_nsec - start.tv_nsec) / 1000000);
  fiobj_str_write_i(l, bytes_sent);
  fiobj_str_write(l, "ms\r\n", 4);

  buff = fiobj_obj2cstr(l);
  fwrite(buff.data, 1, buff.len, stderr);
  fiobj_free(l);
}

/**
A faster (yet less localized) alternative to `gmtime_r`.

See the libc `gmtime_r` documentation for details.

Falls back to `gmtime_r` for dates before epoch.
*/
struct tm *http_gmtime(time_t timer, struct tm *tm) {
  ssize_t a, b;
#if HAVE_TM_TM_ZONE || defined(BSD)
  *tm = (struct tm){
      .tm_isdst = 0,
      .tm_zone = (char *)"UTC",
  };
#else
  *tm = (struct tm){
      .tm_isdst = 0,
  };
#endif

  // convert seconds from epoch to days from epoch + extract data
  if (timer >= 0) {
    // for seconds up to weekdays, we reduce the reminder every step.
    a = (ssize_t)timer;
    b = a / 60;  // b == time in minutes
    tm->tm_sec = a - (b * 60);
    a = b / 60;  // b == time in hours
    tm->tm_min = b - (a * 60);
    b = a / 24;  // b == time in days since epoch
    tm->tm_hour = a - (b * 24);
    // b == number of days since epoch
    // day of epoch was a thursday. Add + 4 so sunday == 0...
    tm->tm_wday = (b + 4) % 7;
  } else {
    // for seconds up to weekdays, we reduce the reminder every step.
    a = (ssize_t)timer;
    b = a / 60;  // b == time in minutes
    if (b * 60 != a) {
      /* seconds passed */
      tm->tm_sec = (a - (b * 60)) + 60;
      --b;
    } else {
      /* no seconds */
      tm->tm_sec = 0;
    }
    a = b / 60;  // b == time in hours
    if (a * 60 != b) {
      /* minutes passed */
      tm->tm_min = (b - (a * 60)) + 60;
      --a;
    } else {
      /* no minutes */
      tm->tm_min = 0;
    }
    b = a / 24;  // b == time in days since epoch?
    if (b * 24 != a) {
      /* hours passed */
      tm->tm_hour = (a - (b * 24)) + 24;
      --b;
    } else {
      /* no hours */
      tm->tm_hour = 0;
    }
    // day of epoch was a thursday. Add + 4 so sunday == 0...
    tm->tm_wday = ((b - 3) % 7);
    if (tm->tm_wday) tm->tm_wday += 7;
    /* b == days from epoch */
  }

  // at this point we can apply the algorithm described here:
  // http://howardhinnant.github.io/date_algorithms.html#civil_from_days
  // Credit to Howard Hinnant.
  {
    b += 719468L;  // adjust to March 1st, 2000 (post leap of 400 year era)
    // 146,097 = days in era (400 years)
    const size_t era = (b >= 0 ? b : b - 146096) / 146097;
    const uint32_t doe = (b - (era * 146097));  // day of era
    const uint16_t yoe =
        (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // year of era
    a = yoe;
    a += era * 400;  // a == year number, assuming year starts on March 1st...
    const uint16_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint16_t mp = (5U * doy + 2) / 153;
    const uint16_t d = doy - (153U * mp + 2) / 5 + 1;
    const uint8_t m = mp + (mp < 10 ? 2 : -10);
    a += (m <= 1);
    tm->tm_year = a - 1900;  // tm_year == years since 1900
    tm->tm_mon = m;
    tm->tm_mday = d;
    const uint8_t is_leap = (a % 4 == 0 && (a % 100 != 0 || a % 400 == 0));
    tm->tm_yday = (doy + (is_leap) + 28 + 31) % (365 + is_leap);
  }

  return tm;
}

static const char *DAY_NAMES[] = {"Sun", "Mon", "Tue", "Wed",
                                  "Thu", "Fri", "Sat"};
static const char *MONTH_NAMES[] = {"Jan ", "Feb ", "Mar ", "Apr ",
                                    "May ", "Jun ", "Jul ", "Aug ",
                                    "Sep ", "Oct ", "Nov ", "Dec "};
static const char *GMT_STR = "GMT";

size_t http_date2rfc7231(char *target, struct tm *tmbuf) {
  /* note: day of month is always 2 digits */
  char *pos = target;
  uint16_t tmp;
  pos[0] = DAY_NAMES[tmbuf->tm_wday][0];
  pos[1] = DAY_NAMES[tmbuf->tm_wday][1];
  pos[2] = DAY_NAMES[tmbuf->tm_wday][2];
  pos[3] = ',';
  pos[4] = ' ';
  pos += 5;
  tmp = tmbuf->tm_mday / 10;
  pos[0] = '0' + tmp;
  pos[1] = '0' + (tmbuf->tm_mday - (tmp * 10));
  pos += 2;
  *(pos++) = ' ';
  pos[0] = MONTH_NAMES[tmbuf->tm_mon][0];
  pos[1] = MONTH_NAMES[tmbuf->tm_mon][1];
  pos[2] = MONTH_NAMES[tmbuf->tm_mon][2];
  pos[3] = ' ';
  pos += 4;
  // write year.
  pos += fio_ltoa(pos, tmbuf->tm_year + 1900, 10);
  *(pos++) = ' ';
  tmp = tmbuf->tm_hour / 10;
  pos[0] = '0' + tmp;
  pos[1] = '0' + (tmbuf->tm_hour - (tmp * 10));
  pos[2] = ':';
  tmp = tmbuf->tm_min / 10;
  pos[3] = '0' + tmp;
  pos[4] = '0' + (tmbuf->tm_min - (tmp * 10));
  pos[5] = ':';
  tmp = tmbuf->tm_sec / 10;
  pos[6] = '0' + tmp;
  pos[7] = '0' + (tmbuf->tm_sec - (tmp * 10));
  pos += 8;
  pos[0] = ' ';
  pos[1] = GMT_STR[0];
  pos[2] = GMT_STR[1];
  pos[3] = GMT_STR[2];
  pos[4] = 0;
  pos += 4;
  return pos - target;
}

size_t http_date2str(char *target, struct tm *tmbuf);
/**
 * Prints Unix time to a HTTP time formatted string.
 *
 * This variation implements cached results for faster processing, at the
 * price of a less accurate string.
 */
size_t http_time2str(char *target, const time_t t) {
  /* pre-print time every 1 or 2 seconds or so. */
  static __thread time_t cached_tick;
  static __thread char cached_httpdate[48];
  static __thread size_t cached_len;
  time_t last_tick = fio_last_tick().tv_sec;
  if ((t | 7) < last_tick) {
    /* this is a custom time, not "now", pass through */
    struct tm tm;
    http_gmtime(t, &tm);
    return http_date2str(target, &tm);
  }
  if (last_tick > cached_tick) {
    struct tm tm;
    cached_tick = last_tick; /* refresh every second */
    http_gmtime(last_tick, &tm);
    cached_len = http_date2str(cached_httpdate, &tm);
  }
  memcpy(target, cached_httpdate, cached_len);
  return cached_len;
}

static inline int hex2byte(uint8_t *dest, const uint8_t *source) {
  if (source[0] >= '0' && source[0] <= '9')
    *dest = (source[0] - '0');
  else if ((source[0] >= 'a' && source[0] <= 'f') ||
           (source[0] >= 'A' && source[0] <= 'F'))
    *dest = (source[0] | 32) - 87;
  else
    return -1;
  *dest <<= 4;
  if (source[1] >= '0' && source[1] <= '9')
    *dest |= (source[1] - '0');
  else if ((source[1] >= 'a' && source[1] <= 'f') ||
           (source[1] >= 'A' && source[1] <= 'F'))
    *dest |= (source[1] | 32) - 87;
  else
    return -1;
  return 0;
}

ssize_t http_decode_url_unsafe(char *dest, const char *url_data) {
  char *pos = dest;
  while (*url_data) {
    if (*url_data == '+') {
      // decode space
      *(pos++) = ' ';
      ++url_data;
    } else if (*url_data == '%') {
      // decode hex value
      // this is a percent encoded value.
      if (hex2byte((uint8_t *)pos, (uint8_t *)&url_data[1])) return -1;
      pos++;
      url_data += 3;
    } else
      *(pos++) = *(url_data++);
  }
  *pos = 0;
  return pos - dest;
}

ssize_t http_decode_path_unsafe(char *dest, const char *url_data) {
  char *pos = dest;
  while (*url_data) {
    if (*url_data == '%') {
      // decode hex value
      // this is a percent encoded value.
      if (hex2byte((uint8_t *)pos, (uint8_t *)&url_data[1])) return -1;
      pos++;
      url_data += 3;
    } else
      *(pos++) = *(url_data++);
  }
  *pos = 0;
  return pos - dest;
}

/* *****************************************************************************
Lookup Tables / functions
***************************************************************************** */

#define FIO_FORCE_MALLOC_TMP 1 /* use malloc for the mime registry */
#define FIO_SET_NAME fio_mime_set
#define FIO_SET_OBJ_TYPE FIOBJ
#define FIO_SET_OBJ_COMPARE(o1, o2) (1)
#define FIO_SET_OBJ_COPY(dest, o) (dest) = fiobj_dup((o))
#define FIO_SET_OBJ_DESTROY(o) fiobj_free((o))

#include <fio.h>

static fio_mime_set_s fio_http_mime_types = FIO_SET_INIT;

#define LONGEST_FILE_EXTENSION_LENGTH 15

/** Registers a Mime-Type to be associated with the file extension. */
void http_mimetype_register(char *file_ext, size_t file_ext_len,
                            FIOBJ mime_type_str) {
  uintptr_t hash = FIO_HASH_FN(file_ext, file_ext_len, 0, 0);
  if (mime_type_str == FIOBJ_INVALID) {
    fio_mime_set_remove(&fio_http_mime_types, hash, FIOBJ_INVALID, NULL);
  } else {
    FIOBJ old = FIOBJ_INVALID;
    fio_mime_set_overwrite(&fio_http_mime_types, hash, mime_type_str, &old);
    if (old != FIOBJ_INVALID) {
      FIO_LOG_WARNING("mime-type collision: %.*s was %s, now %s",
                      (int)file_ext_len, file_ext, fiobj_obj2cstr(old).data,
                      fiobj_obj2cstr(mime_type_str).data);
      fiobj_free(old);
    }
    fiobj_free(mime_type_str); /* move ownership to the registry */
  }
}
/**
 * Finds the mime-type associated with the file extension.
 *  Remember to call `fiobj_free`.
 */
FIOBJ http_mimetype_find(char *file_ext, size_t file_ext_len) {
  uintptr_t hash = FIO_HASH_FN(file_ext, file_ext_len, 0, 0);
  return fiobj_dup(
      fio_mime_set_find(&fio_http_mime_types, hash, FIOBJ_INVALID));
}

void http_mimetype_stats(void) {
  FIO_LOG_DEBUG("HTTP MIME hash storage count/capa: %zu / %zu",
                fio_mime_set_count(&fio_http_mime_types),
                fio_mime_set_capa(&fio_http_mime_types));
}

/** Clears the Mime-Type registry (it will be empty after this call). */
void http_mimetype_clear(void) {
  fio_mime_set_free(&fio_http_mime_types);
  fiobj_free(current_date);
  current_date = FIOBJ_INVALID;
  last_date_added = 0;
}

// clang-format off
#define HTTP_SET_STATUS_STR(status, str) [status-100] = { .data = (char *)(str), .len = (sizeof(str) - 1) }
// clang-format on

/** Returns the status as a C string struct */
fio_str_info_s http_status2str(uintptr_t status) {
  static const fio_str_info_s status2str[] = {
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
