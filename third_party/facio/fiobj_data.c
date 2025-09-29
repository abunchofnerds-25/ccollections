/*
Copyright: Boaz Segev, 2017-2019
License: MIT
*/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/**
 * A dynamic type for reading / writing to a local file,  a temporary file or an
 * in-memory string.
 *
 * Supports basic reak, write, seek, puts and gets operations.
 *
 * Writing is always performed at the end of the stream / memory buffer,
 * ignoring the current seek position.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fio.h>
#include <fio_tmpfile.h>
#include <fiobj_data.h>
#include <fiobj_str.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* *****************************************************************************
Numbers Type
***************************************************************************** */

typedef struct {
  fiobj_object_header_s head;
  uint8_t *buffer; /* reader buffer */
  union {
    FIOBJ parent;
    void (*dealloc)(void *); /* buffer deallocation function */
    size_t fpos;             /* the file reader's position */
  } source;
  size_t capa; /* total buffer capacity / slice offset */
  size_t len;  /* length of valid data in buffer */
  size_t pos;  /* position of reader */
  int fd;      /* file descriptor (-1 if invalid). */
} fiobj_data_s;

#define obj2io(o) ((fiobj_data_s *)(o))

/* *****************************************************************************
Object required VTable and functions
***************************************************************************** */

#define REQUIRE_MEM(mem)                                        \
  do {                                                          \
    if ((mem) == NULL) {                                        \
      perror("FATAL ERROR: fiobj IO couldn't allocate memory"); \
      exit(errno);                                              \
    }                                                           \
  } while (0)

static void fiobj_data_copy_buffer(FIOBJ o) {
  obj2io(o)->capa = (((obj2io(o)->len) >> 12) + 1) << 12;
  void *tmp = fio_malloc(obj2io(o)->capa);
  REQUIRE_MEM(tmp);
  memcpy(tmp, obj2io(o)->buffer, obj2io(o)->len);
  if (obj2io(o)->source.dealloc) obj2io(o)->source.dealloc(obj2io(o)->buffer);
  obj2io(o)->source.dealloc = fio_free;
  obj2io(o)->buffer = tmp;
}

static inline void fiobj_data_pre_write(FIOBJ o, uintptr_t length) {
  switch (obj2io(o)->fd) {
    case -1:
      if (obj2io(o)->source.dealloc != fio_free) {
        fiobj_data_copy_buffer(o);
      }
      break;
  }
  if (obj2io(o)->capa >= obj2io(o)->len + length) return;
  /* add rounded pages (4096) to capacity */
  obj2io(o)->capa = (((obj2io(o)->len + length) >> 12) + 1) << 12;
  obj2io(o)->buffer = fio_realloc(obj2io(o)->buffer, obj2io(o)->capa);
  REQUIRE_MEM(obj2io(o)->buffer);
}

static inline int64_t fiobj_data_get_fd_size(const FIOBJ o) {
  struct stat stat;
retry:
  if (fstat(obj2io(o)->fd, &stat)) {
    if (errno == EINTR) goto retry;
    return -1;
  }
  return stat.st_size;
}

static FIOBJ fiobj_data_alloc(void *buffer, int fd) {
  fiobj_data_s *io = fio_malloc(sizeof(*io));
  REQUIRE_MEM(io);
  *io = (fiobj_data_s){
      .head = {.ref = 1, .type = FIOBJ_T_DATA},
      .buffer = buffer,
      .fd = fd,
  };
  return (FIOBJ)io;
}

static void fiobj_data_dealloc(FIOBJ o, void (*task)(FIOBJ, void *),
                               void *arg) {
  switch (obj2io(o)->fd) {
    case -1:
      if (obj2io(o)->source.dealloc && obj2io(o)->buffer)
        obj2io(o)->source.dealloc(obj2io(o)->buffer);
      break;
    default:
      close(obj2io(o)->fd);
      fio_free(obj2io(o)->buffer);
      break;
  }
  fio_free((void *)o);
  (void)task;
  (void)arg;
}

static intptr_t fiobj_data_i(const FIOBJ o) {
  switch (obj2io(o)->fd) {
    case -1:
      return obj2io(o)->len;
      break;
    default:
      return fiobj_data_get_fd_size(o);
  }
}

static size_t fiobj_data_is_true(const FIOBJ o) { return fiobj_data_i(o) > 0; }

static fio_str_info_s fio_io2str(const FIOBJ o) {
  switch (obj2io(o)->fd) {
    case -1:
      return (fio_str_info_s){.data = (char *)obj2io(o)->buffer,
                              .len = obj2io(o)->len};
      break;
  }
  int64_t i = fiobj_data_get_fd_size(o);
  if (i <= 0)
    return (fio_str_info_s){.data = (char *)obj2io(o)->buffer,
                            .len = obj2io(o)->len};
  obj2io(o)->len = 0;
  obj2io(o)->pos = 0;
  fiobj_data_pre_write((FIOBJ)o, i + 1);
  if (pread(obj2io(o)->fd, obj2io(o)->buffer, i, 0) != i)
    return (fio_str_info_s){.data = NULL, .len = 0};
  obj2io(o)->buffer[i] = 0;
  return (fio_str_info_s){.data = (char *)obj2io(o)->buffer, .len = i};
}

