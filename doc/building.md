# Building, installing and linking

This guide goes from a fresh clone to a program that links with
c_collections. On the way it covers the build switches, some of which are
compile-time options that change how the library behaves while others leave
optional modules out.

The supported systems and compilers are in [Platforms](platforms.md), and
running the test suites is in [Testing](testing.md).

## Requirements

- GNU make 4 or later. On FreeBSD and OpenBSD it's called `gmake`, and
  FreeBSD's own `make` only prints a message that tells you so. The `make`
  that comes with macOS is GNU make 3.81, which is too old, so install a
  current one with Homebrew (which names it `gmake`).
- GCC 10 or later or Clang 11 or later (Apple clang on macOS), with C11 and
  the GNU extensions (see [Platforms](platforms.md)).
- OpenSSL (`libssl`, `libcrypto`) 1.1.1 or later and zlib, with their
  development headers. On OpenBSD the LibreSSL (7.9 and later) and the zlib
  of the base system are fine. Only the HTTP client and the HTTP server use
  OpenSSL and only the logger uses zlib, so you can build without them (see
  [Leaving modules out](#leaving-modules-out)).
- The system thread library and math library. On the BSDs you need
  `libexecinfo` as well, which is part of the base system.

On Debian or Ubuntu:

```bash
sudo apt-get install build-essential libssl-dev zlib1g-dev
```

On Alpine Linux (musl):

```bash
apk add build-base linux-headers openssl-dev zlib-dev
```

On FreeBSD:

```bash
pkg install gmake
```

On OpenBSD:

```bash
pkg_add gmake
```

On macOS, with Homebrew (the macOS SDK has zlib):

```bash
brew install make openssl@3
export CPATH="$(brew --prefix openssl@3)/include"
export LIBRARY_PATH="$(brew --prefix openssl@3)/lib"
```

## Building

```bash
make            # gmake on FreeBSD, OpenBSD and macOS
```

The build leaves these files in the top directory:

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

The compiler is `gcc` on Linux and the system `cc` everywhere else. If you
want a different one, set it on the command line or in the environment:

```bash
make CC=clang
```

Extra compiler flags go in `EXTRA_CFLAGS`. They are added to the project's
own flags, so the `-Wall -Wextra -Werror` baseline stays, and the build
passes them to the link of the shared library too so that a sanitizer can
link its runtime:

```bash
make EXTRA_CFLAGS="-fsanitize=address,undefined"
```

The build compiles again whenever the compiler or the flags change, so you
don't need a `make clean` when you switch between configurations.

### The saved configuration

Every build saves its configuration in `obj/.config.mk`, which holds the
compiler (`CC`), `EXTRA_CFLAGS` and the `WITH_*` switches of
[Leaving modules out](#leaving-modules-out). Every later `make` in the same
tree reads that file, so a command you run after the build works on the
library that build made and you don't have to repeat its options. That
matters most for the install, because `sudo` doesn't pass your variables
on:

```bash
make CC=clang EXTRA_CFLAGS="-DCCOL_MEMPOOL_COMPACT_LAYOUT=1"
sudo make install     # installs that build; nothing is compiled again
```

A value on the command line or in the environment always wins over the
saved one, and the build it starts saves the new value. As long as a saved
value differs from its default, every run prints one line that names it,
for example:

```
Saved build configuration (obj/.config.mk), used by the library build, the install and the test suites: CC="clang"; `make clean` resets it
```

Every target of the top Makefile uses the saved configuration, so
`make check_abi` and the install follow it. `make test`, `make memtest`,
`make coverage_site` and `generate_coverage_report` pass the saved `CC` and
`EXTRA_CFLAGS` on to the Makefile of each suite, which means the suites are
compiled with the same compiler and flags as the library, and the saved
`WITH_*` switches decide which suites run. A `CC` or `EXTRA_CFLAGS` on the
command line wins there as well. The Makefiles under `tests/` don't read
the file by themselves, so a `make test` that you run inside
`tests/<module>/` uses the defaults of that suite; when you want to test a
configured build, run the suites from the top directory. Only a command
that builds the library writes the file. `make clean` removes it and the next
build starts from the defaults again.

The library is compiled with hardening flags (`-fstack-protector-strong`,
`-fstack-clash-protection`, `-D_FORTIFY_SOURCE=3`) and linked with full
RELRO. The build tries each of these flags first, together with
`EXTRA_CFLAGS`, and drops a flag that the compiler doesn't support for that
target with those flags. Apple clang with `-fsanitize=address` for example
sets `_FORTIFY_SOURCE` to 0 on its own, so the build doesn't add
`-D_FORTIFY_SOURCE=3` there. You can see which flags the build uses with:

```bash
make hardening_report
```

`make clean` removes all build artefacts, including the test binaries and
the coverage output.

## Installing

`make install` puts the shared library, the static archive, the public
headers, the `pkg-config` file and the man pages under `/usr/local`:

```bash
sudo make install
sudo make uninstall
```

The install only uses `sudo` when it can't write to a destination
directory, so an install into a prefix that you own never asks for a
password. `SUDO=` turns `sudo` off and `SUDO=doas` uses a different tool.

`make uninstall` removes exactly the files that `make install` installed,
and the header directory too if nothing is left in it.

### Choosing where things go

`PREFIX` moves the whole install. `DESTDIR` puts the install into a
staging directory and leaves the live system alone, which is what a package
build needs:

```bash
make install PREFIX="$HOME/.local"           # your own prefix
make install DESTDIR=/tmp/stage PREFIX=/usr  # a staged package build
```

Four more variables move one part of the install each, and their defaults
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
the locations the install used. The install refuses a value of any of these
variables, or of `PREFIX` or `DESTDIR`, that has a space or a tab in it.

After an install into the live system (no `DESTDIR`) the build updates the
cache of the dynamic linker and the man page index. The first is `ldconfig`
(`ldconfig -R` on FreeBSD and `ldconfig -m LIBDIR` on OpenBSD, which add to
the cache instead of replacing it) and the second is `mandb`, or
`makewhatis` on FreeBSD and OpenBSD. On macOS the install updates neither,
because the macOS dynamic linker has no cache to update and `man` finds the
installed pages without an index. If one of these updates fails, the
install doesn't fail with it.

## Including the headers

The headers are installed into a directory of their own,
`INCLUDEDIR/ccollections/`, and you always include them through that
directory name:

```c
#include <ccollections/cvector.h>
#include <ccollections/chashmap.h>
#include <ccollections/chttpserver.h>
```

Some headers have generic names like `common.h` and `cstring.h`. Your `-I`
flags name `INCLUDEDIR` and never the `ccollections/` directory itself, so a
header of yours with the same name (your project's own `common.h`, say)
never hides one of the library's headers and the library never hides yours,
whatever order your `-I` flags are in.

Include only the headers you use. `cvector.h`, `chashmap.h` and `cbstmap.h`
pull in the shared iteration API (`citerators.h`) by themselves.

## Linking

`pkg-config` gives you the right flags:

```bash
gcc -std=gnu11 -o myapp myapp.c $(pkg-config --cflags --libs ccollections)
```

With the default `/usr/local` prefix you can write the flags yourself as
well. The shared library records its own dependencies, so `-lccollections`
is enough:

```bash
gcc -std=gnu11 -o myapp myapp.c -lccollections
```

If you installed into a non-standard prefix and the program can't find
`libccollections.so.1` at run time, add that `lib` directory to the
loader's search path, with `LD_LIBRARY_PATH` or with an rpath such as
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

An archive records no dependencies, so your link line has to name every
library the archive needs. `pkg-config --static` prints the whole list,
OpenSSL's own dependencies on your system included. When the shared library
and the archive are both installed the linker prefers the shared one, so
name the archive explicitly:

```bash
gcc -std=gnu11 -o myapp myapp.c $(pkg-config --cflags ccollections) \
    -l:libccollections.a $(pkg-config --libs --static ccollections | sed 's/-lccollections//')
```

or build a fully static program:

```bash
gcc -std=gnu11 -static -o myapp myapp.c $(pkg-config --cflags --libs --static ccollections)
```

The archive is compiled with `-fPIC`, so you can link it into a shared
library of your own too.

## Module layering

All the modules are in one library, so you never pick modules at link
time. The layers do matter when you decide how much of the library to use,
or when you care about the size of a static link:

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

Dependencies only point in this direction. Only the logger uses zlib (to
compress rotated files) and only the HTTP modules use OpenSSL. A program
that only uses the containers links with both libraries anyway but never
calls either of them, and if you want them gone completely you leave those
modules out.

The shared library exports exactly the symbols that the installed headers
declare, so you can't link with an internal helper by accident.

## Compile-time configuration

Three switches change the library itself, and you pass them through
`EXTRA_CFLAGS` when you build it.

### CCOL_FORK_SAFETY_REQUIRED

The default is `1`. The thread pool, the thread-communication module, the
logger and the HTTP client and server register `pthread_atfork()` handlers
so that a `fork()` never gives the child one of their internal locks in a
locked state, and that costs some extra work on every `fork()` in the
process. If your program never forks, or only forks in the two supported
patterns that [Concurrency](concurrency.md#fork) describes, you can remove
the handlers at compile time. The extra work is paid per `fork()`, so for
most programs the default is the one to keep; it is a program that forks
often that gains from turning it off.

```bash
make EXTRA_CFLAGS="-DCCOL_FORK_SAFETY_REQUIRED=0"
```

Nothing else about thread safety changes, and the library keeps all its
locks. On macOS a `fork()` also waits while the library makes a new socket
close-on-exec, and that wait stays with the switch off, because it is there
for a child that calls `exec()` (see
[Concurrency](concurrency.md#descriptors-and-exec)).

### CCOL_MEMPOOL_COMPACT_LAYOUT

The default is `0`. This switch sets the distance between the entries of
a `cmempool` pool. By default the distance is rounded up to the next power
of two, which makes each allocation and each release a bit faster. The
compact layout uses the aligned element size as the distance, and that
saves memory when your element sizes are a little above a power of two:

| element size | default distance | compact distance |
|---|---|---|
| 16, 64, 4096 | 16, 64, 4096 | 16, 64, 4096 |
| 96 | 128 | 96 |
| 100 | 128 | 112 |
| 5000 | 8192 | 5008 |

```bash
make EXTRA_CFLAGS="-DCCOL_MEMPOOL_COMPACT_LAYOUT=1"
```

The setting is for the whole build on purpose, because a choice per pool
would make every allocation slower, even in pools where both layouts come
out the same.

No function and no type changes with it. The only thing that changes is
one macro, `CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER`, which sizes an array
in your code, so a file that uses this macro has to be compiled with the
same setting as the library. If they don't match you won't get a pool of
the wrong size; the link fails with an undefined reference that names the
layout your code expected, for example:

```
undefined reference to `_ccol_mempool_built_with_compact_layout'
```

Rebuild whichever side is wrong. A file that declares no such buffer
needs nothing, and the setting doesn't affect
`CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER`.

A compact build has a different ABI from the default build, so
`make check_abi` skips it and `make update_abi_baseline` won't record it.

### CCOL_MEMPOOL_DYNAMIC_TLS

This switch picks the thread-local storage model of the library's
per-thread fast paths. If you don't set it, it's `0` (initial-exec, the fast
model) with glibc and on FreeBSD and `1` (the general model) on the other
systems. On OpenBSD the compiler emulates thread-local storage, so the
switch changes nothing there. Only set it to `1` when your program loads the
library with `dlopen()` and can't give the loader the space that the fast
model needs.

```bash
make EXTRA_CFLAGS="-DCCOL_MEMPOOL_DYNAMIC_TLS=1"
```

[Platforms](platforms.md#loading-the-library-with-dlopen) explains when
that is the case and what it costs.

## Leaving modules out

Five modules are optional and they are in the build by default. To leave
one out, set its switch to `0`:

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

These switches are for removing dependencies, not for making the library
smaller. The generated `ccollections.pc` follows the build, so
`pkg-config --libs --static` only lists the libraries that the build really
uses.

Both HTTP modules write their logs through `clogger`, so as long as one of
them is on the build ignores `WITH_CLOGGER=0` and prints a warning.

The install leaves out the header and the man pages of every module you
left out, so `#include <ccollections/cyaml.h>` fails at compile time and not
at link time. `make test` skips the tests of that module, and so does
`make check_abi`. The build saves the switches (see
[The saved configuration](#the-saved-configuration)), so this holds for
every later command in the tree until a `make clean` or a new value of the
switch.

A reduced build can't take the place of a full build. It has the same
SONAME (`libccollections.so.1`) but exports fewer symbols, so a program that
was linked with a full build won't start with a reduced one. Reduced builds
are meant for a library that you build and embed yourself, and that's why
`make install` refuses a reduced build unless you confirm that the
destination is meant for one:

```bash
make WITH_CJSON=0
make install                          # refused
make install ALLOW_REDUCED_INSTALL=1  # allowed
```

`ALLOW_REDUCED_INSTALL` isn't saved, so you have to give it again on every
install of a reduced build, and such an install should go to a `PREFIX` or
`DESTDIR` of its own.

## Reference

[ccollections(7)](https://abunchofnerds-25.github.io/ccollections/man/common/ccollections.7.html) (compile-time switches, allocators,
return codes),
[CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER(3)](https://abunchofnerds-25.github.io/ccollections/man/cmempool/CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER.3.html),
[ccol_mempool_create(3)](https://abunchofnerds-25.github.io/ccollections/man/cmempool/ccol_mempool_create.3.html)

Related guides:
[How the library works](design.md),
[Platforms](platforms.md),
[Compatibility and versioning](compatibility.md),
[Concurrency](concurrency.md),
[Testing, fuzzing and benchmarks](testing.md),
[cmempool](cmempool.md)
