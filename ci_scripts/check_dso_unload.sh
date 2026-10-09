#!/bin/sh
# This script checks that a process can still fork() after it loads the library
# with dlopen(), uses a module that registers pthread_atfork() handlers, and
# then unloads the library again. It also checks two properties of the
# thread-specific keys of the library. A thread that used a thread-safe
# memory pool, and that outlives the dlclose(), must exit cleanly. And the
# cycles of dlopen() and dlclose() must give back every key that they took.
#
# pthread_atfork() keeps function pointers in the process-global state of the C
# library, and POSIX gives no way to take them back. A handler that an unloaded
# shared object left registered would therefore run at the next fork() in that
# process, at an address that is no longer mapped. glibc closes this hole. It
# registers each handler against the __dso_handle of the object that calls
# pthread_atfork(). The pthread_atfork() that a shared object links comes from
# libc_nonshared.a for exactly this reason, so that handle belongs to the
# caller. glibc then takes those handlers back when it unloads that object.
# This library depends on that behavior, so this script proves it on the real
# built artifact instead of assuming it.
#
# pthread_key_create() is the same kind of process-global state. A key keeps
# the address of its destructor, and the C library calls it for every thread
# that exits with a value on that key. A module that unloads with its key still
# live leaves that address pointing at unmapped code, and the next exit of such
# a thread crashes. Each module therefore deletes its keys when it unloads. The
# probe runs a thread that armed the thread cache of a pool, unloads the
# library, and only then lets that thread exit. It also counts the keys that
# the process can still create, before the first load and after the last
# unload, and a difference fails the check.
#
# The assertion is only real when the loader truly unmaps the library before
# the fork. An unload that did not happen leaves the handlers valid, and a
# fork() that works then proves nothing. The probe reports which case it saw. A
# run that saw no unload is a failure, because the one thing that this check
# exists to prove was never tested in it. Set
# CCOL_DSO_UNLOAD_ALLOW_RESIDENT=1 for a target where the loader truly cannot
# unload the object.
#
# On macOS dyld never unloads a dylib that has thread-local variables (see
# dlclose(3) of macOS), and this library has some. A dlclose() there leaves the
# library mapped, so neither hazard above can occur, and the destructors of the
# modules do not run until the process exits. On a Mach-O library the script
# therefore accepts a library that stays mapped, and the probe compares the
# count of keys only after a real unload. The probe still loads the library,
# uses it, closes it, forks and lets a thread exit, so a crash in any of those
# steps still fails the check.
#
# Run this from the root of the repository, after `make`.
# `make check_dso_unload` and CI both use it.
set -eu

SO="${1:-}"
[ -n "$SO" ] || { echo "usage: $0 <shared-object>" >&2; exit 2; }
[ -e "$SO" ] || { echo "check_dso_unload: $SO not built; run make first" >&2; exit 2; }

# The thread-local block of the library has a budget. A library that uses
# the initial-exec model anywhere carries the STATIC_TLS flag, and a dlopen()
# must then fit its whole thread-local block into a reserve that the loader
# shares among every library that a process loads late: about 1.6 KiB with
# glibc, and on FreeBSD 128 bytes unless LD_STATIC_TLS_EXTRA raises it, which
# a FreeBSD program that loads this library with dlopen() must do (the probe
# below does; see include/internal/ctlsmodel.h). 512 bytes leaves most of the
# glibc reserve to the other libraries of a process.
# dyld gives the thread-local variables of a dylib to each thread when the
# thread first uses them, and keeps no static reserve, so a Mach-O library has
# no such budget.
. ci_scripts/object_format.sh
if ! FORMAT=$(ccol_object_format "$SO"); then
  echo "check_dso_unload: $SO is neither an ELF nor a Mach-O file" >&2
  exit 2
fi
if [ "$FORMAT" = elf ]; then
  . ci_scripts/gnu_binutils.sh
  TLS_BUDGET=512
  if ! phdrs=$("$READELF" -lW "$SO"); then
    echo "check_dso_unload: could not read the program headers of $SO" >&2
    exit 2
  fi
  tls_memsz=$(printf '%s\n' "$phdrs" | awk '$1=="TLS"{print $6}')
  if [ -n "$tls_memsz" ]; then
    tls_bytes=$(printf '%d' "$tls_memsz")
    if [ "$tls_bytes" -gt "$TLS_BUDGET" ]; then
      echo "check_dso_unload: the thread-local block of $SO is $tls_bytes bytes," >&2
      echo "                  over its budget of $TLS_BUDGET; see the comment above." >&2
      exit 1
    fi
  fi
