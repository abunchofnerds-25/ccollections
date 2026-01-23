/*
Copyright: Boaz Segev, 2017-2019
License: MIT
*/
#if !defined(H_FIOBJ_IO_H) && (defined(__unix__) || defined(__APPLE__) || \
                               defined(__linux__) || defined(__CYGWIN__))

/**
 * A dynamic type for reading / writing to a local file, a temporary file or an
 * in-memory string.
 *
 * Supports basic read, write and seek operations.
 *
 * Writing is always performed at the end of the stream / memory buffer,
 * ignoring the current seek position.
 */
#define H_FIOBJ_IO_H

#include <fiobject.h>

#ifdef __cplusplus
extern "C" {
#endif

/* *****************************************************************************
Creating the Data Stream object
***************************************************************************** */

/** Creates a new local in-memory Data Stream object */
FIOBJ fiobj_data_newstr(void);

/** Creates a new local tempfile Data Stream object */
FIOBJ fiobj_data_newtmpfile(void);

/** Creates a new local file Data Stream object */
FIOBJ fiobj_data_newfd(int fd);

/* *****************************************************************************
Reading API
***************************************************************************** */

/**
 * Reads up to `length` bytes and returns a temporary(!) buffer object (not NUL
 * terminated).
 *
 * If `length` is zero or negative, it will be computed from the end of the
 * input backwards (0 == EOF).
 *
 * The C string object will be invalidate the next time a function call to the
 * Data Stream object is made.
 */
fio_str_info_s fiobj_data_read(FIOBJ io, intptr_t length);

/**
 * Moves the reading position to the requested position.
 */
void fiobj_data_seek(FIOBJ io, intptr_t position);

/* *****************************************************************************
Writing API
***************************************************************************** */

/**
 * Writes `length` bytes at the end of the Data Stream stream, ignoring the
 * reading position.
 *
 * Behaves and returns the same value as the system call `write`.
 */
intptr_t fiobj_data_write(FIOBJ io, void *buffer, uintptr_t length);

#if DEBUG
void fiobj_data_test(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