static size_t fiobj_data_iseq(const FIOBJ self, const FIOBJ other) {
  int64_t len;
  return ((len = fiobj_data_i(self)) == fiobj_data_i(other) &&
          !memcmp(fio_io2str(self).data, fio_io2str(other).data, (size_t)len));
}

uintptr_t fiobject___noop_count(FIOBJ o);
double fiobject___noop_to_f(FIOBJ o);

const fiobj_object_vtable_s FIOBJECT_VTABLE_DATA = {
    .class_name = "IO",
    .dealloc = fiobj_data_dealloc,
    .to_i = fiobj_data_i,
    .to_str = fio_io2str,
    .is_eq = fiobj_data_iseq,
    .is_true = fiobj_data_is_true,
    .to_f = fiobject___noop_to_f,
    .count = fiobject___noop_count,
};

/* *****************************************************************************
Seeking for characters in a string
***************************************************************************** */

#if FIO_MEMCHAR

/**
 * This seems to be faster on some systems, especially for smaller distances.
 *
 * On newer systems, `memchr` should be faster.
 */
static inline int swallow_ch(uint8_t **buffer, register uint8_t *const limit,
                             const uint8_t c) {
  if (**buffer == c) return 1;

#if !ALLOW_UNALIGNED_MEMORY_ACCESS || !defined(__x86_64__)
  /* too short for this mess */
  if ((uintptr_t)limit <= 16 + ((uintptr_t)*buffer & (~(uintptr_t)7)))
    goto finish;

  /* align memory */
  {
    const uint8_t *alignment =
        (uint8_t *)(((uintptr_t)(*buffer) & (~(uintptr_t)7)) + 8);
    if (limit >= alignment) {
      while (*buffer < alignment) {
        if (**buffer == c) {
          (*buffer)++;
          return 1;
        }
        *buffer += 1;
      }
    }
  }
  const uint8_t *limit64 = (uint8_t *)((uintptr_t)limit & (~(uintptr_t)7));
#else
  const uint8_t *limit64 = (uint8_t *)limit - 7;
#endif
  uint64_t wanted1 = 0x0101010101010101ULL * c;
  for (; *buffer < limit64; *buffer += 8) {
    const uint64_t eq1 = ~((*((uint64_t *)*buffer)) ^ wanted1);
    const uint64_t t0 = (eq1 & 0x7f7f7f7f7f7f7f7fllu) + 0x0101010101010101llu;
    const uint64_t t1 = (eq1 & 0x8080808080808080llu);
    if ((t0 & t1)) {
      break;
    }
  }
#if !ALLOW_UNALIGNED_MEMORY_ACCESS || !defined(__x86_64__)
finish:
#endif
  while (*buffer < limit) {
    if (**buffer == c) {
      (*buffer)++;
      return 1;
    }
    (*buffer)++;
  }

  return 0;
}
#else

static inline int swallow_ch(uint8_t **buffer, uint8_t *const limit,
                             const uint8_t c) {
  if (limit - *buffer == 0) return 0;
  void *tmp = memchr(*buffer, c, limit - (*buffer));
  if (tmp) {
    *buffer = tmp;
    (*buffer)++;
    return 1;
  }
  *buffer = (uint8_t *)limit;
  return 0;
}

#endif

/* *****************************************************************************
Creating the IO object
***************************************************************************** */

/** Creates a new local in-memory IO object */
FIOBJ fiobj_data_newstr(void) {
  FIOBJ o = fiobj_data_alloc(fio_malloc(4096), -1);
  REQUIRE_MEM(obj2io(o)->buffer);
  obj2io(o)->capa = 4096;
  obj2io(o)->source.dealloc = fio_free;
  return o;
}

/** Creates a new local file IO object */
FIOBJ fiobj_data_newfd(int fd) {
  FIOBJ o = fiobj_data_alloc(fio_malloc(4096), fd);
  REQUIRE_MEM(obj2io(o)->buffer);
  obj2io(o)->source.fpos = 0;
  return o;
}

/** Creates a new local tempfile IO object */
FIOBJ fiobj_data_newtmpfile(void) {
  // create a temporary file to contain the data.
  int fd = fio_tmpfile();
  if (fd == -1) return 0;
  return fiobj_data_newfd(fd);
}
/* *****************************************************************************
Reading API
***************************************************************************** */

/** Reads up to `length` bytes */
static fio_str_info_s fiobj_data_read_str(FIOBJ io, intptr_t length) {
  if (obj2io(io)->pos == obj2io(io)->len) {
    /* EOF */
    return (fio_str_info_s){.data = NULL, .len = 0};
  }

  if (length <= 0) {
    /* read to EOF - length */
    length = (obj2io(io)->len - obj2io(io)->pos) + length;
  }

  if (length <= 0) {
    /* We are at EOF - length or beyond */
    return (fio_str_info_s){.data = NULL, .len = 0};
  }

  /* reading length bytes */
  register size_t pos = obj2io(io)->pos;
  obj2io(io)->pos = pos + length;
  if (obj2io(io)->pos > obj2io(io)->len) obj2io(io)->pos = obj2io(io)->len;
  return (fio_str_info_s){
      .data = (char *)(obj2io(io)->buffer + pos),
      .len = (obj2io(io)->pos - pos),
  };
}

