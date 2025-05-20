/*
Copyright: Boaz Segev, 2017-2019
License: MIT
*/

#include <assert.h>
#include <errno.h>
#include <fio.h>
#include <fiobj_numbers.h>
#include <fiobject.h>
#include <math.h>

/* *****************************************************************************
Numbers Type
***************************************************************************** */

typedef struct {
  fiobj_object_header_s head;
  intptr_t i;
} fiobj_num_s;

typedef struct {
  fiobj_object_header_s head;
  double f;
} fiobj_float_s;

#define obj2num(o) ((fiobj_num_s *)FIOBJ2PTR(o))
#define obj2float(o) ((fiobj_float_s *)FIOBJ2PTR(o))

/* *****************************************************************************
Numbers VTable
***************************************************************************** */

static __thread char num_buffer[512];

static intptr_t fio_i2i(const FIOBJ o) { return obj2num(o)->i; }
static intptr_t fio_f2i(const FIOBJ o) {
  return (intptr_t)floorl(obj2float(o)->f);
}
static double fio_i2f(const FIOBJ o) { return (double)obj2num(o)->i; }
static double fio_f2f(const FIOBJ o) { return obj2float(o)->f; }

static size_t fio_itrue(const FIOBJ o) { return (obj2num(o)->i != 0); }
static size_t fio_ftrue(const FIOBJ o) { return (obj2float(o)->f != 0); }

static fio_str_info_s fio_i2str(const FIOBJ o) {
  return (fio_str_info_s){
      .data = num_buffer,
      .len = fio_ltoa(num_buffer, obj2num(o)->i, 10),
  };
}
static fio_str_info_s fio_f2str(const FIOBJ o) {
  if (isnan(obj2float(o)->f))
    return (fio_str_info_s){.data = (char *)"NaN", .len = 3};
  else if (isinf(obj2float(o)->f)) {
    if (obj2float(o)->f > 0)
      return (fio_str_info_s){.data = (char *)"Infinity", .len = 8};
    else
      return (fio_str_info_s){.data = (char *)"-Infinity", .len = 9};
  }
  return (fio_str_info_s){
      .data = num_buffer,
      .len = fio_ftoa(num_buffer, obj2float(o)->f, 10),
  };
}

static size_t fiobj_i_is_eq(const FIOBJ self, const FIOBJ other) {
  return obj2num(self)->i == obj2num(other)->i;
}
static size_t fiobj_f_is_eq(const FIOBJ self, const FIOBJ other) {
  return obj2float(self)->f == obj2float(other)->f;
}

void fiobject___simple_dealloc(FIOBJ o, void (*task)(FIOBJ, void *), void *arg);
uintptr_t fiobject___noop_count(FIOBJ o);

const fiobj_object_vtable_s FIOBJECT_VTABLE_NUMBER = {
    .class_name = "Number",
    .to_i = fio_i2i,
    .to_f = fio_i2f,
    .to_str = fio_i2str,
    .is_true = fio_itrue,
    .is_eq = fiobj_i_is_eq,
    .count = fiobject___noop_count,
    .dealloc = fiobject___simple_dealloc,
};

const fiobj_object_vtable_s FIOBJECT_VTABLE_FLOAT = {
    .class_name = "Float",
    .to_i = fio_f2i,
    .to_f = fio_f2f,
    .is_true = fio_ftrue,
    .to_str = fio_f2str,
    .is_eq = fiobj_f_is_eq,
    .count = fiobject___noop_count,
    .dealloc = fiobject___simple_dealloc,
};

/* *****************************************************************************
Number API
***************************************************************************** */

FIOBJ fiobj_num_new_bignum(intptr_t num) {
  fiobj_num_s *o = fio_malloc(sizeof(*o));
  FIO_ASSERT_ALLOC(o);
  *o = (fiobj_num_s){
      .head = {.ref = 1, .type = FIOBJ_T_NUMBER},
      .i = num,
  };
  return (FIOBJ)o;
}

/** Creates a temporary Number object. This ignores `fiobj_free`. */
FIOBJ fiobj_num_tmp(intptr_t num) {
  static __thread fiobj_num_s ret;
  ret = (fiobj_num_s){
      .head = {.type = FIOBJ_T_NUMBER, .ref = ((~(uint32_t)0) >> 4)},
      .i = num,
  };
  return (FIOBJ)&ret;
}

/* *****************************************************************************
Numbers to Strings - Buffered
***************************************************************************** */

fio_str_info_s fio_ltocstr(long i) {
  return (fio_str_info_s){.data = num_buffer,
                          .len = fio_ltoa(num_buffer, i, 10)};
}

