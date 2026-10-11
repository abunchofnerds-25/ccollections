# Platforms

This page covers:

- the systems on which c_collections builds and runs
- the compilers and language modes it needs
- the few things that change when a program loads the library at run time
  with `dlopen()`

## At a glance

| | Supported | Notes |
|---|---|---|
| Linux | yes | glibc and musl (Alpine); built and tested in CI |
| FreeBSD | 14 and later | base C library; built and tested in CI |
| macOS | 15 and later | Apple clang; built and tested in CI on arm64 and x86-64 |
| OpenBSD | 7.9 and later | base C library and LibreSSL; built and tested in CI |
| Windows | no | lacks the GNU extensions and the system calls |
| x86-64, i386, AArch64, ARM32 (armhf), s390x | yes | AArch64, ARM32 and s390x are tested in CI under emulation; s390x is big-endian |
| GCC, Clang | yes | CI tests both on Linux, FreeBSD and OpenBSD, and Apple clang on macOS |
| MSVC | no | lacks the GNU extensions that the macros need |

## Operating systems

**Linux, FreeBSD, macOS and OpenBSD.** CI builds and tests all four, and
FreeBSD 14, macOS 15 and OpenBSD 7.9 are the oldest releases it tests. No
FreeBSD release before 13 can work, because the library needs `eventfd(2)`
there.

The reactor behind `cthreadcomm`, `chttpclient` and `chttpserver` uses
`epoll(7)` on Linux and `kqueue(2)` on FreeBSD, macOS and OpenBSD, with the
same behavior on every system, including level-triggered reports, one-shot
re-arms and the delivery of errors. Where a man page names an `epoll` call or
flag, the BSD and macOS builds use the `kqueue` equivalent.

On FreeBSD, OpenBSD and macOS, build with `gmake` (see
[Building](building.md)). On
macOS, Homebrew provides GNU make (`brew install make`) and OpenSSL
(`brew install openssl@3`). Because Homebrew's OpenSSL lies outside the paths
that clang searches, pass its directories to the build:

```bash
ssl="$(brew --prefix openssl@3)"
CPATH="$ssl/include" LIBRARY_PATH="$ssl/lib" gmake
```