fi

CC="${CC:-cc}"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

cat > "$tmp/probe.c" <<'PROBE'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <link.h>
#endif

#include "cmempool.h"
#include "cthreadpool.h"

/* This probe reaches the pool with dlsym and does not link against it. The
 * only reference that this process holds to the library is therefore the one
 * that dlclose() drops. */
typedef ctpool (*create_fn)(size_t, size_t, ccol_memmgmt_procs_t *, char **);
typedef void (*destroy_fn)(ctpool);
typedef ccol_mempool *(*mp_create_fn)(size_t, size_t, bool, bool,
                                      ccol_memmgmt_procs_t *, char **);
typedef void *(*mp_alloc_fn)(ccol_mempool *);
typedef void (*mp_free_fn)(ccol_mempool *, void *);
typedef void (*mp_destroy_fn)(ccol_mempool *);

/* The number of keys that this process can still create. It creates keys until
 * the C library refuses, and deletes them all again. */
static int free_key_count(void) {
  static pthread_key_t keys[65536];
  int n = 0, i;
  while (n < (int)(sizeof(keys) / sizeof(keys[0])) &&
         pthread_key_create(&keys[n], NULL) == 0)
    n++;
  for (i = 0; i < n; i++) pthread_key_delete(keys[i]);
  return n;
}

static mp_alloc_fn mp_alloc;
static mp_free_fn mp_free;
static ccol_mempool *mp_pool;
/* Two pipes carry the two signals between the threads. macOS has no unnamed
 * POSIX semaphores (sem_init fails with ENOSYS there). */
static int pool_used[2], lib_unloaded[2];

static void signal_fd(int fd) {
  char c = 1;
  while (write(fd, &c, 1) != 1) {
  }
}

static void wait_fd(int fd) {
  char c;
  while (read(fd, &c, 1) != 1) {
  }
}

/* This thread takes an entry from a thread-safe pool and gives it back. That
 * builds a magazine and sets the value of the key of the pool for this thread.
 * It then waits until the library is gone, and exits. */
static void *pool_user(void *arg) {
  void *e;
  (void)arg;
  e = mp_alloc(mp_pool);
  if (e) mp_free(mp_pool, e);
  signal_fd(pool_used[1]);
  wait_fd(lib_unloaded[0]);
  return NULL;
}

#if defined(__APPLE__)
/* The images that dyld still holds. dyld unmaps an image when it drops it from
 * this list. macOS has no /proc. */
static int mapped_count(const char *needle) {
  uint32_t i, count = _dyld_image_count();
  int n = 0;
  for (i = 0; i < count; i++) {
    const char *name = _dyld_get_image_name(i);
    if (name && strstr(name, needle)) n++;
  }
  return n;
}
#else
struct loaded_query {
  const char *needle;
  int n;
};

static int count_loaded(struct dl_phdr_info *info, size_t size, void *arg) {
  struct loaded_query *q = arg;
  (void)size;
  if (info->dlpi_name && strstr(info->dlpi_name, q->needle)) q->n++;
  return 0;
}

/* The mappings of the process that name needle, from /proc/self/maps where
 * the system mounts it. Elsewhere (FreeBSD has no /proc by default), the
 * objects that the dynamic loader still holds, from dl_iterate_phdr(3): the
 * loader unmaps an object when it drops it from that list. */
