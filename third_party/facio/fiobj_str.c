/*
Copyright: Boaz Segev, 2017-2019
License: MIT
*/

#if defined(__unix__) || defined(__APPLE__) || defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <unistd.h>
#endif

#ifdef _SC_PAGESIZE
#define PAGE_SIZE sysconf(_SC_PAGESIZE)
#else
#define PAGE_SIZE 4096
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fio_siphash.h>
#include <fiobj_numbers.h>
#include <fiobj_str.h>
#include <fiobject.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>

#define FIO_INCLUDE_STR
#define FIO_STR_NO_REF
#include <fio.h>

#ifndef PATH_MAX
#define PATH_MAX PAGE_SIZE
#endif

/* *****************************************************************************
String Type
***************************************************************************** */

typedef struct {
  fiobj_object_header_s head;
  uint64_t hash;
  fio_str_s str;
} fiobj_str_s;

#define obj2str(o) ((fiobj_str_s *)(FIOBJ2PTR(o)))

static inline fio_str_info_s fiobj_str_get_cstr(const FIOBJ o) {
  return fio_str_info(&obj2str(o)->str);
}

/* *****************************************************************************
String VTables
***************************************************************************** */

static fio_str_info_s fio_str2str(const FIOBJ o) {
  return fiobj_str_get_cstr(o);
}

static void fiobj_str_dealloc(FIOBJ o, void (*task)(FIOBJ, void *), void *arg) {
  fio_str_free(&obj2str(o)->str);
  fio_free(FIOBJ2PTR(o));
  (void)task;
  (void)arg;
}

static size_t fiobj_str_is_eq(const FIOBJ self, const FIOBJ other) {
  return fio_str_iseq(&obj2str(self)->str, &obj2str(other)->str);
}

static intptr_t fio_str2i(const FIOBJ o) {
  char *pos = fio_str_data(&obj2str(o)->str);
  return fio_atol(&pos);
}
static double fio_str2f(const FIOBJ o) {
  char *pos = fio_str_data(&obj2str(o)->str);
  return fio_atof(&pos);
}

static size_t fio_str2bool(const FIOBJ o) {
  return fio_str_len(&obj2str(o)->str) != 0;
}

uintptr_t fiobject___noop_count(const FIOBJ o);

const fiobj_object_vtable_s FIOBJECT_VTABLE_STRING = {
    .class_name = "String",
    .dealloc = fiobj_str_dealloc,
    .to_i = fio_str2i,
    .to_f = fio_str2f,
    .to_str = fio_str2str,
    .is_eq = fiobj_str_is_eq,
    .is_true = fio_str2bool,
    .count = fiobject___noop_count,
};

/* *****************************************************************************
String API
***************************************************************************** */

/** Creates a buffer String object. Remember to use `fiobj_free`. */
FIOBJ fiobj_str_buf(size_t capa) {
  if (capa)
    capa = capa + 1;
  else
    capa = PAGE_SIZE;

  fiobj_str_s *s = fio_malloc(sizeof(*s));
  if (!s) {
    perror("ERROR: fiobj string couldn't allocate memory");
    exit(errno);
  }
  *s = (fiobj_str_s){
      .head =
          {
              .ref = 1,
              .type = FIOBJ_T_STRING,
          },
      .str = FIO_STR_INIT,
  };
  if (capa) {
    fio_str_capa_assert(&s->str, capa);
  }
  return ((uintptr_t)s | FIOBJECT_STRING_FLAG);
}

/** Creates a String object. Remember to use `fiobj_free`. */
FIOBJ fiobj_str_new(const char *str, size_t len) {
  fiobj_str_s *s = fio_malloc(sizeof(*s));
  if (!s) {
    perror("ERROR: fiobj string couldn't allocate memory");
    exit(errno);
  }
  *s = (fiobj_str_s){
      .head =
          {
              .ref = 1,
              .type = FIOBJ_T_STRING,
          },
      .str = FIO_STR_INIT,
  };
  if (str && len) {
    fio_str_write(&s->str, str, len);
  }
  return ((uintptr_t)s | FIOBJECT_STRING_FLAG);
}

/**
 * Returns a thread-static temporary string. Avoid calling `fiobj_dup` or
 * `fiobj_free`.
 */
FIOBJ fiobj_str_tmp(void) {
  static __thread fiobj_str_s tmp = {
      .head =
          {
              .ref = ((~(uint32_t)0) >> 4),
              .type = FIOBJ_T_STRING,
          },
      .str = {.small = 1},
  };
  tmp.str.frozen = 0;
  fio_str_resize(&tmp.str, 0);
  return ((uintptr_t)&tmp | FIOBJECT_STRING_FLAG);
}

/** Prevents the String object from being changed. */
void fiobj_str_freeze(FIOBJ str) {
  if (FIOBJ_TYPE_IS(str, FIOBJ_T_STRING)) fio_str_freeze(&obj2str(str)->str);
}

/** Resizes a String object, allocating more memory if required. */
void fiobj_str_resize(FIOBJ str, size_t size) {
  assert(FIOBJ_TYPE_IS(str, FIOBJ_T_STRING));
  fio_str_resize(&obj2str(str)->str, size);
  obj2str(str)->hash = 0;
  return;
}

/**
 * Writes data at the end of the string, resizing the string as required.
 * Returns the new length of the String
 */
size_t fiobj_str_write(FIOBJ dest, const char *data, size_t len) {
  assert(FIOBJ_TYPE_IS(dest, FIOBJ_T_STRING));
  if (obj2str(dest)->str.frozen) return 0;
  obj2str(dest)->hash = 0;
  return fio_str_write(&obj2str(dest)->str, data, len).len;
}

/**
 * Writes a number at the end of the String using normal base 10 notation.
 *
 * Returns the new length of the String
 */
size_t fiobj_str_write_i(FIOBJ dest, int64_t num) {
  assert(FIOBJ_TYPE_IS(dest, FIOBJ_T_STRING));
  if (obj2str(dest)->str.frozen) return 0;
  obj2str(dest)->hash = 0;
  return fio_str_write_i(&obj2str(dest)->str, num).len;
}

/**
 * Writes data at the end of the string, resizing the string as required.
 * Returns the new length of the String
 */
size_t fiobj_str_concat(FIOBJ dest, FIOBJ obj) {
  assert(FIOBJ_TYPE_IS(dest, FIOBJ_T_STRING));
  if (obj2str(dest)->str.frozen) return 0;
  obj2str(dest)->hash = 0;
  fio_str_info_s o = fiobj_obj2cstr(obj);
  if (o.len == 0) return fio_str_len(&obj2str(dest)->str);
  return fio_str_write(&obj2str(dest)->str, o.data, o.len).len;
}

/**
 * Calculates a String's SipHash value for use as a HashMap key.
 */
uint64_t fiobj_str_hash(FIOBJ o) {
  assert(FIOBJ_TYPE_IS(o, FIOBJ_T_STRING));
  // if (obj2str(o)->is_small) {
  //   return fiobj_hash_string(STR_INTENAL_STR(o), STR_INTENAL_LEN(o));
  // } else
  if (obj2str(o)->hash) {
    return obj2str(o)->hash;
  }
  fio_str_info_s state = fio_str_info(&obj2str(o)->str);
  obj2str(o)->hash = fiobj_hash_string(state.data, state.len);
  return obj2str(o)->hash;
}
