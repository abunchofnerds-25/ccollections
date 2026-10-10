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

#ifndef CLOGGER_TESTS_CONSUMER_VIEW_H
#define CLOGGER_TESTS_CONSUMER_VIEW_H

#include <clogger.h>
#include <stddef.h>

/*
 * This holds how an application translation unit, compiled with its own
 * ordinary flags, lays out clog_rotation_cfg_t.
 *
 * The build makes consumer_view.c WITHOUT the -D_FILE_OFFSET_BITS=64 that
 * the library and the rest of this suite use, which is exactly the situation
 * of a program that includes <clogger.h> and compiles with default flags.
 * The two views must describe the same object: otherwise the library reads a
 * public struct whose layout follows a feature-test macro member by member
 * from the wrong offsets, and the logger silently rotates on a schedule that
 * the caller never asked for and turns on options that the caller never
 * set.
 */
typedef struct {
  size_t struct_size;
  size_t struct_align;
  size_t off_size_rotation_enabled;
  size_t off_max_file_size;
  size_t off_time_rotation_enabled;
  size_t off_rotation_interval_us;
  size_t off_max_rotated_files;
  size_t off_compress_rotated;
  size_t sizeof_max_file_size;
  size_t sizeof_rotation_interval_us;
} clogger_consumer_view_t;

/* This fills *out with the view of the struct that this translation unit
 * has. */
void clogger_consumer_fill_view(clogger_consumer_view_t *out);

/*
 * This opens a rotating file logger whose clog_rotation_cfg_t comes
 * entirely from this translation unit. That config sets size rotation at
 * max_file_size bytes, no time rotation, max_rotated_files files kept, and no
 * compression. The function returns CLOG_INVALID after a failure, exactly as
 * clog_open_file_mp() does.
 */
clog clogger_consumer_open_rotating(const char *path, int64_t max_file_size,
                                    int max_rotated_files);

#endif /* CLOGGER_TESTS_CONSUMER_VIEW_H */
