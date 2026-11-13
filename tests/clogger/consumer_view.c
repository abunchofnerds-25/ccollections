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

// Built WITHOUT the -D_FILE_OFFSET_BITS=64 of the library and the rest of the
// suite (see the Makefile), so this file sees <clogger.h> as an application
// built with default flags does.
#include "consumer_view.h"

// Fills *out with clog_rotation_cfg's size, alignment and member offsets as
// this translation unit lays the struct out, for the layout test.
void clogger_consumer_fill_view(clogger_consumer_view_t *out) {
  out->struct_size = sizeof(clog_rotation_cfg);
  out->struct_align = _Alignof(clog_rotation_cfg);
  out->off_size_rotation_enabled =
      offsetof(clog_rotation_cfg, size_rotation_enabled);
  out->off_max_file_size = offsetof(clog_rotation_cfg, max_file_size);
  out->off_time_rotation_enabled =
      offsetof(clog_rotation_cfg, time_rotation_enabled);
  out->off_rotation_interval_us =
      offsetof(clog_rotation_cfg, rotation_interval_us);
  out->off_max_rotated_files = offsetof(clog_rotation_cfg, max_rotated_files);
  out->off_compress_rotated = offsetof(clog_rotation_cfg, compress_rotated);
  out->sizeof_max_file_size = sizeof(((clog_rotation_cfg *)0)->max_file_size);
  out->sizeof_rotation_interval_us =
      sizeof(((clog_rotation_cfg *)0)->rotation_interval_us);
}

// Opens a rotating file logger with a config built here (size rotation at
// max_file_size, max_rotated_files kept), so the test can check the library
// reads a config built with default flags. Gives the handle or CLOG_NULL.
clog clogger_consumer_open_rotating(const char *path, int64_t max_file_size,
                                    int max_rotated_files) {
  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = max_file_size,
      .time_rotation_enabled = false,
      .rotation_interval_us = 0,
      .max_rotated_files = max_rotated_files,
      .compress_rotated = false,
  };
  return clog_create_from_file_path_mp(path, CLOG_INFO, &cfg, NULL, NULL);
}