/** Reads up to `length` bytes */
static fio_str_info_s fiobj_data_read_file(FIOBJ io, intptr_t length) {
  uintptr_t fsize = fiobj_data_get_fd_size(io);

  if (length <= 0) {
    /* read to EOF - length */
    length = (fsize - obj2io(io)->source.fpos) + length;
  }

  if (length <= 0) {
    /* We are at EOF - length or beyond */
    errno = 0;
    return (fio_str_info_s){.data = NULL, .len = 0};
  }

  /* reading length bytes */
  if (length + obj2io(io)->pos <= obj2io(io)->len) {
    /* the data already exists in the buffer */
    // fprintf(stderr, "in_buffer...\n");
    fio_str_info_s data = {
        .data = (char *)(obj2io(io)->buffer + obj2io(io)->pos),
        .len = (uintptr_t)length};
    obj2io(io)->pos += length;
    obj2io(io)->source.fpos += length;
    return data;
  } else {
    /* read the data into the buffer - internal counting gets invalidated */
    // fprintf(stderr, "populate buffer...\n");
    obj2io(io)->len = 0;
    obj2io(io)->pos = 0;
    fiobj_data_pre_write(io, length);
    ssize_t l;
  retry_int:
    l = pread(obj2io(io)->fd, obj2io(io)->buffer, length,
              obj2io(io)->source.fpos);
    if (l == -1 && errno == EINTR) goto retry_int;
    if (l == -1 || l == 0) return (fio_str_info_s){.data = NULL, .len = 0};
    obj2io(io)->source.fpos += l;
    return (fio_str_info_s){.data = (char *)obj2io(io)->buffer, .len = l};
  }
}

/**
 * Reads up to `length` bytes and returns a temporary(!) C string object.
 *
 * The C string object will be invalidate the next time a function call to the
 * IO object is made.
 */
fio_str_info_s fiobj_data_read(FIOBJ io, intptr_t length) {
  if (!io || !FIOBJ_TYPE_IS(io, FIOBJ_T_DATA)) {
    errno = EFAULT;
    return (fio_str_info_s){.data = NULL, .len = 0};
  }
  errno = 0;
  switch (obj2io(io)->fd) {
    case -1:
      return fiobj_data_read_str(io, length);
      break;
    default:
      return fiobj_data_read_file(io, length);
  }
}
/* *****************************************************************************
Position / Seeking
***************************************************************************** */

/**
 * Moves the reading position to the requested position.
 */
void fiobj_data_seek(FIOBJ io, intptr_t position) {
  if (!io || !FIOBJ_TYPE_IS(io, FIOBJ_T_DATA)) return;
  switch (obj2io(io)->fd) {
    case -1:
      /* String code */
      if (position == 0) {
        obj2io(io)->pos = 0;
        return;
      }
      if (position > 0) {
        if ((uintptr_t)position > obj2io(io)->len) position = obj2io(io)->len;
        obj2io(io)->pos = position;
        return;
      }
      position = (0 - position);
      if ((uintptr_t)position > obj2io(io)->len)
        position = 0;
      else
        position = obj2io(io)->len - position;
      obj2io(io)->pos = position;
      return;
      break;
    default:
      /* File code */
      obj2io(io)->pos = 0;
      obj2io(io)->len = 0;

      if (position == 0) {
        obj2io(io)->source.fpos = 0;
        return;
      }
      int64_t len = fiobj_data_get_fd_size(io);
      if (len < 0) len = 0;
      if (position > 0) {
        if (position > len) position = len;

        obj2io(io)->source.fpos = position;
        return;
      }
      position = (0 - position);
      if (position > len)
        position = 0;
      else
        position = len - position;
      obj2io(io)->source.fpos = position;
      return;
  }
}
/* *****************************************************************************
Writing API
***************************************************************************** */

/**
 * Writes `length` bytes at the end of the IO stream, ignoring the reading
 * position.
 *
 * Behaves and returns the same value as the system call `write`.
 */
intptr_t fiobj_data_write(FIOBJ io, void *buffer, uintptr_t length) {
  if (!io || !FIOBJ_TYPE_IS(io, FIOBJ_T_DATA) || (!buffer && length)) {
    errno = EFAULT;
    return -1;
  }
  errno = 0;
  if (obj2io(io)->fd == -1) {
    /* String Code */
    fiobj_data_pre_write(io, length + 1);
    memcpy(obj2io(io)->buffer + obj2io(io)->len, buffer, length);
    obj2io(io)->len = obj2io(io)->len + length;
    obj2io(io)->buffer[obj2io(io)->len] = 0;
    return length;
  }

  /* File Code */
  return pwrite(obj2io(io)->fd, buffer, length, fiobj_data_get_fd_size(io));
}
