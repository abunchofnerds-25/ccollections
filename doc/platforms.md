# Platforms

This page is about the systems that c_collections builds and runs on, the
compilers and language modes it needs, and the few things that change when a
program loads the library at run time with `dlopen()`.

## At a glance

| | Supported | Notes |
|---|---|---|
| Linux | yes | glibc and musl (Alpine); built and tested in CI |
| FreeBSD | 14 and later | base C library; built and tested in CI |
| macOS | 15 and later | Apple clang; built and tested in CI on arm64 and x86-64 |
| OpenBSD | 7.9 and later | base C library and LibreSSL; built and tested in CI |
| Windows | no | lacks the GNU extensions and the system calls |
| x86-64, i386, AArch64, ARM32 (armhf), s390x | yes | AArch64, ARM32 and s390x are tested in CI under emulation; s390x is big-endian |
| GCC 10 and later, Clang 11 and later | yes | CI tests both on Linux, FreeBSD and OpenBSD, GCC 10 and Clang 11 on Debian 11, and Apple clang on macOS |
| MSVC | no | lacks the GNU extensions that the macros need |

## Operating systems

CI builds and tests Linux, FreeBSD, macOS and OpenBSD, and the oldest
releases it tests are FreeBSD 14, macOS 15 and OpenBSD 7.9. A FreeBSD
release before 13 can't work at all, because the library needs `eventfd(2)`
there.

The reactor behind `cthreadcomm`, `chttpclient` and `chttpserver` uses
`epoll(7)` on Linux and `kqueue(2)` on FreeBSD, macOS and OpenBSD, and it
behaves the same on every system, level-triggered reports, one-shot re-arms
and the delivery of errors included. Where a man page names an `epoll` call
or flag, the BSD and macOS builds use the `kqueue` equivalent.

On FreeBSD, OpenBSD and macOS you build with `gmake` (see
[Building](building.md)). On macOS Homebrew gives you GNU make
(`brew install make`) and OpenSSL (`brew install openssl@3`), and since
Homebrew's OpenSSL isn't in the paths that clang searches, you pass its
directories to the build:

```bash
ssl="$(brew --prefix openssl@3)"
CPATH="$ssl/include" LIBRARY_PATH="$ssl/lib" gmake
```

