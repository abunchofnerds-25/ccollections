/*
Copyright: Boaz Segev, 2017-2019
License: MIT
*/

/**
This facil.io core library provides wrappers around complex and (or) dynamic
types, abstracting some complexity and making dynamic type related tasks easier.
*/

#include <fiobject.h>

#define FIO_ARY_NAME fiobj_stack
#define FIO_ARY_TYPE FIOBJ
#define FIO_ARY_INVALID FIOBJ_INVALID
/* don't free or compare objects, this stack shouldn't have side-effects */
#include <fio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* *****************************************************************************
Use the facil.io features when available, but override when missing.
***************************************************************************** */
#ifndef fd_data /* defined in fio.c */

#pragma weak fio_malloc
void *fio_malloc(size_t size) {
  void *m = malloc(size);
  if (m) memset(m, 0, size);
  return m;
}

#pragma weak fio_calloc
void *__attribute__((weak)) fio_calloc(size_t size, size_t count) {
  return calloc(size, count);
}

#pragma weak fio_free
void __attribute__((weak)) fio_free(void *ptr) { free(ptr); }

#pragma weak fio_realloc
void *__attribute__((weak)) fio_realloc(void *ptr, size_t new_size) {
  return realloc(ptr, new_size);
}

#pragma weak fio_realloc2
void *__attribute__((weak)) fio_realloc2(void *ptr, size_t new_size,
                                         size_t valid_len) {
  return realloc(ptr, new_size);
  (void)valid_len;
}

#pragma weak fio_mmap
void *__attribute__((weak)) fio_mmap(size_t size) { return fio_malloc(size); }

/** The logging level */
#if DEBUG
#pragma weak FIO_LOG_LEVEL
int __attribute__((weak)) FIO_LOG_LEVEL = FIO_LOG_LEVEL_DEBUG;
#else
#pragma weak FIO_LOG_LEVEL
int __attribute__((weak)) FIO_LOG_LEVEL = FIO_LOG_LEVEL_INFO;
#endif

/**
 * We include this in case the parser is used outside of facil.io.
 */
int64_t __attribute__((weak)) fio_atol(char **pstr) {
  return strtoll(*pstr, pstr, 0);
}
#pragma weak fio_atol

/**
 * We include this in case the parser is used outside of facil.io.
 */
double __attribute__((weak)) fio_atof(char **pstr) {
  return strtod(*pstr, pstr);
}
#pragma weak fio_atof

/**
 * A helper function that writes a signed int64_t to a string.
 *
 * No overflow guard is provided, make sure there's at least 68 bytes
 * available (for base 2).
 *
 * Offers special support for base 2 (binary), base 8 (octal), base 10 and base
 * 16 (hex). An unsupported base will silently default to base 10. Prefixes
 * are automatically added (i.e., "0x" for hex and "0b" for base 2).
 *
 * Returns the number of bytes actually written (excluding the NUL
 * terminator).
 */
