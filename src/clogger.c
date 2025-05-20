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

#define _GNU_SOURCE

#include <chashmap.h>
#include <clogger.h>
#include <common.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#if defined(__GLIBC__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <execinfo.h>
#define CLOG_HAS_BACKTRACE 1
#else
#define CLOG_HAS_BACKTRACE 0
#endif

/* ========================================================================== */
/*                         CONSTANTS                                          */
/* ========================================================================== */

#define CLOG_BUF_INITIAL 4096U
#define CLOG_BUF_MAX (16U * 1024U * 1024U) /* 16 MiB hard cap */
#define CLOG_BACKTRACE_DEPTH 64
/* Rotation suffix format: ".YYYYMMDDHHMMSS" = 15 chars */
#define CLOG_ROTATION_FMT ".%Y%m%d%H%M%S"
#define CLOG_ROTATION_FMT_LEN 15
/* Extra headroom for collision suffix "_0001"-"_9999" */
#define CLOG_ROTATION_EXTRA 8

static const char *const _LEVEL_STR[] = {"TRACE", "DEBUG", "INFO",  "WARN",
                                         "ERROR", "ALERT", "FATAL", "OFF"};

/* RFC 5424 severity codes indexed by clog_level_t. */
static const int _SYSLOG_SEVERITY[] = {
    7, /* TRACE -> debug         */
    7, /* DEBUG -> debug         */
    6, /* INFO  -> informational */
    4, /* WARN  -> warning       */
    3, /* ERROR -> error         */
    1, /* ALERT -> alert         */
    0, /* FATAL -> emergency     */
    7, /* OFF   -- sentinel; CLOG_OFF must never be used as a message level */
};

/* ========================================================================== */
/*                         INTERNAL STRUCTURES                                */
/* ========================================================================== */

/*
 * Shared backing store: fd, rotation state, and mutex.
 * Reference-counted so the root logger and all derived loggers share one
 * instance.  The fd is closed (if owned) when ref_count reaches zero.
 */
typedef struct clog_shared {
  int fd;
  bool owns_fd;
  char *file_path; /* NULL for fd-based loggers */

  bool rotation_enabled;
  clog_rotation_cfg_t rotation;
  off_t bytes_written;
  time_t last_rotation;

  mutex_t mutex;
  int ref_count;
  clog_format_t format;
  clog_syslog_facility_t syslog_facility; /* PRI facility for CLOG_FMT_SYSLOG */
  char syslog_hostname[256]; /* hostname cached at creation       */
  char syslog_appname[49];   /* APP-NAME cached at creation        */
  ccol_memmgmt_procs_t
      *m_procs; /* heap-allocated copy; NULL = default allocator */
} clog_shared_t;

typedef struct {
  char *data;
  size_t len;
  size_t cap;
  const ccol_memmgmt_procs_t *m_procs; /* borrowed from clog_shared_t */
} clog_buf_t;

struct clogger {
  clog_shared_t *shared;  /* shared output backing store              */
  clog_level_t min_level; /* per-logger level filter                  */
  chmap fields;           /* chmap(char* -> char*) -- per-logger fields */
  clog_buf_t buf;         /* per-logger reusable write buffer          */
};

/* ========================================================================== */
/*                         BUFFER HELPERS                                     */
/* ========================================================================== */

static int _buf_init(clog_buf_t *b, const ccol_memmgmt_procs_t *m_procs) {
  b->m_procs = m_procs;
  b->data = _mem_alloc(m_procs, CLOG_BUF_INITIAL);
  if (!b->data) return -1;
  b->data[0] = '\0';
  b->len = 0;
  b->cap = CLOG_BUF_INITIAL;
  return 0;
}

static void _buf_reset(clog_buf_t *b) { b->len = 0; }

static void _buf_free(clog_buf_t *b) {
  _mem_free(b->m_procs, b->data);
  b->data = NULL;
  b->len = b->cap = 0;
}

/* Ensure at least `need` free bytes are available. */
static int _buf_ensure(clog_buf_t *b, size_t need) {
  if (b->cap - b->len >= need) return 0;
  size_t new_cap = b->cap ? b->cap : CLOG_BUF_INITIAL;
  while (new_cap - b->len < need) {
    if (new_cap >= CLOG_BUF_MAX) return -1;
    new_cap *= 2;
    if (new_cap > CLOG_BUF_MAX) new_cap = CLOG_BUF_MAX;
  }
  char *p = _mem_realloc(b->m_procs, b->data, new_cap);
  if (!p) return -1;
  b->data = p;
  b->cap = new_cap;
  return 0;
}

static int _buf_append(clog_buf_t *b, const char *s, size_t n) {
  if (_buf_ensure(b, n + 1) != 0) return -1;
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
  return 0;
}

static int __attribute__((format(printf, 2, 3))) _buf_appendf(clog_buf_t *b,
                                                              const char *fmt,
                                                              ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);

  size_t avail = b->cap - b->len;
  int n = vsnprintf(b->data + b->len, avail, fmt, ap);
  va_end(ap);

  if (n < 0) {
    va_end(ap2);
    return -1;
  }
  if ((size_t)n >= avail) {
    if (_buf_ensure(b, (size_t)n + 1) != 0) {
      va_end(ap2);
      return -1;
    }
    avail = b->cap - b->len;
    n = vsnprintf(b->data + b->len, avail, fmt, ap2);
    if (n < 0 || (size_t)n >= avail) {
      va_end(ap2);
      return -1;
    }
  }
  va_end(ap2);
  b->len += (size_t)n;
  return 0;
}

/*
 * Append a logfmt-safe value.  Values that contain spaces, '=', '"', '\\',
 * or control characters are double-quoted with backslash escaping.
 * Empty strings are emitted as "".
 */
