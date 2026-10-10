# Platforms

This page tells you about these topics:

- the systems where c_collections builds and runs
- the compilers and language modes that it needs
- the small number of changes that apply when a program loads the library at
  run time with `dlopen()`

## At a glance

| | Supported | Notes |
|---|---|---|
| Linux | yes | glibc; CI builds and tests it |
| FreeBSD | 14 and later | base C library; CI builds and tests it |
| macOS | 15 and later | Apple clang; CI builds and tests it on arm64 and x86-64 |
| OpenBSD, NetBSD | no | do not build |
| Windows | no | it does not have the GNU extensions or the system calls |
| x86-64, i386, AArch64, ARM32 (armhf) | yes | CI tests AArch64 and ARM32 under emulation |
| GCC, Clang | yes | CI tests both on Linux and FreeBSD, and Apple clang on macOS |
| MSVC | no | it has none of the GNU extensions that the macros need |

## Operating systems

**Linux, FreeBSD and macOS.** CI builds and tests all three. FreeBSD 14
and macOS 15 are the oldest releases that CI tests. No FreeBSD release
before 13 can work, because the library needs `eventfd(2)` there.

The reactor of `cthreadcomm`, `chttpclient` and `chttpserver` uses
`epoll(7)` on Linux and `kqueue(2)` on FreeBSD and macOS. The behavior is
the same on all three systems. This includes level-triggered reports,
one-shot re-arms and the delivery of errors. When a man page gives the name
of an `epoll` call or flag, the FreeBSD and macOS builds use the equivalent
`kqueue` item.

On FreeBSD and macOS, build with `gmake` (see [Building](building.md)). On
macOS, Homebrew gives GNU make (`brew install make`) and OpenSSL
(`brew install openssl@3`). OpenSSL from Homebrew is outside the paths that
clang searches, so give its directories to the build:

```bash
ssl="$(brew --prefix openssl@3)"
CPATH="$ssl/include" LIBRARY_PATH="$ssl/lib" gmake
```

On macOS, the build makes `libccollections.dylib`. The library behaves as on
the other systems, with one addition about `fork()` and `posix_spawn()`
that [Concurrency](concurrency.md#descriptors-and-exec) describes.

OpenBSD and NetBSD do not build. The library does not support Windows.

## C libraries

The library supports glibc on Linux, the base C library on FreeBSD and the
system C library on macOS. It does not support other C libraries, for
example musl. CI does not test them.

## Architectures

CI builds and runs all test suites on x86-64, i386 (a 32-bit target),
AArch64 and ARM32 (armhf). The last two run under emulation. The library
supports 64-bit targets and 32-bit targets. It is correct with a 32-bit
`long`, with a 32-bit pointer, and with a `long double` whose size is
different on different platforms.

## Compilers and language mode

The library supports GCC and Clang. CI uses both on Linux and FreeBSD, and
Apple clang on macOS. Two separate requirements apply.

**Code that uses the typed macros needs `-std=gnu11`** (or a later `gnu`
mode). The macros use C11 and three GNU extensions that GCC and Clang both
have: `__typeof__`, statement expressions and
`__attribute__((cleanup(...)))`. There is no alternative implementation.
Therefore, `-std=c11 -pedantic` refuses code that uses the macros.

**A file that only includes a header needs less.** Each installed header
compiles alone under `-std=c11 -pedantic-errors -Wall -Wextra -Werror`, with
GCC and with Clang. Therefore, a header of yours can include one of our headers,
and the files that include your header do not need `gnu11`. Only the files
that call the typed macros need it.

You can also nest the typed macros under `-Wshadow -Werror`. Therefore,
expressions such as `chmap_insert(m, k, chmap_get(m, k) + 1)` or
`cvec_at(v, cvec_at(idx, i))` compile without warnings.

The build of the library always gives `-std=gnu11`. It does not use the
default mode of the compiler. The reason is that the mode changes the
meaning of `bool` and `static_assert`, and the build machine must not
decide this.

### Compiler versions

CI builds with the GCC and Clang of its runner images. It does not use
fixed versions. The hardening flags of the build are fully effective only
with newer compilers:

- `-D_FORTIFY_SOURCE=3` gives level 3 with GCC 12 or later, or Clang 9 or
  later. Older compilers get level 2.
- `-fstack-clash-protection` needs Clang 11.

The build tests each compiler flag before it uses it. It does not use a flag
that the compiler does not support for the target. Therefore, an older compiler
can build the library, with the hardening that it supports.
`make hardening_report` shows which flags the build uses. The source code
itself does not need these flags.

## Loading the library with dlopen

If a program links with `libccollections.so` in the usual way, you can
ignore this section.

The library has some fast paths for each thread:

- the per-thread cache of `cmempool`
- a per-thread slot that the library uses when it finds the object of a
  `clogger`, `clrucache`, `cthreadpool` or `chttpclient` handle
- the date cache of `chttpserver`
- the per-thread state of `clogger`

With glibc and on FreeBSD, these fast paths use the *initial-exec*
thread-local storage model. This model removes one function call from each
access. But it has a cost. A `dlopen()` of the library must put the
thread-local block of the library, some hundreds of bytes, into a small
reserve. The dynamic loader keeps this reserve for libraries that load late,
and all of these libraries share it:

- **glibc** keeps approximately 1.6 KiB. Therefore, a `dlopen()` fails only when
  other libraries that loaded late used all of the reserve before.
- **FreeBSD** keeps 128 bytes, and the load fails with "No space available
  for static Thread Local Storage". Start such a program with
  `LD_STATIC_TLS_EXTRA=512` in its environment. The FreeBSD loader reads
  this variable at start-up. This library needs a minimum of 384.

```bash
LD_STATIC_TLS_EXTRA=512 ./my_plugin_host
```

If you cannot control the environment, build the library with the general
model:

```bash
make EXTRA_CFLAGS="-DCCOL_MEMPOOL_DYNAMIC_TLS=1"
```

With this model, `dlopen()` always works. The cost is mainly on the
thread-safe path of `cmempool`. This path is between one third and two
thirds slower on Linux, and up to 1.9 times slower on FreeBSD. It is then
slower than `malloc` itself. On C libraries other than glibc and the C
library of FreeBSD, the general model is the default.

On macOS, thread-local storage has no static reserve, so `dlopen()` of the
library always works, and none of this section applies.

## Unloading the library with dlclose

A process can unload the library with `dlclose()`, continue to run, and then
call `fork()`. When the library unloads, the system removes the
`pthread_atfork()` handlers and the thread-specific keys that some modules
register. A thread that continues to run after the unload can stop
correctly. CI uses `make check_dso_unload` to check all of this on the built
library.

You must do two things:

- **Destroy each handle before you unload.** A thread pool, logger, event
  loop or HTTP client with threads that run cannot continue to work when its
  code is removed. This is true for all shared libraries.
- **Join the threads that used a thread-safe `cmempool` pool,** if their
  cached memory is important to you. When the code is unloaded, the library
  cannot free the per-thread cache of a thread that continues to run. This
  cache stays allocated until the process stops.

On macOS, `dlclose()` never unloads the library, because the dynamic loader
never unloads a library that has thread-local variables (see `dlclose(3)` of
macOS). The library and its handlers then stay until the process stops. You
must still destroy each handle before the `dlclose()`.

## Reference

[ccollections(7)](../man/common/ccollections.7) (compile-time switches)

Related guides:
[Building and linking](building.md),
[How the library works](design.md),
[Testing, fuzzing and benchmarks](testing.md),
[cmempool](cmempool.md)