On macOS the build produces `libccollections.dylib`. The library behaves as
it does on the other systems, with one addition about `fork()` and
`posix_spawn()` that [Concurrency](concurrency.md#descriptors-and-exec)
describes.

On OpenBSD the library uses the LibreSSL of the base system. LibreSSL cannot
resume a TLS 1.3 session, so there every connection that negotiates TLS 1.3
makes a full handshake; a TLS 1.2 session resumes as it does on the other
systems.

OpenBSD also runs the exit handlers that a constructor registered (with
`atexit()`) after the dynamic loader has run the destructors of every shared
object, and the loader keeps a lock from then until the process ends. Any
other thread that joins a thread needs that lock, so such a handler must not
wait for a background thread of the library: `chttpsvr_engine_wait()` there
waits for ever. Destroying a server, a pool or a logger there works, because
the exiting thread holds the lock itself. Handlers that `main()` registers
are not affected.

Windows is not supported.

## C libraries

The supported C libraries are glibc and musl on Linux, the base C libraries
of FreeBSD and OpenBSD, and the system C library on macOS. Other C libraries
are not supported and are not tested in CI.

CI tests musl on Alpine Linux, the usual base image of containers. Three
things differ there:

- musl has no `backtrace()`, so the records of `clog_error`, `clog_alert`
  and `clog_fatal` carry the "backtrace unavailable" marker instead of
  frames.
- musl refuses to `dlopen()` a library that uses the initial-exec
  thread-local model, so the library uses the general model there (see
  [Loading the library with dlopen](#loading-the-library-with-dlopen)).
  That makes the thread-safe path of `cmempool` about 1.45 times slower than
  with the initial-exec model, while it stays far faster than musl's own
  `malloc`: about 0.4 of its time for a 64-byte block on one thread, and
  under a hundredth of it with several threads, where musl's allocator
  contends on one lock. A program that links the library at build time and
  never loads it with `dlopen()` can build it with
  `CCOL_MEMPOOL_DYNAMIC_TLS=0` to get the fast model back.
- musl's `dlclose()` never unloads a library, so the library and its
  handlers stay until the process exits.

## Architectures

CI builds and runs every test suite on x86-64, i386 (a 32-bit target),
AArch64, ARM32 (armhf) and s390x, the last three under emulation. Both 64-bit
and 32-bit targets are supported: the library is correct with a 32-bit
`long`, a 32-bit pointer, and a `long double` whose size varies from platform
to platform. s390x is big-endian, so it also proves that nothing in the
library depends on byte order: the hashes read their input as little-endian
words on every target, as their specifications define, and give the same
values everywhere.

## Compilers and language mode

The library supports GCC and Clang; CI uses both on Linux, FreeBSD and
OpenBSD, and Apple clang on macOS. Two separate requirements apply.

**Code that uses the typed macros needs `-std=gnu11`** (or a later `gnu`
mode). The macros use C11 plus three GNU extensions that both GCC and Clang
provide: `__typeof__`, statement expressions and
`__attribute__((cleanup(...)))`. There is no fallback implementation, so
`-std=c11 -pedantic` rejects code that uses the macros.

**A file that only includes a header needs less.** Every installed header
compiles on its own under `-std=c11 -pedantic-errors -Wall -Wextra -Werror`
with both GCC and Clang. One of your headers can therefore include one of
ours without forcing `gnu11` on every file that includes yours; only the
files that call the typed macros need it.

The typed macros can also be nested under `-Wshadow -Werror`, so expressions
such as `chmap_insert(m, k, chmap_get(m, k) + 1)` or
`cvec_at(v, cvec_at(idx, i))` compile without warnings.

The library itself is always built with `-std=gnu11` rather than the
compiler's default mode, because the mode changes the meaning of `bool` and
`static_assert`, and that must not depend on the build machine.

### Compiler versions

CI builds with whatever GCC and Clang its runner images provide, not with
pinned versions. The build's hardening flags take full effect only with newer
compilers:

- `-D_FORTIFY_SOURCE=3` gives level 3 with GCC 12 or later, or Clang 9 or
  later; older compilers get level 2.
- `-fstack-clash-protection` needs Clang 11.

The build probes each compiler flag before using it and leaves out any flag
that the compiler does not support for the target, so an older compiler can
still build the library with whatever hardening it supports.
`make hardening_report` shows which flags the build uses. The source code
itself does not depend on these flags.

## Loading the library with dlopen

If your program links with `libccollections.so` in the usual way, you can
skip this section.

The library has several per-thread fast paths:

- the per-thread cache of `cmempool`
- a per-thread slot through which the library finds the object behind a
  `clogger`, `clrucache`, `cthreadpool` or `chttpclient` handle
- the date cache of `chttpserver`
- the per-thread state of `clogger`

With glibc and on FreeBSD, these fast paths use the *initial-exec*
thread-local storage model, which removes one function call from every
access. The cost is that a `dlopen()` of the library must place its
thread-local block (a few hundred bytes) in a small reserve that the dynamic
loader keeps for libraries loaded late, and all such libraries share that
reserve:

- **glibc** keeps about 1.6 KiB, so a `dlopen()` fails only when other
  late-loaded libraries have already used up the reserve.
- **FreeBSD** keeps 128 bytes, and the load fails with "No space available
  for static Thread Local Storage". Start such a program with
  `LD_STATIC_TLS_EXTRA=512` in its environment; the FreeBSD loader reads this
  variable at start-up, and this library needs at least 384.

```bash
LD_STATIC_TLS_EXTRA=512 ./my_plugin_host
```

If you cannot control the environment, build the library with the general
model:

```bash
make EXTRA_CFLAGS="-DCCOL_MEMPOOL_DYNAMIC_TLS=1"
```

With this model `dlopen()` always works. The cost falls mainly on the
thread-safe path of `cmempool`, which becomes between one third and two
thirds slower on Linux and up to 1.9 times slower on FreeBSD, at which point
it is slower than `malloc` itself. On C libraries other than glibc and the
FreeBSD C library, the general model is the default.

On macOS, thread-local storage has no static reserve, and on OpenBSD the
compiler emulates thread-local storage, so on both a `dlopen()` of the
library always works and none of this section applies.

## Unloading the library with dlclose

A process can unload the library with `dlclose()`, keep running, and then
call `fork()`. When the library unloads, the system removes the
`pthread_atfork()` handlers and the thread-specific keys that some modules
register, and a thread that keeps running after the unload can exit
correctly. CI checks all of this on the built library with
`make check_dso_unload`.

Two things are your responsibility:

- **Destroy every handle before you unload.** A thread pool, logger, event
  loop or HTTP client whose threads are running cannot keep working once its
  code is gone. This is true of any shared library.
- **Join the threads that used a thread-safe `cmempool` pool,** if their
  cached memory matters to you. Once the code is unloaded, the library cannot
  free the per-thread cache of a thread that keeps running, so that cache
  stays allocated until the process exits.

On OpenBSD the compiler emulates thread-local storage with a pthread key
for each loaded object that has thread-local variables, and an unload does
not delete that key. Each load of the library therefore uses one key for the
rest of the process, out of the 512 that OpenBSD allows, so a program that
loads and unloads the library hundreds of times runs out of keys there.

On macOS, `dlclose()` never unloads the library, because the dynamic loader
never unloads a library that has thread-local variables (see the macOS
`dlclose(3)`). The library and its handlers then stay until the process
exits, but you must still destroy every handle before the `dlclose()`.

## Reference

[ccollections(7)](../man/common/ccollections.7) (compile-time switches)

Related guides:
[Building and linking](building.md),
[How the library works](design.md),
[Testing, fuzzing and benchmarks](testing.md),
[cmempool](cmempool.md)