static int _buf_append_lv(clog_buf_t *b, const char *s) {
  if (!s) return _buf_append(b, "null", 4);

  bool quote = (s[0] == '\0');
  if (!quote) {
    for (const char *p = s; *p && !quote; p++) {
      unsigned char c = (unsigned char)*p;
      if (c < 0x20 || c == 0x7f || *p == ' ' || *p == '=' || *p == '"' ||
          *p == '\\')
        quote = true;
    }
  }

  if (!quote) return _buf_append(b, s, strlen(s));

  if (_buf_append(b, "\"", 1) != 0) return -1;

  const char *run = s;
  for (const char *p = s;; p++) {
    unsigned char c = (unsigned char)*p;

    if (*p == '\0') {
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      break;
    }
    if (*p == '"' || *p == '\\') {
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      char esc[2] = {'\\', *p};
      if (_buf_append(b, esc, 2) != 0) return -1;
      run = p + 1;
    } else if (c < 0x20 || c == 0x7f) {
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      char esc[5];
      int esc_len;
      if (c == '\n') {
        esc[0] = '\\';
        esc[1] = 'n';
        esc_len = 2;
      } else if (c == '\r') {
        esc[0] = '\\';
        esc[1] = 'r';
        esc_len = 2;
      } else if (c == '\t') {
        esc[0] = '\\';
        esc[1] = 't';
        esc_len = 2;
      } else {
        snprintf(esc, sizeof esc, "\\x%02x", c);
        esc_len = 4;
      }
      if (_buf_append(b, esc, (size_t)esc_len) != 0) return -1;
      run = p + 1;
    }
  }

  return _buf_append(b, "\"", 1);
}

/* ========================================================================== */
/*                         JSON HELPERS                                       */
/* ========================================================================== */

/*
 * Append the JSON-escaped content of s without surrounding quotes.
 * Escaping: '"' -> '\"', '\' -> '\\', '\n'->'\n', '\r'->'\r', '\t'->'\t',
 * other control chars -> '\uXXXX'.  s must not be NULL.
 */
static int _buf_append_json_content(clog_buf_t *b, const char *s) {
  const char *run = s;
  for (const char *p = s;; p++) {
    unsigned char c = (unsigned char)*p;
    if (*p == '\0') {
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      break;
    }
    if (*p == '"' || *p == '\\') {
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      char esc[2] = {'\\', *p};
      if (_buf_append(b, esc, 2) != 0) return -1;
      run = p + 1;
    } else if (c < 0x20 || c == 0x7f) {
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      char esc[7];
      int esc_len;
      if (c == '\n') {
        esc[0] = '\\';
        esc[1] = 'n';
        esc_len = 2;
      } else if (c == '\r') {
        esc[0] = '\\';
        esc[1] = 'r';
        esc_len = 2;
      } else if (c == '\t') {
        esc[0] = '\\';
        esc[1] = 't';
        esc_len = 2;
      } else {
        esc_len = snprintf(esc, sizeof esc, "\\u%04x", c);
      }
      if (_buf_append(b, esc, (size_t)esc_len) != 0) return -1;
      run = p + 1;
    }
  }
  return 0;
}

/*
 * Append a comma-prefixed JSON key-value pair: ,"key":"value"
 * key must not be NULL.  val NULL -> ,"key":null
 */
static int _buf_append_json_kv(clog_buf_t *b, const char *key,
                               const char *val) {
  if (_buf_append(b, ",\"", 2) != 0) return -1;
  if (_buf_append_json_content(b, key) != 0) return -1;
  if (_buf_append(b, "\":", 2) != 0) return -1;
  if (!val) return _buf_append(b, "null", 4);
  if (_buf_append(b, "\"", 1) != 0) return -1;
  if (_buf_append_json_content(b, val) != 0) return -1;
  return _buf_append(b, "\"", 1);
}

/* ========================================================================== */
/*                         SYSLOG HELPERS                                     */
/* ========================================================================== */

/*
 * Append an RFC 5424 SD-PARAM-VALUE: any UTF-8 text with '"', '\', and ']'
 * escaped as '\"', '\\', '\]'.  s must not be NULL.
 */
static int _buf_append_sd_value(clog_buf_t *b, const char *s) {
  const char *run = s;
  for (const char *p = s;; p++) {
    if (*p == '\0') {
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      break;
    }
    if (*p == '"' || *p == '\\' || *p == ']') {
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      char esc[2] = {'\\', *p};
      if (_buf_append(b, esc, 2) != 0) return -1;
      run = p + 1;
    }
  }
  return 0;
}

/* Return the process name for RFC 5424 APP-NAME, or "-" if unavailable. */
static const char *_syslog_appname(void) {
#if defined(__GLIBC__)
  if (program_invocation_short_name && program_invocation_short_name[0])
    return program_invocation_short_name;
#elif defined(__APPLE__) || defined(__FreeBSD__)
  const char *p = getprogname();
  if (p && p[0]) return p;
#endif
  return "-";
}

/* Return the basename of the executable, or "unknown" if unavailable. */
static const char *_get_progname(void) {
#if defined(__GLIBC__)
  if (program_invocation_short_name && program_invocation_short_name[0])
    return program_invocation_short_name;
#elif defined(__APPLE__) || defined(__FreeBSD__)
  const char *p = getprogname();
  if (p && p[0]) return p;
#endif
  return "unknown";
}

/* Return the OS-level thread ID for the calling thread. */
static pid_t _get_tid(void) {
#if defined(__linux__)
  return (pid_t)syscall(SYS_gettid);
#elif defined(__APPLE__)
  uint64_t tid64 = 0;
  pthread_threadid_np(NULL, &tid64);
  return (pid_t)tid64;
#else
  return (pid_t)(uintptr_t)pthread_self();
#endif
}

/* Fill buf with the name of the calling thread (at most bufsz-1 chars). */
static void _get_thread_name(char *buf, size_t bufsz) {
#if defined(__linux__)
  char name[16]; /* prctl writes at most 16 bytes including null */
  if (prctl(PR_GET_NAME, name) == 0 && name[0]) {
    snprintf(buf, bufsz, "%s", name);
    return;
  }
#elif defined(__APPLE__) || defined(__FreeBSD__)
  if (pthread_getname_np(pthread_self(), buf, bufsz) == 0 && buf[0]) return;
#endif
  snprintf(buf, bufsz, "unknown");
}