#pragma weak fio_ltoa
size_t __attribute__((weak)) fio_ltoa(char *dest, int64_t num, uint8_t base) {
  const char notation[] = {'0', '1', '2', '3', '4', '5', '6', '7',
                           '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

  size_t len = 0;
  char buf[48]; /* we only need up to 20 for base 10, but base 3 needs 41... */

  if (!num) goto zero;

  switch (base) {
    case 1: /* fallthrough */
    case 2:
      /* Base 2 */
      {
        uint64_t n =
            num;       /* avoid bit shifting inconsistencies with signed bit */
        uint8_t i = 0; /* counting bits */
        dest[len++] = '0';
        dest[len++] = 'b';

        while ((i < 64) && (n & 0x8000000000000000) == 0) {
          n = n << 1;
          i++;
        }
        /* make sure the Binary representation doesn't appear signed. */
        if (i) {
          dest[len++] = '0';
        }
        /* write to dest. */
        while (i < 64) {
          dest[len++] = ((n & 0x8000000000000000) ? '1' : '0');
          n = n << 1;
          i++;
        }
        dest[len] = 0;
        return len;
      }
    case 8:
      /* Base 8 */
      {
        uint64_t l = 0;
        if (num < 0) {
          dest[len++] = '-';
          num = 0 - num;
        }
        dest[len++] = '0';

        while (num) {
          buf[l++] = '0' + (num & 7);
          num = num >> 3;
        }
        while (l) {
          --l;
          dest[len++] = buf[l];
        }
        dest[len] = 0;
        return len;
      }

    case 16:
      /* Base 16 */
      {
        uint64_t n =
            num;       /* avoid bit shifting inconsistencies with signed bit */
        uint8_t i = 0; /* counting bits */
        dest[len++] = '0';
        dest[len++] = 'x';
        while (i < 8 && (n & 0xFF00000000000000) == 0) {
          n = n << 8;
          i++;
        }
        /* make sure the Hex representation doesn't appear signed. */
        if (i && (n & 0x8000000000000000)) {
          dest[len++] = '0';
          dest[len++] = '0';
        }
        /* write the damn thing */
        while (i < 8) {
          uint8_t tmp = (n & 0xF000000000000000) >> 60;
          dest[len++] = notation[tmp];
          tmp = (n & 0x0F00000000000000) >> 56;
          dest[len++] = notation[tmp];
          i++;
          n = n << 8;
        }
        dest[len] = 0;
        return len;
      }
    case 3: /* fallthrough */
    case 4: /* fallthrough */
    case 5: /* fallthrough */
    case 6: /* fallthrough */
    case 7: /* fallthrough */
    case 9: /* fallthrough */
      /* rare bases */
      if (num < 0) {
        dest[len++] = '-';
        num = 0 - num;
      }
      uint64_t l = 0;
      while (num) {
        uint64_t t = num / base;
        buf[l++] = '0' + (num - (t * base));
        num = t;
      }
      while (l) {
        --l;
        dest[len++] = buf[l];
      }
      dest[len] = 0;
      return len;

    default:
      break;
  }
  /* Base 10, the default base */

  if (num < 0) {
    dest[len++] = '-';
    num = 0 - num;
  }
  uint64_t l = 0;
  while (num) {
    uint64_t t = num / 10;
    buf[l++] = '0' + (num - (t * 10));
    num = t;
  }
  while (l) {
    --l;
    dest[len++] = buf[l];
  }
  dest[len] = 0;
  return len;

zero:
  switch (base) {
    case 1: /* fallthrough */
    case 2:
      dest[len++] = '0';
      dest[len++] = 'b';
      /* fallthrough */
    case 16:
      dest[len++] = '0';
      dest[len++] = 'x';
      dest[len++] = '0';
  }
  dest[len++] = '0';
  dest[len] = 0;
  return len;
}

/**
 * A helper function that converts between a double to a string.
 *
 * No overflow guard is provided, make sure there's at least 130 bytes
 * available (for base 2).
 *
 * Supports base 2, base 10 and base 16. An unsupported base will silently
 * default to base 10. Prefixes aren't added (i.e., no "0x" or "0b" at the
 * beginning of the string).
 *
 * Returns the number of bytes actually written (excluding the NUL
 * terminator).
 */
#pragma weak fio_ftoa
size_t __attribute__((weak)) fio_ftoa(char *dest, double num, uint8_t base) {
  if (base == 2 || base == 16) {
    /* handle the binary / Hex representation the same as if it were an
     * int64_t
     */
    int64_t *i = (void *)&num;
    return fio_ltoa(dest, *i, base);
  }

  size_t written = sprintf(dest, "%g", num);
  uint8_t need_zero = 1;
  char *start = dest;
  while (*start) {
    if (*start == ',')  // locale issues?
      *start = '.';
    if (*start == '.' || *start == 'e') {
      need_zero = 0;
      break;
    }
    start++;
  }
  if (need_zero) {
    dest[written++] = '.';
    dest[written++] = '0';
  }
  return written;
}

#endif
/* *****************************************************************************
the `fiobj_each2` function
***************************************************************************** */

struct task_packet_s {
  int (*task)(FIOBJ obj, void *arg);
  void *arg;
  fiobj_stack_s *stack;
  FIOBJ next;
  uintptr_t counter;
  uint8_t stop;
  uint8_t incomplete;
};

static int fiobj_task_wrapper(FIOBJ o, void *p_) {
  struct task_packet_s *p = p_;
  ++p->counter;
  int ret = p->task(o, p->arg);
  if (ret == -1) {
    p->stop = 1;
    return -1;
  }
  if (FIOBJ_IS_ALLOCATED(o) && FIOBJECT2VTBL(o)->each) {
    p->incomplete = 1;
    p->next = o;
    return -1;
  }
  return 0;
}
/**
 * Single layer iteration using a callback for each nested fio object.
 *
 * Accepts any `FIOBJ ` type but only collections (Arrays and Hashes) are
 * processed. The container itself (the Array or the Hash) is **not** processed
 * (unlike `fiobj_each2`).
 *
 * The callback task function must accept an object and an opaque user pointer.
 *
 * Hash objects pass along a `FIOBJ_T_COUPLET` object, containing
 * references for both the key and the object. Keys shouldn't be altered once
 * placed as a key (or the Hash will break). Collections (Arrays / Hashes) can't
 * be used as keeys.
 *
 * If the callback returns -1, the loop is broken. Any other value is ignored.
 *
 * Returns the "stop" position, i.e., the number of items processed + the
 * starting point.
 */
size_t fiobj_each2(FIOBJ o, int (*task)(FIOBJ obj, void *arg), void *arg) {
  if (!o || !FIOBJ_IS_ALLOCATED(o) || (FIOBJECT2VTBL(o)->each == NULL)) {
    task(o, arg);
    return 1;
  }
  /* run task for root object */
  if (task(o, arg) == -1) return 1;
  uintptr_t pos = 0;
  fiobj_stack_s stack = FIO_ARY_INIT;
  struct task_packet_s packet = {
      .task = task,
      .arg = arg,
      .stack = &stack,
      .counter = 1,
  };
  do {
    if (!pos) packet.next = 0;
    packet.incomplete = 0;
    pos = FIOBJECT2VTBL(o)->each(o, pos, fiobj_task_wrapper, &packet);
    if (packet.stop) goto finish;
    if (packet.incomplete) {
      fiobj_stack_push(&stack, pos);
      fiobj_stack_push(&stack, o);
    }

    if (packet.next) {
      fiobj_stack_push(&stack, (FIOBJ)0);
      fiobj_stack_push(&stack, packet.next);
    }
    o = FIOBJ_INVALID;
    fiobj_stack_pop(&stack, &o);
    fiobj_stack_pop(&stack, &pos);
  } while (o);
finish:
  fiobj_stack_free(&stack);
  return packet.counter;
}

/* *****************************************************************************
Free complex objects (objects with nesting)
***************************************************************************** */

static void fiobj_dealloc_task(FIOBJ o, void *stack_) {
  // if (!o)
  //   fprintf(stderr, "* WARN: freeing a NULL no-object\n");
  // else
  //   fprintf(stderr, "* freeing object %s\n", fiobj_obj2cstr(o).data);
  if (!o || !FIOBJ_IS_ALLOCATED(o)) return;
  if (OBJREF_REM(o)) return;
  if (!FIOBJECT2VTBL(o)->each || !FIOBJECT2VTBL(o)->count(o)) {
    FIOBJECT2VTBL(o)->dealloc(o, NULL, NULL);
    return;
  }
  fiobj_stack_s *s = stack_;
  fiobj_stack_push(s, o);
}
/**
 * Decreases an object's reference count, releasing memory and
 * resources.
 *
 * This function affects nested objects, meaning that when an Array or
 * a Hash object is passed along, it's children (nested objects) are
 * also freed.
 */
void fiobj_free_complex_object(FIOBJ o) {
  fiobj_stack_s stack = FIO_ARY_INIT;
  do {
    FIOBJECT2VTBL(o)->dealloc(o, fiobj_dealloc_task, &stack);
  } while (!fiobj_stack_pop(&stack, &o));
  fiobj_stack_free(&stack);
}

/* *****************************************************************************
Is Equal?
***************************************************************************** */
#include <fiobj_hash.h>

static inline int fiobj_iseq_simple(const FIOBJ o, const FIOBJ o2) {
  if (o == o2) return 1;
  if (!o || !o2) return 0; /* they should have compared equal before. */
  if (!FIOBJ_IS_ALLOCATED(o) || !FIOBJ_IS_ALLOCATED(o2))
    return 0; /* they should have compared equal before. */
  if (FIOBJECT2HEAD(o)->type != FIOBJECT2HEAD(o2)->type)
    return 0; /* non-type equality is a barriar to equality. */
  if (!FIOBJECT2VTBL(o)->is_eq(o, o2)) return 0;
  return 1;
}

static int fiobj_iseq____internal_complex__task(FIOBJ o, void *ary_) {
  fiobj_stack_s *ary = ary_;
  fiobj_stack_push(ary, o);
  if (fiobj_hash_key_in_loop()) fiobj_stack_push(ary, fiobj_hash_key_in_loop());
  return 0;
}

/** used internally for complext nested tests (Array / Hash types) */
int fiobj_iseq____internal_complex__(FIOBJ o, FIOBJ o2) {
  // if (FIOBJECT2VTBL(o)->each && FIOBJECT2VTBL(o)->count(o))
  //   return int fiobj_iseq____internal_complex__(const FIOBJ o, const FIOBJ
  //   o2);
  fiobj_stack_s left = FIO_ARY_INIT, right = FIO_ARY_INIT, queue = FIO_ARY_INIT;
  do {
    fiobj_each1(o, 0, fiobj_iseq____internal_complex__task, &left);
    fiobj_each1(o2, 0, fiobj_iseq____internal_complex__task, &right);
    while (fiobj_stack_count(&left)) {
      o = FIOBJ_INVALID;
      o2 = FIOBJ_INVALID;
      fiobj_stack_pop(&left, &o);
      fiobj_stack_pop(&right, &o2);
      if (!fiobj_iseq_simple(o, o2)) goto unequal;
      if (FIOBJ_IS_ALLOCATED(o) && FIOBJECT2VTBL(o)->each &&
          FIOBJECT2VTBL(o)->count(o)) {
        fiobj_stack_push(&queue, o);
        fiobj_stack_push(&queue, o2);
      }
    }
    o = FIOBJ_INVALID;
    o2 = FIOBJ_INVALID;
    fiobj_stack_pop(&queue, &o2);
    fiobj_stack_pop(&queue, &o);
    if (!fiobj_iseq_simple(o, o2)) goto unequal;
  } while (o);
  fiobj_stack_free(&left);
  fiobj_stack_free(&right);
  fiobj_stack_free(&queue);
  return 1;
unequal:
  fiobj_stack_free(&left);
  fiobj_stack_free(&right);
  fiobj_stack_free(&queue);
  return 0;
}

/* *****************************************************************************
Defaults / NOOPs
***************************************************************************** */

void fiobject___simple_dealloc(FIOBJ o, void (*task)(FIOBJ, void *),
                               void *arg) {
  fio_free(FIOBJ2PTR(o));
  (void)task;
  (void)arg;
}

uintptr_t fiobject___noop_count(const FIOBJ o) {
  (void)o;
  return 0;
}

fio_str_info_s fiobject___noop_to_str(const FIOBJ o) {
  (void)o;
  return (fio_str_info_s){.len = 0, .data = NULL};
}
intptr_t fiobject___noop_to_i(const FIOBJ o) {
  (void)o;
  return 0;
}
double fiobject___noop_to_f(const FIOBJ o) {
  (void)o;
  return 0;
}

