/*
Copyright: Boaz Segev, 2017-2019
License: MIT
*/
#include <fio.h>
#include <fiobject.h>

#define FIO_ARY_NAME fio_ary__
#define FIO_ARY_TYPE FIOBJ
#define FIO_ARY_TYPE_INVALID FIOBJ_INVALID
#define FIO_ARY_TYPE_COMPARE(a, b) (fiobj_iseq((a), (b)))
#define FIO_ARY_INVALID FIOBJ_INVALID
#include <assert.h>
#include <fio.h>

/* *****************************************************************************
Array Type
***************************************************************************** */

typedef struct {
  fiobj_object_header_s head;
  fio_ary___s ary;
} fiobj_ary_s;

#define obj2ary(o) ((fiobj_ary_s *)(o))

/* *****************************************************************************
VTable
***************************************************************************** */

static void fiobj_ary_dealloc(FIOBJ o, void (*task)(FIOBJ, void *), void *arg) {
  FIO_ARY_FOR((&obj2ary(o)->ary), i) { task(*i, arg); }
  fio_ary___free(&obj2ary(o)->ary);
  fio_free(FIOBJ2PTR(o));
}

static size_t fiobj_ary_each1(FIOBJ o, size_t start_at,
                              int (*task)(FIOBJ obj, void *arg), void *arg) {
  return fio_ary___each(&obj2ary(o)->ary, start_at, task, arg);
}

static size_t fiobj_ary_is_eq(const FIOBJ self, const FIOBJ other) {
  fio_ary___s *a = &obj2ary(self)->ary;
  fio_ary___s *b = &obj2ary(other)->ary;
  if (fio_ary___count(a) != fio_ary___count(b)) return 0;
  return 1;
}

/** Returns the number of elements in the Array. */
size_t fiobj_ary_count(const FIOBJ ary) {
  assert(FIOBJ_TYPE_IS(ary, FIOBJ_T_ARRAY));
  return fio_ary___count(&obj2ary(ary)->ary);
}

static size_t fiobj_ary_is_true(const FIOBJ ary) {
  return fiobj_ary_count(ary) > 0;
}

fio_str_info_s fiobject___noop_to_str(const FIOBJ o);
intptr_t fiobject___noop_to_i(const FIOBJ o);
double fiobject___noop_to_f(const FIOBJ o);

const fiobj_object_vtable_s FIOBJECT_VTABLE_ARRAY = {
    .class_name = "Array",
    .dealloc = fiobj_ary_dealloc,
    .is_eq = fiobj_ary_is_eq,
    .is_true = fiobj_ary_is_true,
    .count = fiobj_ary_count,
    .each = fiobj_ary_each1,
    .to_i = fiobject___noop_to_i,
    .to_f = fiobject___noop_to_f,
    .to_str = fiobject___noop_to_str,
};

/* *****************************************************************************
Allocation
***************************************************************************** */

static inline FIOBJ fiobj_ary_alloc(size_t capa) {
  fiobj_ary_s *ary = fio_malloc(sizeof(*ary));
  if (!ary) {
    /* WARNING: terminates the entire process on OOM, taking down every
     * other in-flight connection with it; not just the one request that
     * happened to trigger the allocation. This is this vendor library's
     * allocation philosophy throughout (see also fiobj_str.c, fiobj_hash.c),
     * not something specific to this call site or fixable in isolation. */
    perror("ERROR: fiobj array couldn't allocate memory");
    exit(errno);
  }
  *ary = (fiobj_ary_s){
      .head =
          {
              .ref = 1,
              .type = FIOBJ_T_ARRAY,
          },
  };
  if (capa) fio_ary_____require_on_top(&ary->ary, capa);
  return (FIOBJ)ary;
}

/** Creates a mutable empty Array object. Use `fiobj_free` when done. */
FIOBJ fiobj_ary_new(void) { return fiobj_ary_alloc(0); }

/* *****************************************************************************
Array direct entry access API
***************************************************************************** */

/**
 * Returns a temporary object owned by the Array.
 *
 * Negative values are retrieved from the end of the array. i.e., `-1`
 * is the last item.
 */
FIOBJ fiobj_ary_index(FIOBJ ary, int64_t pos) {
  assert(ary && FIOBJ_TYPE_IS(ary, FIOBJ_T_ARRAY));
  return fio_ary___get(&obj2ary(ary)->ary, pos);
}

/* *****************************************************************************
Array push / shift API
***************************************************************************** */

/**
 * Pushes an object to the end of the Array.
 */
void fiobj_ary_push(FIOBJ ary, FIOBJ obj) {
  assert(ary && FIOBJ_TYPE_IS(ary, FIOBJ_T_ARRAY));
  fio_ary___push(&obj2ary(ary)->ary, obj);
}

/** Pops an object from the end of the Array. */
FIOBJ fiobj_ary_pop(FIOBJ ary) {
  assert(ary && FIOBJ_TYPE_IS(ary, FIOBJ_T_ARRAY));
  FIOBJ ret = FIOBJ_INVALID;
  fio_ary___pop(&obj2ary(ary)->ary, &ret);
  return ret;
}
