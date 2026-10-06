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

/*
 * The build makes this file WITHOUT -D_FILE_OFFSET_BITS=64. See the Makefile
 * of this directory. Every other translation unit in this suite uses that
 * flag, and so does the library itself. This file therefore sees
 * <clogger.h> the way an application with ordinary default flags sees it.
 */

#include "consumer_view.h"

void clogger_consumer_fill_view(clogger_consumer_view_t *out) {
  out->struct_size = sizeof(clog_rotation_cfg_t);
  out->struct_align = _Alignof(clog_rotation_cfg_t);
  out->off_size_rotation_enabled =
      offsetof(clog_rotation_cfg_t, size_rotation_enabled);
  out->off_max_file_size = offsetof(clog_rotation_cfg_t, max_file_size);
  out->off_time_rotation_enabled =
      offsetof(clog_rotation_cfg_t, time_rotation_enabled);
  out->off_rotation_interval_us =
      offsetof(clog_rotation_cfg_t, rotation_interval_us);
  out->off_max_rotated_files = offsetof(clog_rotation_cfg_t, max_rotated_files);
  out->off_compress_rotated = offsetof(clog_rotation_cfg_t, compress_rotated);
  out->sizeof_max_file_size = sizeof(((clog_rotation_cfg_t *)0)->max_file_size);
  out->sizeof_rotation_interval_us =
      sizeof(((clog_rotation_cfg_t *)0)->rotation_interval_us);
}

clog clogger_consumer_open_rotating(const char *path, int64_t max_file_size,
                                    int max_rotated_files) {
  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = max_file_size,
      .time_rotation_enabled = false,
      .rotation_interval_us = 0,
      .max_rotated_files = max_rotated_files,
      .compress_rotated = false,
  };
  return clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
}