/* ========================================================================== */
/*                         I/O HELPER                                         */
/* ========================================================================== */

/* write() loop that retries on EINTR and partial writes. */
static void _write_all(int fd, const char *data, size_t len) {
  while (len > 0) {
    ssize_t w = write(fd, data, len);
    if (w < 0) {
      if (errno == EINTR) continue;
      break; /* silently drop on unrecoverable write error */
    }
    if (w == 0) break; /* no progress (quota / fd limit); avoid spinning */
    data += (size_t)w;
    len -= (size_t)w;
  }
}

/* ========================================================================== */
/*                         TIMESTAMP                                          */
/* ========================================================================== */

static int _buf_append_ts(clog_buf_t *b) {
  struct timeval tv = {0, 0};
  gettimeofday(&tv, NULL);
  struct tm tm;
  gmtime_r(&tv.tv_sec, &tm);
  return _buf_appendf(b, "%04d-%02d-%02dT%02d:%02d:%02d.%06ldZ",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                      tm.tm_min, tm.tm_sec, (long)tv.tv_usec);
}

/* ========================================================================== */
/*                         PATH UTILITIES                                     */
/* ========================================================================== */

/* Fill dirbuf with the directory component of path (no heap allocation). */
static void _path_dir(const char *path, char *dirbuf, size_t bufsz) {
  const char *slash = strrchr(path, '/');
  if (!slash) {
    strncpy(dirbuf, ".", bufsz - 1);
    dirbuf[bufsz - 1] = '\0';
    return;
  }
  size_t len = (size_t)(slash - path);
  if (len == 0) len = 1; /* root "/" */
  if (len >= bufsz) len = bufsz - 1;
  memcpy(dirbuf, path, len);
  dirbuf[len] = '\0';
}

/* Return pointer into path just after the last '/'. */
static const char *_path_base(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

/* ========================================================================== */
/*                         GZIP COMPRESSION (via zlib)                        */
/* ========================================================================== */

#define _GZ_BUF_SIZE 65536U

/*
 * Gzip-compress the file at src into dst (src + ".gz") using zlib.
 * On success: dst is a valid gzip file, src is unlinked; returns 0.
 * On failure: dst is removed if partially written, src is untouched; returns
 * -1.
 */
static int _gzip_compress_file(const char *src, const char *dst) {
  FILE *in = NULL;
  gzFile out = NULL;
  int rc = -1;

  in = fopen(src, "rb");
  if (!in) goto done;

  out = gzopen(dst, "wb");
  if (!out) goto done;

  unsigned char buf[_GZ_BUF_SIZE];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
    if (gzwrite(out, buf, (unsigned)n) != (int)n) goto done;
  }
  if (ferror(in)) goto done;

  if (gzclose(out) != Z_OK) {
    out = NULL;
    goto done;
  }
  out = NULL;
  rc = 0;

done:
  if (in) fclose(in);
  if (out) gzclose(out);
  if (rc != 0) unlink(dst); /* remove truncated output */
  if (rc == 0) unlink(src); /* remove uncompressed original */
  return rc;
}
/* ========================================================================== */
/*                         LOG ROTATION                                       */
/* ========================================================================== */

