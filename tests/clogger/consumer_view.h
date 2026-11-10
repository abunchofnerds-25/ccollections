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

// The size, alignment and member offsets of clog_rotation_cfg as one
// translation unit sees it. consumer_view.c fills it without the
// -D_FILE_OFFSET_BITS=64 the library uses, and the test compares it with the
// library's own view: if the two differ, the library reads a caller's config
// from the wrong offsets.
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

// Fills *out with clog_rotation_cfg's layout as consumer_view.c sees it, so the
// test can compare it with the suite's view.
void clogger_consumer_fill_view(clogger_consumer_view_t *out);

// Opens a file logger at path with a rotation config built entirely in
// consumer_view.c (size rotation at max_file_size bytes, max_rotated_files
// kept, no time rotation, no compression), so the test can check that the
// library reads such a config right. Gives the handle or CLOG_NULL.
clog clogger_consumer_open_rotating(const char *path, int64_t max_file_size,
                                    int max_rotated_files);

#endif