static int mapped_count(const char *needle) {
  FILE *m = fopen("/proc/self/maps", "r");
  char line[4096];
  int n = 0;
  if (!m) {
    struct loaded_query q = {needle, 0};
    dl_iterate_phdr(count_loaded, &q);
    return q.n;
  }
  while (fgets(line, sizeof(line), m))
    if (strstr(line, needle)) n++;
  fclose(m);
  return n;
}
#endif

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "./libccollections.so";
  const char *needle = "libccollections";
  int cycles = 3, i;
  int keys_before = free_key_count();

  for (i = 0; i < cycles; i++) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    char *err = NULL;
    ctpool p;
    create_fn create;
    destroy_fn destroy;

    if (!h) {
      fprintf(stderr, "dlopen(%s) failed: %s\n", path, dlerror());
      return 2;
    }
    create = (create_fn)(unsigned long)dlsym(h, "ccol_create_cthread_pool_mp");
    destroy = (destroy_fn)(unsigned long)dlsym(h, "__ctpool_destroy");
    if (!create || !destroy) {
      fprintf(stderr, "dlsym failed: %s\n", dlerror());
      return 2;
    }
    /* The module registers its atfork handlers when it creates a pool. An
     * unload with no handler registered would prove nothing. */
    p = create(2, 0, NULL, &err);
    if (p == CTPOOL_INVALID) {
      fprintf(stderr, "pool creation failed: %s\n", err ? err : "(no detail)");
      return 2;
    }
    destroy(p);
    if (dlclose(h) != 0) {
      fprintf(stderr, "dlclose failed: %s\n", dlerror());
      return 2;
    }
  }

  /* A thread that used a thread-safe pool outlives the dlclose(). */
  {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    mp_create_fn mp_create;
    mp_destroy_fn mp_destroy;
    pthread_t t;
    char *err = NULL;
    int still;

    if (!h) {
      fprintf(stderr, "dlopen(%s) failed: %s\n", path, dlerror());
      return 2;
    }
    mp_create = (mp_create_fn)(unsigned long)dlsym(h, "ccol_mempool_create");
    mp_alloc = (mp_alloc_fn)(unsigned long)dlsym(h, "ccol_mempool_alloc_entry");
    mp_free = (mp_free_fn)(unsigned long)dlsym(h, "_ccol_mempool_free_entry");
    mp_destroy = (mp_destroy_fn)(unsigned long)dlsym(h, "_ccol_mempool_destroy");
    if (!mp_create || !mp_alloc || !mp_free || !mp_destroy) {
      fprintf(stderr, "dlsym failed: %s\n", dlerror());
      return 2;
    }
    /* 64 entries is large enough for the pool to give each thread a cache. */
    mp_pool = mp_create(64, 32, false, false, NULL, &err);
    if (!mp_pool) {
      fprintf(stderr, "mempool creation failed: %s\n", err ? err : "(no detail)");
      return 2;
    }
    if (pipe(pool_used) != 0 || pipe(lib_unloaded) != 0) {
      perror("pipe");
      return 2;
    }
    if (pthread_create(&t, NULL, pool_user, NULL) != 0) {
      fprintf(stderr, "pthread_create failed\n");
      return 2;
    }
    wait_fd(pool_used[0]);
    mp_destroy(mp_pool);
    if (dlclose(h) != 0) {
      fprintf(stderr, "dlclose failed: %s\n", dlerror());
      signal_fd(lib_unloaded[1]);
      pthread_join(t, NULL);
      return 2;
    }
    still = mapped_count(needle);
    if (still > 0) printf("NOT-UNLOADED %d\n", still);
    fflush(stdout);
    /* The thread exits here, after the unload. A key of the pool that is still
     * live makes that exit call into unmapped code. */
    signal_fd(lib_unloaded[1]);
    pthread_join(t, NULL);
    printf("THREAD-EXIT-OK\n");
  }

  /* The destructors of the modules delete their keys when the library
   * unloads. A library that stayed mapped has not run them, so the count is
   * compared only after a real unload. */
  if (mapped_count(needle) == 0) {
    int keys_after = free_key_count();
    if (keys_after != keys_before) {
      fprintf(stderr, "%d thread-specific keys stayed allocated after the "
                      "last dlclose\n", keys_before - keys_after);
      return 1;
    }
    printf("KEYS-OK\n");
  }

  {
    int still = mapped_count(needle);
    pid_t pid;
    int st = 0;

    if (still < 0) {
      fprintf(stderr, "could not read the mappings of the process\n");
      return 2;
    }
    /* The probe reports both results, because the two mean different things.
     * A log that shows them as one cannot tell you whether the test used the
     * unload path at all. */
    if (still > 0)
      printf("NOT-UNLOADED %d\n", still);
    else
      printf("UNLOADED\n");

    fflush(stdout);
    pid = fork();
    if (pid < 0) {
      perror("fork");
      return 2;
    }
    if (pid == 0) _exit(42);
    if (waitpid(pid, &st, 0) < 0) {
      perror("waitpid");
      return 2;
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 42) {
      fprintf(stderr, "child did not exit cleanly\n");
      return 1;
    }
  }
  printf("FORK-OK\n");
  return 0;
}
PROBE