static int _cmp_strptr(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Delete the oldest rotated files beyond max_keep.  Called with mutex held. */
static void _prune_rotated(const char *file_path, int max_keep,
                           const ccol_memmgmt_procs_t *m_procs) {
  if (max_keep <= 0) return;

  char dir[PATH_MAX];
  _path_dir(file_path, dir, sizeof dir);

  const char *base = _path_base(file_path);
  size_t base_len = strlen(base);

  DIR *d = opendir(dir);
  if (!d) return;

  char **matches = NULL;
  int mc = 0, cap = 0;
  struct dirent *e;

  while ((e = readdir(d)) != NULL) {
    const char *n = e->d_name;

    /* Match: <base>.<YYYYMMDDHHMMSS>[_N]  (14 mandatory digit chars) */
    if (strncmp(n, base, base_len) != 0 || n[base_len] != '.') continue;
    const char *ts_start = n + base_len + 1;
    if (strlen(ts_start) < 14) continue;

    bool ts_ok = true;
    for (int i = 0; i < 14; i++) {
      if (!isdigit((unsigned char)ts_start[i])) {
        ts_ok = false;
        break;
      }
    }
    if (!ts_ok) continue;

    size_t dlen = strlen(dir);
    size_t nlen = strlen(n);
    char *full = _mem_alloc(m_procs, dlen + 1 + nlen + 1);
    if (!full) continue;
    memcpy(full, dir, dlen);
    full[dlen] = '/';
    memcpy(full + dlen + 1, n, nlen + 1);

    if (mc >= cap) {
      int nc = cap ? cap * 2 : 16;
      char **nm = _mem_realloc(m_procs, matches, (size_t)nc * sizeof *matches);
      if (!nm) {
        _mem_free(m_procs, full);
        break;
      }
      matches = nm;
      cap = nc;
    }
    matches[mc++] = full;
  }
  closedir(d);

  if (mc > 1) qsort(matches, (size_t)mc, sizeof *matches, _cmp_strptr);

  /* Delete oldest entries that exceed the quota */
  int to_del = mc - max_keep;
  for (int i = 0; i < to_del; i++) unlink(matches[i]);

  for (int i = 0; i < mc; i++) _mem_free(m_procs, matches[i]);
  _mem_free(m_procs, matches);
}

/*
 * Rotate the current log file.  Must be called with shared->mutex held.
 *
 * Steps:
 *   1. Build a timestamped destination name, handling same-second collisions.
 *   2. Close the current fd.
 *   3. Rename the current file to the destination (atomic on POSIX).
 *   4. Prune old rotated files if max_rotated_files is set.
 *   5. Open a fresh log file.
 */
static int _rotate(clog_shared_t *sh) {
  if (!sh->file_path || sh->fd < 0) return 0;

  time_t now = time(NULL);
  struct tm tm;
  gmtime_r(&now, &tm);

  size_t plen = strlen(sh->file_path);
  char rotated[PATH_MAX];

  if (plen + CLOG_ROTATION_FMT_LEN + CLOG_ROTATION_EXTRA + 1 > sizeof rotated)
    return -1;

  memcpy(rotated, sh->file_path, plen);
  size_t slen =
      strftime(rotated + plen, sizeof(rotated) - plen, CLOG_ROTATION_FMT, &tm);
  if (slen == 0) return -1;

  /* Resolve collisions: append _0001, _0002, ... until the name is free.
   * Zero-padded so alphabetical sort in _prune_rotated matches creation order.
   */
  if (access(rotated, F_OK) == 0) {
    size_t base = plen + slen;
    bool found = false;
    for (int n = 1; n < 10000; n++) {
      int w = snprintf(rotated + base, sizeof(rotated) - base, "_%04d", n);
      if (w < 0) return -1;
      if (access(rotated, F_OK) != 0) {
        found = true;
        break;
      }
    }
    if (!found) return -1;
  }

  /* Rename while the old fd is still open (POSIX allows renaming open files).
   * We only close the old fd once we have a replacement; this way a failed
   * open() leaves the logger alive -- writes continue to the rotated file.
   * ENOENT means the file was deleted externally; treat it as a clean slate
   * (O_CREAT below will create a fresh file).  Any other rename error is a
   * hard failure -- leave the logger writing to the still-open original fd. */
  if (rename(sh->file_path, rotated) != 0 && errno != ENOENT) return -1;

  if (sh->rotation.max_rotated_files > 0)
    _prune_rotated(sh->file_path, sh->rotation.max_rotated_files, sh->m_procs);

  int new_fd =
      open(sh->file_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (new_fd < 0) {
    /* Recovery: restore the original path so the still-open fd remains useful.
     */
    (void)rename(rotated, sh->file_path);
    return -1;
  }

  close(sh->fd);
  sh->fd = new_fd;
  sh->bytes_written = 0;
  sh->last_rotation = now;

  /*
   * Compress the rotated file outside the mutex so log writers are not stalled
   * during what can be a slow I/O operation.  The critical shared state (fd,
   * bytes_written, last_rotation) is already committed above; releasing the
   * mutex here is safe.
   */
  if (sh->rotation.compress_rotated) {
    char gz_path[PATH_MAX];
    size_t rlen = strlen(rotated);
    if (rlen + 3 < sizeof gz_path) {
      memcpy(gz_path, rotated, rlen);
      memcpy(gz_path + rlen, ".gz", 4); /* includes NUL */
      mutex_unlock(sh->mutex);
      _gzip_compress_file(rotated, gz_path);
      mutex_lock(sh->mutex);
    }
  }

  return 0;
}

/* ========================================================================== */
/*                         BACKTRACE                                          */
/* ========================================================================== */

/*
 * Emit backtrace frames as tab-indented continuation lines.
 * Must be called with shared->mutex held.
 */
static void __attribute__((noinline)) _emit_backtrace(struct clogger *lg) {
#if CLOG_HAS_BACKTRACE
  void *ptrs[CLOG_BACKTRACE_DEPTH];
  int depth = backtrace(ptrs, CLOG_BACKTRACE_DEPTH);
  char **syms = backtrace_symbols(ptrs, depth);
  if (!syms) return;

  /*
   * noinline guarantees a deterministic frame layout:
   * Frame 0 = _emit_backtrace
   * Frame 1 = _clog_write
   * Frame 2 = caller (first frame shown to the user)
   */
  int initial_frame = 2;
  for (int i = initial_frame; i < depth; i++) {
    _buf_reset(&lg->buf);
    _buf_appendf(&lg->buf, "\t#%d %s\n", i - initial_frame, syms[i]);
    _write_all(lg->shared->fd, lg->buf.data, lg->buf.len);
    if (lg->shared->rotation_enabled)
      lg->shared->bytes_written += (off_t)lg->buf.len;
  }

  free(syms);
#else
  (void)lg;
#endif
}

/*
 * Append backtrace frames as a JSON array: ,"bt":["#0 sym","#1 sym",...]
 * Called while the JSON object is still open (before the closing "}\n").
 * noinline ensures a predictable call-stack depth:
 *   Frame 0 = _emit_backtrace_json
 *   Frame 1 = _clog_write
 *   Frame 2 = caller (first frame shown to the user)
 */
static void __attribute__((noinline)) _emit_backtrace_json(clog_buf_t *b) {
#if CLOG_HAS_BACKTRACE
  void *ptrs[CLOG_BACKTRACE_DEPTH];
  int depth = backtrace(ptrs, CLOG_BACKTRACE_DEPTH);
  char **syms = backtrace_symbols(ptrs, depth);
  if (!syms) return;

  int initial_frame = 2;
  size_t bt_start = b->len;
  if (_buf_append(b, ",\"bt\":[", 7) != 0) {
    free(syms);
    return;
  }

  bool first = true;
  for (int i = initial_frame; i < depth; i++) {
    size_t entry_start = b->len;
    char prefix[20];
    int pl = snprintf(prefix, sizeof prefix, "#%d ", i - initial_frame);
    if ((!first && _buf_append(b, ",", 1) != 0) ||
        _buf_append(b, "\"", 1) != 0 ||
        (pl > 0 && _buf_append(b, prefix, (size_t)pl) != 0) ||
        _buf_append_json_content(b, syms[i]) != 0 ||
        _buf_append(b, "\"", 1) != 0) {
      b->len =
          entry_start; /* roll back partial entry so array closes cleanly */
      break;
    }
    first = false;
  }

  if (_buf_append(b, "]", 1) != 0)
    b->len = bt_start; /* roll back entire bt array so the JSON object closes
                          cleanly */
  free(syms);
#else
  (void)b;
#endif
}

/*
 * Emit backtrace frames as separate RFC 5424 syslog messages, one per frame.
 * Must be called with shared->mutex held.
 * noinline ensures a predictable call-stack depth:
 *   Frame 0 = _emit_backtrace_syslog
 *   Frame 1 = _clog_write
 *   Frame 2 = caller (first frame shown to the user)
 */
static void __attribute__((noinline)) _emit_backtrace_syslog(
    struct clogger *lg, clog_level_t level) {
#if CLOG_HAS_BACKTRACE
  void *ptrs[CLOG_BACKTRACE_DEPTH];
  int depth = backtrace(ptrs, CLOG_BACKTRACE_DEPTH);
  char **syms = backtrace_symbols(ptrs, depth);
  if (!syms) return;

  int pri = (int)lg->shared->syslog_facility * 8 + _SYSLOG_SEVERITY[level];
  const char *hostname =
      lg->shared->syslog_hostname[0] ? lg->shared->syslog_hostname : "-";
  const char *appname = lg->shared->syslog_appname;
  const char *msgid = _LEVEL_STR[level];

  int initial_frame = 2;
  for (int i = initial_frame; i < depth; i++) {
    _buf_reset(&lg->buf);
    _buf_appendf(&lg->buf, "<%d>1 ", pri);
    _buf_append_ts(&lg->buf);
    _buf_appendf(&lg->buf, " %s %s %d %s - \t#%d %s\n", hostname, appname,
                 (int)getpid(), msgid, i - initial_frame, syms[i]);
    _write_all(lg->shared->fd, lg->buf.data, lg->buf.len);
    if (lg->shared->rotation_enabled)
      lg->shared->bytes_written += (off_t)lg->buf.len;
  }
  free(syms);
#else
  (void)lg;
  (void)level;
#endif
}

/* ========================================================================== */
/*                         CONSTRUCTOR HELPERS                                */
/* ========================================================================== */

static chmap _fields_create(ccol_memmgmt_procs_t *m_procs) {
  char *err = NULL;
  chmap m = chmap_create_mp(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                            ccol_string, m_procs, &err);
  return m; /* NULL on failure */
}

/*
 * Free a clog_shared_t that has no live mutex and no logger children.
 * Safe to call at any point during _shared_alloc's initialization after
 * sh->m_procs has been set (which happens immediately after allocating sh).
 */
static void _shared_free_partial(clog_shared_t *sh) {
  _mem_free(sh->m_procs, sh->file_path);
  ccol_memmgmt_procs_t *mp = sh->m_procs;
  if (mp) {
    ccol_free_t free_fn = mp->free;
    free_fn(mp);
    free_fn(sh);
  } else {
    free(sh);
  }
}

static clog_shared_t *_shared_alloc(int fd, bool owns_fd, const char *file_path,
                                    ccol_memmgmt_procs_t *m_procs) {
  clog_shared_t *sh = _mem_calloc(m_procs, 1, sizeof *sh);
  if (!sh) return NULL;

  sh->fd = fd;
  sh->owns_fd = owns_fd;
  sh->ref_count = 1;

  if (m_procs) {
    sh->m_procs = (ccol_memmgmt_procs_t *)m_procs->malloc(sizeof *m_procs);
    if (!sh->m_procs) {
      m_procs->free(sh);
      return NULL;
    }
    memcpy(sh->m_procs, m_procs, sizeof *m_procs);
  }

  if (file_path) {
    size_t len = strlen(file_path) + 1;
    sh->file_path = _mem_alloc(sh->m_procs, len);
    if (!sh->file_path) {
      _shared_free_partial(sh);
      return NULL;
    }
    memcpy(sh->file_path, file_path, len);
  }

  if (mutex_init(sh->mutex) != 0) {
    _shared_free_partial(sh);
    return NULL;
  }

  sh->last_rotation = time(NULL);

  sh->syslog_facility = CLOG_SYSLOG_USER; /* calloc zeroes to KERN; override */

  /* Cache hostname for RFC 5424 HOSTNAME field; truncate at first space. */
  if (gethostname(sh->syslog_hostname, sizeof sh->syslog_hostname) != 0)
    sh->syslog_hostname[0] = '\0';
  sh->syslog_hostname[sizeof sh->syslog_hostname - 1] = '\0';
  char *sp = strchr(sh->syslog_hostname, ' ');
  if (sp) *sp = '\0';

  /* Cache APP-NAME for RFC 5424; keep only PRINTUSASCII (0x21-0x7e). */
  {
    const char *raw = _syslog_appname();
    size_t i = 0;
    for (; raw[i] && i < sizeof(sh->syslog_appname) - 1; i++) {
      unsigned char c = (unsigned char)raw[i];
      if (c < 0x21 || c > 0x7e) break;
      sh->syslog_appname[i] = raw[i];
    }
    sh->syslog_appname[i] = '\0';
    if (i == 0) {
      sh->syslog_appname[0] = '-';
      sh->syslog_appname[1] = '\0';
    }
  }

  return sh;
}

static struct clogger *_logger_alloc(clog_shared_t *shared,
                                     clog_level_t min_level) {
  struct clogger *lg = _mem_calloc(shared->m_procs, 1, sizeof *lg);
  if (!lg) return NULL;

  if (_buf_init(&lg->buf, shared->m_procs) != 0) {
    _mem_free(shared->m_procs, lg);
    return NULL;
  }

  lg->fields = _fields_create(shared->m_procs);
  if (!lg->fields) {
    _buf_free(&lg->buf);
    _mem_free(shared->m_procs, lg);
    return NULL;
  }

  lg->shared = shared;
  lg->min_level = min_level;
  return lg;
}

/* Free a fully-initialised logger without touching the shared backing store. */
static void _logger_free(struct clogger *lg) {
  __chmap_destroy(lg->fields);
  lg->fields = NULL;
  _buf_free(&lg->buf);
  _mem_free(lg->shared->m_procs, lg);
}

static struct clogger *_alloc(int fd, bool owns_fd, const char *file_path,
                              clog_level_t min_level,
                              ccol_memmgmt_procs_t *m_procs) {
  clog_shared_t *sh = _shared_alloc(fd, owns_fd, file_path, m_procs);
  if (!sh) return NULL;

  struct clogger *lg = _logger_alloc(sh, min_level);
  if (!lg) {
    mutex_destroy(sh->mutex);
    _shared_free_partial(sh);
    return NULL;
  }

  return lg;
}

/* ========================================================================== */
/*                         PUBLIC LIFECYCLE                                   */
/* ========================================================================== */

clog clog_open_fd_mp(int fd, clog_level_t min_level,
                     ccol_memmgmt_procs_t *mprocs) {
  if (fd < 0) return NULL;
  int _fd_flags = fcntl(fd, F_GETFL);
  if (_fd_flags == -1) return NULL;
  int _accmode = _fd_flags & O_ACCMODE;
  if (_accmode != O_WRONLY && _accmode != O_RDWR) return NULL;
  if (!ccol_verify_memmgmt_procs(mprocs, (char **)NULL)) return NULL;
  return _alloc(fd, false, NULL, min_level, mprocs);
}

clog clog_open_file_mp(const char *path, clog_level_t min_level,
                       const clog_rotation_cfg_t *cfg,
                       ccol_memmgmt_procs_t *mprocs) {
  if (!path) return NULL;
  if (!ccol_verify_memmgmt_procs(mprocs, (char **)NULL)) return NULL;

  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) return NULL;

  struct clogger *lg = _alloc(fd, true, path, min_level, mprocs);
  if (!lg) {
    close(fd);
    return NULL;
  }

  if (cfg) {
    lg->shared->rotation_enabled =
        cfg->size_rotation_enabled || cfg->time_rotation_enabled;
    lg->shared->rotation = *cfg;

    if (cfg->size_rotation_enabled && cfg->max_file_size <= 0)
      lg->shared->rotation.max_file_size = CLOG_DEFAULT_MAX_FILE_SIZE;

    if (cfg->time_rotation_enabled && cfg->rotation_interval_secs <= 0)
      lg->shared->rotation.rotation_interval_secs =
          CLOG_DEFAULT_ROTATION_INTERVAL;

    /* Bootstrap byte counter from the current on-disk size */
    struct stat st;
    if (fstat(fd, &st) == 0) lg->shared->bytes_written = st.st_size;
  }

  return lg;
}

void clog_close(clog lg) {
  clog_shared_t *sh = lg->shared;

  mutex_lock(sh->mutex);

  int remaining = --sh->ref_count;

  if (remaining == 0 && sh->owns_fd && sh->fd >= 0) {
    close(sh->fd);
    sh->fd = -1;
  }

  _logger_free(lg);

  mutex_unlock(sh->mutex);

  if (remaining == 0) {
    mutex_destroy(sh->mutex);
    _shared_free_partial(sh);
  }
}

clog clog_derive(clog parent) {
  mutex_lock(parent->shared->mutex);

  struct clogger *child = _logger_alloc(parent->shared, parent->min_level);
  if (!child) {
    mutex_unlock(parent->shared->mutex);
    return NULL;
  }

  /* Copy parent's fields into the child's independent field map */
  if (chmap_elem_count(parent->fields) > 0) {
    cmap_iterator *it = chashmap_begin_iter(parent->fields, NULL);
    if (!it) {
      _logger_free(child);
      mutex_unlock(parent->shared->mutex);
      return NULL;
    }
    while (it) {
      const char *k = (const char *)it->key_pair->ptr;
      const char *v = (const char *)it->val_pair->ptr;
      cmap_pair kp = {.ptr = (void *)k, .size = strlen(k) + 1};
      cmap_pair vp = {.ptr = (void *)v, .size = strlen(v) + 1};
      if (chmap_insert_elem(child->fields, &kp, &vp) != ccol_success) {
        ccol_iter_destroy(it);
        _logger_free(child);
        mutex_unlock(parent->shared->mutex);
        return NULL;
      }
      it = it->_next_fn(it);
    }
  }

  parent->shared->ref_count++;

  mutex_unlock(parent->shared->mutex);
  return child;
}

/* ========================================================================== */
/*                         LEVEL CONTROL                                      */
/* ========================================================================== */

void clog_set_level(clog lg, clog_level_t level) {
  mutex_lock(lg->shared->mutex);
  lg->min_level = level;
  mutex_unlock(lg->shared->mutex);
}

clog_level_t clog_get_level(clog lg) {
  mutex_lock(lg->shared->mutex);
  clog_level_t l = lg->min_level;
  mutex_unlock(lg->shared->mutex);
  return l;
}

/* ========================================================================== */
/*                         OUTPUT FORMAT */
/* ========================================================================== */

void clog_set_format(clog lg, clog_format_t fmt) {
  mutex_lock(lg->shared->mutex);
  /* CLOG_FMT_SYSLOG requires a caller-supplied fd (owns_fd == false). */
  if (fmt == CLOG_FMT_SYSLOG && lg->shared->owns_fd) {
    mutex_unlock(lg->shared->mutex);
    return;
  }
  lg->shared->format = fmt;
  mutex_unlock(lg->shared->mutex);
}

clog_format_t clog_get_format(clog lg) {
  mutex_lock(lg->shared->mutex);
  clog_format_t f = lg->shared->format;
  mutex_unlock(lg->shared->mutex);
  return f;
}

void clog_set_facility(clog lg, clog_syslog_facility_t facility) {
  mutex_lock(lg->shared->mutex);
  lg->shared->syslog_facility = facility;
  mutex_unlock(lg->shared->mutex);
}

clog_syslog_facility_t clog_get_facility(clog lg) {
  mutex_lock(lg->shared->mutex);
  clog_syslog_facility_t f = lg->shared->syslog_facility;
  mutex_unlock(lg->shared->mutex);
  return f;
}

/* ========================================================================== */
/*                         STRUCTURED FIELDS                                  */
/* ========================================================================== */

void clog_set_field(clog lg, const char *key, const char *value) {
  if (!key || !value) return;

  /* Reject keys that would corrupt logfmt or RFC 5424 SD output.
   * RFC 5424 SD-PARAM-NAME requires at least one character. */
  if (*key == '\0') return;
  for (const char *p = key; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x20 || c == 0x7f || *p == ' ' || *p == '=' || *p == '"' ||
        *p == '\\' || *p == ']')
      return;
  }

  mutex_lock(lg->shared->mutex);

  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  cmap_pair vp = {.ptr = (void *)value, .size = strlen(value) + 1};
  chmap_insert_elem(lg->fields, &kp, &vp);

  mutex_unlock(lg->shared->mutex);
}

