/*
Copyright: Boaz Segev, 2018-2019
License: MIT
*/
#ifndef H_FIO_TMPFILE_H
/** a simple helper to create temporary files and file names */
#define H_FIO_TMPFILE_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static inline int fio_tmpfile(void) {
  // create a temporary file to contain the data.
  int fd = 0;
#ifdef P_tmpdir
  if (P_tmpdir[sizeof(P_tmpdir) - 1] == '/') {
    char name_template[] = P_tmpdir "facil_io_tmpfile_XXXXXXXX";
    fd = mkstemp(name_template);
    if (fd != -1) unlink(name_template);
  } else {
    char name_template[] = P_tmpdir "/facil_io_tmpfile_XXXXXXXX";
    fd = mkstemp(name_template);
    if (fd != -1) unlink(name_template);
  }
#else
  char name_template[] = "/tmp/facil_io_tmpfile_XXXXXXXX";
  fd = mkstemp(name_template);
  if (fd != -1) unlink(name_template);
#endif
  /* unlink()ed immediately (classic "delete on close" pattern): the file's
   * data stays fully accessible through this fd (read/write/seek/pread all
   * still work) until the fd is closed, but the directory entry disappears
   * right away, so the backing disk space is automatically reclaimed once
   * the last fd referencing it closes - including on process crash, unlike
   * relying on an explicit unlink() call somewhere in this object's normal
   * teardown path. Without this, every large HTTP request body that spills
   * to a temp file (see fiobj_data_newtmpfile, used by http1.c for any body
   * whose Content-Length exceeds HTTP_MAX_HEADER_LENGTH) left a permanent
   * orphan file on disk - unbounded disk exhaustion on ordinary, legitimate
   * traffic to a server handling large uploads. */
  return fd;
}
#endif