On macOS the build makes `libccollections.dylib`. The library behaves the
same as on the other systems, with one addition about `fork()` and
`posix_spawn()` that [Concurrency](concurrency.md#descriptors-and-exec)
describes.

The TLS layer works with OpenSSL 1.1.1 and every later release, and with
the LibreSSL of OpenBSD 7.9 and later. On Linux, FreeBSD and macOS the build
links whatever OpenSSL the compiler finds.

On OpenBSD the library uses the LibreSSL of the base system. LibreSSL can't
resume a TLS 1.3 session, so every connection there that negotiates TLS 1.3
makes a full handshake, while a TLS 1.2 session resumes like it does on the
other systems.

On OpenBSD, a child that `fork()` creates while the parent runs several
threads must not start threads of its own and then fork again. OpenBSD's C
library deadlocks on that second `fork()` as soon as any fork handler is
registered, and this library registers some. Forking before you create a
handle, or forking and then calling `exec()`, works there like on every
other system (see [Concurrency](concurrency.md#fork)).

OpenBSD also runs the exit handlers that a constructor registered with
`atexit()` only after the dynamic loader has run the destructors of every
shared object, and from then until the process ends the loader holds a lock.
Every other thread that joins a thread needs that lock, so such a handler
must not wait for a background thread of the library, and
`chttpsvr_engine_wait()` waits for ever there. Destroying a server, a pool
or a logger in such a handler works, because the exiting thread holds the
lock itself. Handlers that `main()` registers aren't affected.

## C libraries

The supported C libraries are glibc and musl on Linux, the base C
libraries of FreeBSD and OpenBSD, and the system C library on macOS. Other
C libraries aren't supported and CI doesn't test them.

CI tests musl on Alpine Linux, which is the usual base image for
containers, and a few things are different there. musl has no
`backtrace()`, so the records of `clog_error`, `clog_alert` and `clog_fatal`
carry the "backtrace unavailable" marker instead of frames.

musl also refuses to `dlopen()` a library that uses the initial-exec
thread-local model, so the library uses the general model there (see
[Loading the library with dlopen](#loading-the-library-with-dlopen)). That
makes the thread-safe path of `cmempool` about 1.45 times slower than with
initial-exec, but it is much faster than musl's own `malloc`: about
0.4 of its time for a 64-byte block on one thread, and under a hundredth of
it with several threads, where musl's allocator contends on one lock. So on
musl the pool is well worth using even with the slower model. A
program that links the library at build time and never loads it with
`dlopen()` can build it with `CCOL_MEMPOOL_DYNAMIC_TLS=0` and get the fast
model back.

Finally, musl's `dlclose()` never unloads a library, so the library and its
handlers stay until the process exits.

## Architectures

CI builds and runs every test suite on x86-64, i386 (a 32-bit target),
AArch64, ARM32 (armhf) and s390x, the last three under emulation. 64-bit and
32-bit targets are both supported, and the library is correct with a 32-bit
`long`, a 32-bit pointer and a `long double` whose size changes from one
platform to the next. s390x is big-endian, so it also shows that nothing in
the library depends on byte order. The hashes read their input as
little-endian words on every target, as their specifications define, and
give the same values everywhere.

## Compilers and language mode

The library supports GCC 10 and later, Clang 11 and later, and Apple
clang on macOS. There are two separate requirements, one for code that
uses the macros and one for code that only includes a header.

Code that uses the typed macros needs `-std=gnu11` (or a later `gnu` mode).
The macros use C11 plus three GNU extensions that GCC and Clang both have,
`__typeof__`, statement expressions and `__attribute__((cleanup(...)))`, and
there is no fallback implementation, so `-std=c11 -pedantic` rejects code
that uses them.

A file that only includes a header needs less. Every installed header
compiles on its own under `-std=c11 -pedantic-errors -Wall -Wextra -Werror`
with GCC and with Clang, so one of your headers can include one of ours
without forcing `gnu11` on every file that includes yours. Only the files
that call the typed macros need it.

You can nest the typed macros under `-Wshadow -Werror` as well, so
expressions like `chmap_insert(m, k, chmap_get(m, k) + 1)` or
`cvec_at(v, cvec_at(idx, i))` compile without warnings.

The library itself is always built with `-std=gnu11` and not with the
compiler's default mode, because the mode changes the meaning of `bool` and
`static_assert` and we don't want that to depend on the build machine.

### Compiler versions

The headers, the library and the test suites build with GCC 10 and Clang 11,
and anything older isn't supported. CI builds with the GCC and Clang of its
runner images on Linux, FreeBSD and OpenBSD, with Apple clang on macOS, and
with GCC 10 and Clang 11 on Debian 11. The hardening flags of the build only
take full effect with newer compilers: `-D_FORTIFY_SOURCE=3` gives level 3
with GCC 12 or later or Clang 9 or later (older compilers get level 2), and
`-fstack-clash-protection` needs Clang 11.

The build tries each compiler flag before it uses it and drops a flag that
the compiler doesn't support for the target, so an older compiler can
build the library with whatever hardening it has. `make hardening_report`
shows which flags the build uses. The source code doesn't depend on these
flags.

## Loading the library with dlopen

If your program links with `libccollections.so` the usual way, you can
skip this section.

The library has several fast paths per thread: the per-thread cache of
`cmempool`, a per-thread slot through which the library finds the object
behind a `clogger`, `clrucache`, `cthreadpool` or `chttpclient` handle, the
date cache of `chttpserver` and the per-thread state of `clogger`.

With glibc and on FreeBSD these fast paths use the *initial-exec*
thread-local storage model, which saves one function call on every access.
The catch is that a `dlopen()` of the library has to place its thread-local
block (a few hundred bytes) in a small reserve that the dynamic loader keeps
for libraries loaded late, and all such libraries share it. glibc keeps
about 1.6 KiB, so a `dlopen()` only fails when other late-loaded libraries
have already used the reserve up. FreeBSD keeps 128 bytes and the load fails
with "No space available for static Thread Local Storage". On FreeBSD you
start such a program with `LD_STATIC_TLS_EXTRA=512` in its environment,
which the loader reads at start-up (this library needs at least 384):

```bash
LD_STATIC_TLS_EXTRA=512 ./my_plugin_host
```

If you can't control the environment, build the library with the general
model:

```bash
make EXTRA_CFLAGS="-DCCOL_MEMPOOL_DYNAMIC_TLS=1"
```

With this model `dlopen()` always works. Most of the cost is on the
thread-safe path of `cmempool`, which gets between one third and two thirds
slower on Linux and up to 1.9 times slower on FreeBSD, and at that point it
is slower than `malloc` itself. That is too much to pay by default, which is
why we keep initial-exec where the system allows it and leave the general
model for programs that really have to load the library late. On C libraries
other than glibc and the FreeBSD one, the general model is the default.

macOS has no static reserve for thread-local storage, and on OpenBSD the
compiler emulates thread-local storage, so on both of them a `dlopen()` of
the library always works and none of this section applies.

## Unloading the library with dlclose

A process can unload the library with `dlclose()`, keep running and then
call `fork()`. When the library unloads, the system removes the
`pthread_atfork()` handlers and the thread-specific keys that some modules
register, and a thread that keeps running after the unload exits correctly.
CI checks all of this on the built library with `make check_dso_unload`.

Two things are up to you. You have to destroy every handle before you
unload, because a thread pool, logger, event loop or HTTP client whose
threads are running can't keep working once its code is gone (that is true
of any shared library). And if the memory they hold matters to you, join
the threads that used the library. Some modules keep state for each thread
and free it when that thread exits: the cache of a thread-safe `cmempool`
pool, the node pools of `cjson` and `cyaml`, the error buffer of `cjson` and
the thread identity that `clogger` writes into each record. Once the code
is unloaded the library can't free that state for a thread that keeps
running, so it stays allocated until the process exits.

On OpenBSD the compiler emulates thread-local storage with a pthread key
for each loaded object that has thread-local variables, and an unload
doesn't delete that key. So each load of the library takes one key for the
rest of the process out of the 512 that OpenBSD allows, and a program that
loads and unloads the library hundreds of times runs out of keys there.

On macOS `dlclose()` never unloads the library, because the dynamic loader
never unloads a library that has thread-local variables (see the macOS
`dlclose(3)`). The library and its handlers stay until the process exits,
but you must still destroy every handle before the `dlclose()`.

## Reference

[ccollections(7)](../man/common/ccollections.7) (compile-time switches)

Related guides:
[Building and linking](building.md),
[How the library works](design.md),
[Testing, fuzzing and benchmarks](testing.md),
[cmempool](cmempool.md)
