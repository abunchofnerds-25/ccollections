# Building, installing and linking

This guide takes you from a fresh clone to a program that links with
c_collections. Along the way it covers the build switches: some are
compile-time options that change how the library behaves, and others leave
optional modules out.

For the supported systems and compilers, see [Platforms](platforms.md). To
run the test suites, see [Testing](testing.md).

## Requirements

- **GNU make** 4 or later. On FreeBSD it is called `gmake`; the system
  `make` there only prints a message telling you so. The `make` that ships
  with macOS is GNU make 3.81, which is too old, so install a current one
  with Homebrew, which names it `gmake`.
- **GCC or Clang** with C11 and the GNU extensions (see
  [Platforms](platforms.md)).
- **OpenSSL** (`libssl`, `libcrypto`) and **zlib**, with their development
  headers. Only the HTTP client and the HTTP server use OpenSSL, and only the
  logger uses zlib, so you can build without them (see
  [Leaving modules out](#leaving-modules-out)).
- The system thread library and math library. On the BSDs you also need
  `libexecinfo`, which is part of the base system.

On Debian or Ubuntu:

```bash
sudo apt-get install build-essential libssl-dev zlib1g-dev
```

On FreeBSD:

```bash
pkg install gmake
```

On macOS, with Homebrew (the macOS SDK has zlib):

```bash
brew install make openssl@3
export CPATH="$(brew --prefix openssl@3)/include"
export LIBRARY_PATH="$(brew --prefix openssl@3)/lib"
```

## Building

```bash
make            # gmake on FreeBSD and macOS
```

The build produces these files in the top directory:

| File | What it is |
|---|---|
| `libccollections.so.1.0.0` | the shared library |
| `libccollections.so.1` | the SONAME link, which programs record |
| `libccollections.so` | the development link that `-lccollections` finds |
| `libccollections.a` | the static archive |
| `ccollections.pc` | the `pkg-config` metadata |

On macOS the three shared library files are `libccollections.1.0.0.dylib`,
`libccollections.1.dylib` and `libccollections.dylib`, and the install name
that programs record is `$(LIBDIR)/libccollections.1.dylib`.

The compiler is `gcc` on Linux and the system `cc` elsewhere. To use a
different one, set it on the command line or in the environment:

```bash
make CC=clang
```

To add compiler flags, use `EXTRA_CFLAGS`. It adds to the project's own flags
and keeps the `-Wall -Wextra -Werror` baseline. The build also passes these
flags to the link of the shared library, so that a sanitizer can link its
runtime:

```bash
make EXTRA_CFLAGS="-fsanitize=address,undefined"
```

The build remembers the compiler and flags of the last build and compiles
again when they change, so you do not need a `make clean` when you switch
between configurations.

The library is compiled with hardening flags (`-fstack-protector-strong`,
`-fstack-clash-protection`, `-D_FORTIFY_SOURCE=3`) and linked with full
RELRO. The build first tests each of these flags, together with
`EXTRA_CFLAGS`, and leaves out any flag that the compiler does not support for
that target with those flags. For example, Apple clang with
`-fsanitize=address` sets `_FORTIFY_SOURCE` to 0 itself, so the build does
not add `-D_FORTIFY_SOURCE=3` there. To see which flags the build uses:

```bash
make hardening_report
```

`make clean` removes all build artefacts, including the test binaries and
the coverage output.

## Installing

`make install` installs these items under `/usr/local`:

- the shared library
- the static archive
- the public headers
- the `pkg-config` file
- the man pages

```bash
sudo make install
sudo make uninstall
```

The install asks for `sudo` only when it cannot write to a destination
directory, so an install into a prefix that you own never asks for a
password. `SUDO=` disables `sudo`, and `SUDO=doas` uses a different tool.

`make uninstall` removes exactly the files that `make install` installed,
and also removes the header directory if it is left empty.

### Choosing where things go

`PREFIX` moves the whole install. `DESTDIR` puts the install into a staging
directory without touching the live system, which is what a package build
needs:

```bash
make install PREFIX="$HOME/.local"           # your own prefix
make install DESTDIR=/tmp/stage PREFIX=/usr  # a staged package build
```

Four more variables each move one part of the install, and their defaults
follow `PREFIX`:

| Variable | Default | Holds |
|---|---|---|
| `LIBDIR` | `$(PREFIX)/lib` | the shared library and the archive |
| `INCLUDEDIR` | `$(PREFIX)/include` | the `ccollections/` header directory |
| `MANDIR` | `$(PREFIX)/share/man` | `man3/` and `man7/` |
| `PKGCONFIGDIR` | `$(LIBDIR)/pkgconfig`; `$(PREFIX)/libdata/pkgconfig` on FreeBSD | `ccollections.pc` |

A distribution can set its own library directory:

```bash
make install DESTDIR=/tmp/stage PREFIX=/usr LIBDIR=/usr/lib64
make install DESTDIR=/tmp/stage PREFIX=/usr LIBDIR=/usr/lib/x86_64-linux-gnu
```

`ccollections.pc` records `LIBDIR` and `INCLUDEDIR`, so `pkg-config` finds
the locations that the install used. The install refuses a value of any of
these variables, or of `PREFIX` or `DESTDIR`, that contains a space or a tab.

After an install into the live system (no `DESTDIR`), the build updates two
indexes:

- the cache of the dynamic linker (`ldconfig`, or `ldconfig -R` on FreeBSD)
- the man page index (`mandb`, or `makewhatis` on FreeBSD)

On macOS the install updates neither, because the macOS dynamic linker keeps
no cache to update and `man` finds the installed pages without an index.

A failure of either update does not make the install fail.

## Including the headers

The headers install into a directory of their own, `INCLUDEDIR/ccollections/`. Always include them through that directory name:

```c
#include <ccollections/cvector.h>
#include <ccollections/chashmap.h>
#include <ccollections/chttpserver.h>
```

Some headers have generic names (`common.h`, `cstring.h`). Because your `-I`
flags name `INCLUDEDIR` and never the `ccollections/` directory itself, a
header of yours with the same name, such as your project's own `common.h`,
never hides a header of the library, and a header of the library never hides
yours. This holds whatever the order of your `-I` flags.

Include only the headers that you use. `cvector.h`, `chashmap.h` and
`cbstmap.h` automatically include the shared iteration API
(`citerators.h`).

## Linking

`pkg-config` gives the correct flags:

```bash
gcc -std=gnu11 -o myapp myapp.c $(pkg-config --cflags --libs ccollections)
```

With the default `/usr/local` prefix you can also write the flags yourself.
The shared library records its own dependencies, so `-lccollections` is
enough:

```bash
gcc -std=gnu11 -o myapp myapp.c -lccollections
```

If you installed into a non-standard prefix and the program cannot find
`libccollections.so.1` at run time, add that `lib` directory to the loader's
search path, either with `LD_LIBRARY_PATH` or with an rpath such as
`-Wl,-rpath,$HOME/.local/lib`. On macOS a program finds the library through
its install name, which is the `LIBDIR` of the install.

### In your own Makefile

```make
PKG_CONFIG ?= pkg-config
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra $(shell $(PKG_CONFIG) --cflags ccollections)
LDLIBS  += $(shell $(PKG_CONFIG) --libs ccollections)

wordcount: wordcount.c

clean:
	rm -f wordcount
```

### Static linking

An archive records no dependencies, so your link line must name every
library that the archive needs. `pkg-config --static` prints the full list,
including the dependencies of OpenSSL on your system. When both the shared
library and the archive are installed, the linker prefers the shared
library, so name the archive explicitly:

```bash
gcc -std=gnu11 -o myapp myapp.c $(pkg-config --cflags ccollections) \
    -l:libccollections.a $(pkg-config --libs --static ccollections | sed 's/-lccollections//')
```

Or build a fully static program:

```bash
gcc -std=gnu11 -static -o myapp myapp.c $(pkg-config --cflags --libs --static ccollections)
```

The archive is compiled with `-fPIC`, so you can also link it into a shared
library of your own.

## Module layering

All modules live in one library, so you never select modules at link time.
The layers do matter when you decide how much of the library to use, or
when you think about the size of a static link:

```
Core                    nothing outside Core
  common                return codes, cmap_pair, allocator hooks
  csort                 iterative bottom-up mergesort
  cvector               dynamic array (csort)
  chashmap              hash map
  cbstmap               ordered map (AVL tree)
  cstring               dynamic string (cvector)
  cmempool              fixed-size and ranged memory pools
  citerators            header only; one iteration API over the containers

Concurrency             builds on Core
  cthreadpool           cvector
  cthreadcomm           cvector, chashmap, cthreadpool

Utilities               builds on Core and Concurrency
  clogger               cvector, chashmap, cthreadcomm, cthreadpool
  clrucache             cvector, chashmap, cthreadcomm, cthreadpool

Serialization           builds on Core
  cjson                 cvector, chashmap
  cyaml                 cvector, chashmap

Networking              builds on all of the above, plus OpenSSL
  chttp                 shared HTTP types, base64, Basic auth
  chttpclient           chttp, cvector, chashmap, cthreadcomm,
                        cthreadpool, clogger
  chttpserver           chttp, cvector, chashmap, cthreadcomm,
                        cthreadpool, clogger
```

Dependencies only ever point in this direction. Only the logger uses zlib
(to compress rotated files), and only the HTTP modules use OpenSSL. A program
that uses only the containers links with both libraries anyway but never
calls either of them; to remove them completely, leave those modules out.

The shared library exports exactly the symbols that the installed headers
declare, so you cannot link with an internal helper by accident.

## Compile-time configuration

Three switches change the library itself. Pass them through `EXTRA_CFLAGS`
when you build the library.

### CCOL_FORK_SAFETY_REQUIRED

The default is `1`. Some modules register `pthread_atfork()` handlers:

- the thread pool
- the thread-communication module
- the logger
- the HTTP client and the HTTP server

These handlers make sure that a `fork()` never hands the child one of their
internal locks in a locked state, at the cost of some extra work on every
`fork()` in the process. If your program never forks, or forks only in the
two supported patterns that [Concurrency](concurrency.md#fork) describes,
you can remove the handlers at compile time:

```bash
make EXTRA_CFLAGS="-DCCOL_FORK_SAFETY_REQUIRED=0"
```

This changes nothing else about thread safety: the library keeps all its
locks. On macOS a `fork()` also waits while the library makes a new socket
close-on-exec. That wait is kept even with the switch off, because it
protects a child that calls `exec()` (see
[Concurrency](concurrency.md#descriptors-and-exec)).

### CCOL_MEMPOOL_COMPACT_LAYOUT

The default is `0`. This switch sets the distance between the entries of a
`cmempool` pool. By default the distance is rounded up to the next power of
two, which makes each allocation and each release a little faster. The
compact layout uses the aligned element size as the distance instead, which
saves memory when your element sizes are a little larger than a power of
two:

| element size | default distance | compact distance |
|---|---|---|
| 16, 64, 4096 | 16, 64, 4096 | 16, 64, 4096 |
| 96 | 128 | 96 |
| 100 | 128 | 112 |
| 5000 | 8192 | 5008 |

```bash
make EXTRA_CFLAGS="-DCCOL_MEMPOOL_COMPACT_LAYOUT=1"
```

The setting applies to the whole build on purpose: a per-pool choice would
make every allocation slower, even in pools where the two layouts are the
same.

The setting changes no function and no type, only one macro,
`CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER`, which sizes an array in your
code. A file that uses this macro must therefore be compiled with the same
setting as the library. A mismatch does not give you a pool of the wrong
size; the link fails instead, with an undefined reference that names the
layout your code expected, for example:

```
undefined reference to `_ccol_mempool_built_with_compact_layout'
```

Rebuild whichever side is wrong. A file that declares no such buffer needs
nothing, and the setting has no effect on
`CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER`.

A compact build has a different ABI from the default build, so
`make check_abi` skips it and `make update_abi_baseline` refuses to record
it.

### CCOL_MEMPOOL_DYNAMIC_TLS

This switch selects the thread-local storage model of the library's
per-thread fast paths. If you do not set it, its value is `0` (initial-exec,
the fast model) with glibc and on FreeBSD, and `1` (the general model) on
other systems. Set it to `1` only if both of these are true:

- Your program loads the library with `dlopen()`.
- Your program cannot give the loader the space that the fast model needs.

```bash
make EXTRA_CFLAGS="-DCCOL_MEMPOOL_DYNAMIC_TLS=1"
```

[Platforms](platforms.md#loading-the-library-with-dlopen) explains when
this applies and what it costs.

## Leaving modules out

Five modules are optional and included by default. To leave one out, set
its switch to `0`:

| Switch | Module | Also removes |
|---|---|---|
| `WITH_CJSON` | `cjson` | nothing |
| `WITH_CYAML` | `cyaml` | nothing |
| `WITH_CLOGGER` | `clogger` | zlib |
| `WITH_CHTTPCLIENT` | `chttpclient` | OpenSSL, together with the server |
| `WITH_CHTTPSERVER` | `chttpserver` | OpenSSL, together with the client |

```bash
# no OpenSSL
make WITH_CHTTPCLIENT=0 WITH_CHTTPSERVER=0

# pthread and libm only
make WITH_CJSON=0 WITH_CYAML=0 WITH_CLOGGER=0 \
     WITH_CHTTPCLIENT=0 WITH_CHTTPSERVER=0
```

The purpose is to remove dependencies, not to make the library smaller.
With both HTTP modules off the build does not use OpenSSL, and with the
logger also off it does not use zlib. The generated `ccollections.pc`
follows the build, so `pkg-config --libs --static` lists only the libraries
that the build actually uses.

Both HTTP modules write their logs through `clogger`, so while either of them
is on, the build ignores `WITH_CLOGGER=0` and prints a warning.

The install leaves out the header and the man pages of every module that you
left out, so `#include <ccollections/cyaml.h>` fails at compile time rather
than at link time. `make test` skips the tests of that module, and
`make check_abi` skips as well.

**A reduced build cannot replace a full build.** It has the same SONAME
(`libccollections.so.1`) but exports fewer symbols, so a program that you
linked with a full build does not start with a reduced one. Reduced builds
are meant for a library that you build and embed yourself, which is why `make
install` refuses a reduced build unless you confirm that the
destination is meant for one:

```bash
make WITH_CJSON=0 install                          # refused
make WITH_CJSON=0 ALLOW_REDUCED_INSTALL=1 install  # allowed
```

For such an install, use a separate `PREFIX` or `DESTDIR`.

## Reference

[ccollections(7)](../man/common/ccollections.7) (compile-time switches, allocators,
return codes),
[CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER(3)](../man/cmempool/CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER.3),
[ccol_mempool_create(3)](../man/cmempool/ccol_mempool_create.3)

Related guides:
[How the library works](design.md),
[Platforms](platforms.md),
[Compatibility and versioning](compatibility.md),
[Concurrency](concurrency.md),
[Testing, fuzzing and benchmarks](testing.md),
[cmempool](cmempool.md)