void clog_remove_field(clog lg, const char *key) {
  if (!key) return;

  mutex_lock(lg->shared->mutex);

  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  chmap_delete_elem(lg->fields, &kp);

  mutex_unlock(lg->shared->mutex);
}

void clog_clear_fields(clog lg) {
  mutex_lock(lg->shared->mutex);
  chmap_reset(lg->fields, 0);
  mutex_unlock(lg->shared->mutex);
}

/* ========================================================================== */
/*                         CORE WRITE                                         */
/* ========================================================================== */

void _clog_write(clog lg, clog_level_t level, const char *file, int line,
                 const char *func, bool with_backtrace, const char *fmt, ...) {
  mutex_lock(lg->shared->mutex);

  /* CLOG_FATAL bypasses the level filter: the cause of termination must
   * always be recorded, regardless of min_level. */
  if (level < lg->min_level && level != CLOG_FATAL) {
    mutex_unlock(lg->shared->mutex);
    return;
  }

  if (lg->shared->fd < 0) {
    mutex_unlock(lg->shared->mutex);
    if (level == CLOG_FATAL) {
      exit(EXIT_FAILURE);
    }
    return;
  }

  const clog_format_t log_fmt = lg->shared->format;

  /* ----------------------------------------------------------------------- */
  /* Format the user message into a stack buffer; spill to heap if needed.   */
  /* ----------------------------------------------------------------------- */
  char msg_stack[1024];
  char *msg = msg_stack;
  char *msg_heap = NULL;

  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);

  int mlen = vsnprintf(msg_stack, sizeof msg_stack, fmt, ap);
  va_end(ap);

  if (mlen < 0) {
    va_end(ap2);
    mutex_unlock(lg->shared->mutex);
    if (level == CLOG_FATAL) {
      exit(EXIT_FAILURE);
    }
    return;
  }
  if ((size_t)mlen >= sizeof msg_stack) {
    msg_heap = _mem_alloc(lg->shared->m_procs, (size_t)mlen + 1);
    if (msg_heap) {
      int mlen2 = vsnprintf(msg_heap, (size_t)mlen + 1, fmt, ap2);
      if (mlen2 >= 0) msg = msg_heap;
      /* On vsnprintf failure msg_heap is freed below; fall back to stack
       * version */
    }
    /* On allocation failure we fall back to the truncated stack version */
  }
  va_end(ap2);

  /* ----------------------------------------------------------------------- */
  /* Time-based rotation check (before the write).                           */
  /* ----------------------------------------------------------------------- */
  if (lg->shared->rotation_enabled &&
      lg->shared->rotation.time_rotation_enabled) {
    time_t now = time(NULL);
    if (now - lg->shared->last_rotation >=
        lg->shared->rotation.rotation_interval_secs)
      _rotate(lg->shared);
  }

  /* ----------------------------------------------------------------------- */
  /* Build proc identifier: [progname(pid):tname(tid)]                       */
  /* ----------------------------------------------------------------------- */
  char proc_val[256];
  {
    char tname[16];
    _get_thread_name(tname, sizeof tname);
    snprintf(proc_val, sizeof proc_val, "%.200s(%d):%.15s(%d)", _get_progname(),
             (int)getpid(), tname, (int)_get_tid());
  }

  /* ----------------------------------------------------------------------- */
  /* Build the log line.                                                      */
  /* ----------------------------------------------------------------------- */
  _buf_reset(&lg->buf);

  if (log_fmt == CLOG_FMT_JSON) {
    /* JSON:
     * {"ts":"...","level":"...","src":"file:N","func":"...",...,"msg":"..."} */
    _buf_append(&lg->buf, "{\"ts\":\"", 7);
    _buf_append_ts(&lg->buf);
    _buf_append(&lg->buf, "\"", 1);
    _buf_appendf(&lg->buf, ",\"level\":\"%s\"", _LEVEL_STR[level]);
    _buf_append(&lg->buf, ",\"proc\":\"", 9);
    _buf_append_json_content(&lg->buf, proc_val);
    _buf_append(&lg->buf, "\"", 1);
    _buf_append(&lg->buf, ",\"src\":\"", 8);
    _buf_append_json_content(&lg->buf, file);
    _buf_appendf(&lg->buf, ":%d\"", line);
    _buf_append(&lg->buf, ",\"func\":\"", 9);
    _buf_append_json_content(&lg->buf, func);
    _buf_append(&lg->buf, "\"", 1);

    if (chmap_elem_count(lg->fields) > 0) {
      cmap_iterator *it = chashmap_begin_iter(lg->fields, NULL);
      while (it) {
        const char *k = (const char *)it->key_pair->ptr;
        const char *v = (const char *)it->val_pair->ptr;
        size_t field_start = lg->buf.len;
        if (_buf_append_json_kv(&lg->buf, k, v) != 0) {
          lg->buf.len =
              field_start; /* roll back partial field so JSON stays valid */
          ccol_iter_destroy(it);
          break;
        }
        it = it->_next_fn(it);
      }
    }

    _buf_append(&lg->buf, ",\"msg\":\"", 8);
    _buf_append_json_content(&lg->buf, msg);
    _buf_append(&lg->buf, "\"", 1);

    if (with_backtrace) _emit_backtrace_json(&lg->buf);

    _buf_append(&lg->buf, "}\n", 2);
  } else if (log_fmt == CLOG_FMT_SYSLOG) {
    /* RFC 5424: <PRI>1 TIMESTAMP HOSTNAME APP-NAME PROCID MSGID [SD] MSG\n */
    int pri = (int)lg->shared->syslog_facility * 8 + _SYSLOG_SEVERITY[level];
    const char *hostname =
        lg->shared->syslog_hostname[0] ? lg->shared->syslog_hostname : "-";
    const char *appname = lg->shared->syslog_appname;

    _buf_appendf(&lg->buf, "<%d>1 ", pri);
    _buf_append_ts(&lg->buf);
    _buf_appendf(&lg->buf, " %s %s %d %s", hostname, appname, (int)getpid(),
                 _LEVEL_STR[level]);

    /* Structured data: [ccol proc="name:pid:tid" src="file:N" func="fn"
     * <user-fields>] */
    _buf_append(&lg->buf, " [ccol proc=\"", 13);
    _buf_append_sd_value(&lg->buf, proc_val);
    _buf_append(&lg->buf, "\" src=\"", 7);
    _buf_append_sd_value(&lg->buf, file);
    _buf_appendf(&lg->buf, ":%d\" func=\"", line);
    _buf_append_sd_value(&lg->buf, func);
    _buf_append(&lg->buf, "\"", 1);

    if (chmap_elem_count(lg->fields) > 0) {
      cmap_iterator *it = chashmap_begin_iter(lg->fields, NULL);
      while (it) {
        const char *k = (const char *)it->key_pair->ptr;
        const char *v = (const char *)it->val_pair->ptr;
        size_t field_start = lg->buf.len;
        /* RFC 5424 SD-PARAM-NAME is at most 32 PRINTUSASCII chars. */
        if (_buf_appendf(&lg->buf, " %.32s=\"", k) != 0 ||
            _buf_append_sd_value(&lg->buf, v) != 0 ||
            _buf_append(&lg->buf, "\"", 1) != 0) {
          lg->buf.len = field_start;
          ccol_iter_destroy(it);
          break;
        }
        it = it->_next_fn(it);
      }
    }

    _buf_append(&lg->buf, "] ", 2);
    _buf_append(&lg->buf, msg, strlen(msg));
    _buf_append(&lg->buf, "\n", 1);
  } else {
    /* Logfmt: ts=... level=... proc=... src=...:N func=... [fields] msg=... */
    _buf_append(&lg->buf, "ts=", 3);
    _buf_append_ts(&lg->buf);
    _buf_appendf(&lg->buf, " level=%s", _LEVEL_STR[level]);
    _buf_append(&lg->buf, " proc=", 6);
    _buf_append_lv(&lg->buf, proc_val);
    _buf_append(&lg->buf, " src=", 5);
    {
      /* Build "file:line" as a single value so _buf_append_lv quotes it if
       * the path contains logfmt-special characters (spaces, =, etc.). */
      char src_val[512];
      int slen = snprintf(src_val, sizeof src_val, "%s:%d", file, line);
      if (slen > 0 && (size_t)slen < sizeof src_val)
        _buf_append_lv(&lg->buf, src_val);
      else
        _buf_appendf(&lg->buf, "%s:%d", file, line);
    }
    _buf_append(&lg->buf, " func=", 6);
    _buf_append_lv(&lg->buf, func);

    if (chmap_elem_count(lg->fields) > 0) {
      cmap_iterator *it = chashmap_begin_iter(lg->fields, NULL);
      while (it) {
        const char *k = (const char *)it->key_pair->ptr;
        const char *v = (const char *)it->val_pair->ptr;
        size_t field_start = lg->buf.len;
        if (_buf_appendf(&lg->buf, " %s=", k) != 0 ||
            _buf_append_lv(&lg->buf, v) != 0) {
          lg->buf.len = field_start; /* roll back partial field */
          ccol_iter_destroy(it);
          break;
        }
        it = it->_next_fn(it);
      }
    }

    _buf_append(&lg->buf, " msg=", 5);
    _buf_append_lv(&lg->buf, msg);
    _buf_append(&lg->buf, "\n", 1);
  }

  _mem_free(lg->shared->m_procs, msg_heap);

  /* ----------------------------------------------------------------------- */
  /* Emit.                                                                    */
  /* ----------------------------------------------------------------------- */
  if (lg->shared->fd >= 0) {
    _write_all(lg->shared->fd, lg->buf.data, lg->buf.len);
    if (lg->shared->rotation_enabled)
      lg->shared->bytes_written += (off_t)lg->buf.len;
  }

  /* ----------------------------------------------------------------------- */
  /* Backtrace -- JSON embeds inline; logfmt and syslog write per-frame.      */
  /* ----------------------------------------------------------------------- */
  if (with_backtrace && lg->shared->fd >= 0) {
    if (log_fmt == CLOG_FMT_SYSLOG)
      _emit_backtrace_syslog(lg, level);
    else if (log_fmt != CLOG_FMT_JSON)
      _emit_backtrace(lg);
  }

  /* ----------------------------------------------------------------------- */
  /* Size-based rotation check (after the write).                             */
  /* ----------------------------------------------------------------------- */
  if (lg->shared->rotation_enabled &&
      lg->shared->rotation.size_rotation_enabled &&
      lg->shared->bytes_written >= lg->shared->rotation.max_file_size)
    _rotate(lg->shared);

  mutex_unlock(lg->shared->mutex);
  if (level == CLOG_FATAL) {
    exit(EXIT_FAILURE);
  }
}