if ! $CC -Iinclude -std=gnu11 -g -O1 -pthread -o "$tmp/probe" "$tmp/probe.c" \
     2>"$tmp/cc.err"; then
  echo "check_dso_unload: could not compile the probe." >&2
  sed 's/^/  /' "$tmp/cc.err" >&2
  exit 2
fi

# The script finds the library through its own directory and not through the
# search path of the loader. It therefore tests the artifact that you just
# built, and not one that is already installed on the machine.
case "$SO" in
/*) so_path="$SO" ;;
*)  so_path="$PWD/$SO" ;;
esac

# The script saves the status of the probe before anything else reads it. A
# crash at fork() is the failure that this check exists to catch, and it
# appears as a signal and not as output.
# LD_STATIC_TLS_EXTRA is the room that FreeBSD's loader keeps for the
# initial-exec thread-local block of a late-loaded object; glibc ignores it.
set +e
out=$(LD_STATIC_TLS_EXTRA=512 "$tmp/probe" "$so_path" 2>"$tmp/probe.err")
rc=$?
set -e

if [ "$rc" -ne 0 ]; then
  echo "check_dso_unload: FAILED (probe exited $rc)." >&2
  [ "$rc" -gt 128 ] && echo "                  Terminated by signal $((rc - 128)); a crash here is a" >&2
  [ "$rc" -gt 128 ] && echo "                  fork() dispatching into an unloaded object, or a thread" >&2
  [ "$rc" -gt 128 ] && echo "                  exit calling a key destructor of an unloaded object." >&2
  sed 's/^/  /' "$tmp/probe.err" >&2
  printf '%s\n' "$out" | sed 's/^/  /' >&2
  exit 1
fi

case "$out" in
*UNLOADED*)
  if printf '%s\n' "$out" | grep -q '^NOT-UNLOADED'; then
    maps=$(printf '%s' "$out" | sed -n 's/^NOT-UNLOADED \([0-9]*\).*/\1/p')
    if [ "$FORMAT" = macho ]; then
      echo "check_dso_unload: accepted, dyld never unloads a dylib that has"
      echo "                  thread-local variables (dlclose(3) of macOS). The"
      echo "                  load, use, close, fork() and thread exit worked."
      exit 0
    fi
    echo "check_dso_unload: the library stayed mapped after dlclose ($maps" >&2
    echo "                  mappings), so the loader is keeping it resident." >&2
    echo "                  A fork() that works in that state proves nothing:" >&2
    echo "                  the handlers are still valid because the object" >&2
    echo "                  they live in was never unmapped, so the" >&2
    echo "                  unregistration path this exists to assert was not" >&2
    echo "                  exercised at all." >&2
    echo "                  An object can become resident for reasons that are" >&2
    echo "                  nobody's mistake (initial-exec TLS makes the loader" >&2
    echo "                  mark it NODELETE), and this library is meant to be" >&2
    echo "                  unloadable, so the default is to fail rather than" >&2
    echo "                  to report a pass nothing stands behind. Set" >&2
    echo "                  CCOL_DSO_UNLOAD_ALLOW_RESIDENT=1 on a target where" >&2
    echo "                  residency is genuinely expected; the run then" >&2
    echo "                  reports that it observed no unload." >&2
    if [ "${CCOL_DSO_UNLOAD_ALLOW_RESIDENT:-0}" = 1 ]; then
      echo "check_dso_unload: accepted anyway, CCOL_DSO_UNLOAD_ALLOW_RESIDENT=1."
      exit 0
    fi
    exit 1
  fi
  echo "check_dso_unload: OK (library unmapped after dlclose, fork() and thread exit still work, no key left behind)"
  ;;
*)
  echo "check_dso_unload: the probe did not report an outcome; treating that" >&2
  echo "                  as a failure rather than a pass." >&2
  exit 1
  ;;
esac
exit 0
