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

/* The open file descriptors of the test process, on every supported system.
 * Linux lists them in /proc/self/fd, which is fast whatever the descriptor
 * limit is. The BSDs and macOS have no such listing by default, so there
 * every descriptor number up to the process limit (at most 65536) is probed
 * with fcntl(F_GETFD). Neither form counts a descriptor of its own. */

#ifndef CCOL_TESTS_TEST_FDS_H
#define CCOL_TESTS_TEST_FDS_H

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

/* Calls fn(fd, arg) once for every open descriptor. */
static inline void test_for_each_open_fd(void (*fn)(int fd, void *arg),
                                         void *arg) {
  DIR *d = opendir("/proc/self/fd");
  if (d) {
    int own = dirfd(d);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      if (e->d_name[0] == '.') continue;
      int fd = atoi(e->d_name);
      if (fd != own) fn(fd, arg);
    }
    closedir(d);
    return;
  }
  long limit = sysconf(_SC_OPEN_MAX);
  if (limit < 0 || limit > 65536) limit = 65536;
  for (int fd = 0; fd < (int)limit; fd++) {
    if (fcntl(fd, F_GETFD) != -1) fn(fd, arg);
  }
}

static inline void test_fds_count_one(int fd, void *arg) {
  (void)fd;
  (*(int *)arg)++;
}

/* The number of open descriptors. */
static inline int test_count_open_fds(void) {
  int n = 0;
  test_for_each_open_fd(test_fds_count_one, &n);
  return n;
}

#endif /* CCOL_TESTS_TEST_FDS_H */
