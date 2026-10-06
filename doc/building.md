# Building, installing and linking

This guide starts with a new clone and ends with a program that links with
c_collections. It also tells you about the build switches. Some switches are
compile-time options that change the behavior of the library. Other switches
leave optional modules out.

For the supported systems and compilers, see [Platforms](platforms.md). To
run the test suites, see [Testing](testing.md).

## Requirements

- **GNU make.** On FreeBSD, its name is `gmake`. The system `make` on
  FreeBSD only prints a message that tells you this.
- **GCC or Clang** with C11 and the GNU extensions (see
  [Platforms](platforms.md)).
- **OpenSSL** (`libssl`, `libcrypto`) and **zlib**, with their development
  headers. Only the HTTP client and the HTTP server use OpenSSL. Only the
  logger uses zlib. You can build without them. See
  [Leaving modules out](#leaving-modules-out).
- The thread library and the math library of the system. On the BSDs, also
  `libexecinfo`, which is part of the base system.

On Debian or Ubuntu:

```bash
sudo apt-get install build-essential libssl-dev zlib1g-dev
```

On FreeBSD:

```bash
pkg install gmake
```

## Building

```bash
make            # gmake on FreeBSD
```

The build makes these files in the top directory:

| File | What it is |
|---|---|
| `libccollections.so.1.0.0` | the shared library |
| `libccollections.so.1` | the SONAME link, which programs record |
| `libccollections.so` | the development link that `-lccollections` finds |
| `libccollections.a` | the static archive |
| `ccollections.pc` | the `pkg-config` metadata |

The compiler is `gcc` on Linux and the system `cc` on other systems. To use
a different compiler, give it on the command line or in the environment:

```bash
make CC=clang
```

To add compiler flags, use `EXTRA_CFLAGS`. It adds to the flags of the
project and keeps the `-Wall -Wextra -Werror` baseline:

```bash
make EXTRA_CFLAGS="-fsanitize=address,undefined"
```

The build records the compiler and the flags of the last build. When they
change, the build compiles again. Therefore, you do not need a `make clean`
between two different configurations.

The build compiles the library with hardening flags
(`-fstack-protector-strong`, `-fstack-clash-protection`,
`-D_FORTIFY_SOURCE=3`). It links the library with full RELRO. The build
first tests each compiler flag. It does not use a flag that the compiler
does not support for the target. To see which flags the build uses:

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
directory. Therefore, an install into a prefix that you own never asks for a
password. `SUDO=` disables `sudo`. `SUDO=doas` uses a different tool.

`make uninstall` removes exactly the files that `make install` installed.
It also removes the header directory when that directory is empty.

### Choosing where things go

`PREFIX` moves the full install. `DESTDIR` puts the install into a staging
directory and does not change the live system. A package build needs this:

```bash
make install PREFIX="$HOME/.local"           # your own prefix
make install DESTDIR=/tmp/stage PREFIX=/usr  # a staged package build
```

Four more variables each move one part of the install. Their defaults follow
`PREFIX`:

| Variable | Default | Holds |
|---|---|---|
| `LIBDIR` | `$(PREFIX)/lib` | the shared library and the archive |
| `INCLUDEDIR` | `$(PREFIX)/include` | the `ccollections/` header directory |
| `MANDIR` | `$(PREFIX)/share/man` | `man3/` and `man7/` |
| `PKGCONFIGDIR` | `$(LIBDIR)/pkgconfig`; `$(PREFIX)/libdata/pkgconfig` on FreeBSD | `ccollections.pc` |

A distribution gives its own library directory:

```bash
make install DESTDIR=/tmp/stage PREFIX=/usr LIBDIR=/usr/lib64
make install DESTDIR=/tmp/stage PREFIX=/usr LIBDIR=/usr/lib/x86_64-linux-gnu
```

`ccollections.pc` records `LIBDIR` and `INCLUDEDIR`. Therefore, `pkg-config`
finds the locations that the install used. The install refuses a value of one
of these variables, or of `PREFIX` or `DESTDIR`, that contains a space or a
tab.

After an install into the live system (no `DESTDIR`), the build updates two
indexes:

- the cache of the dynamic linker (`ldconfig`, or `ldconfig -R` on FreeBSD)
- the man page index (`mandb`, or `makewhatis` on FreeBSD)

If one of these updates fails, the install does not fail.

## Including the headers

The headers install into their own directory, `INCLUDEDIR/ccollections/`.
Always include them through this directory name:

```c
#include <ccollections/cvector.h>
#include <ccollections/chashmap.h>
#include <ccollections/chttpserver.h>
```

Some headers have generic names (`common.h`, `cstring.h`). Your `-I` flags
give `INCLUDEDIR`, and never the `ccollections/` directory itself. Therefore, a
header of yours with the same name, for example the `common.h` of your
project, never hides a header of the library. Also, a header of the library
never hides it. The order of your `-I` flags has no effect on this.

Include only the headers that you use. `cvector.h`, `chashmap.h` and
`cbstmap.h` automatically include the shared iteration API
(`citerators.h`).

## Linking

`pkg-config` gives the correct flags:

```bash
gcc -std=gnu11 -o myapp myapp.c $(pkg-config --cflags --libs ccollections)
```

With the default `/usr/local` prefix, you can also write the flags yourself.
The shared library records its own dependencies. Therefore, `-lccollections` is
enough:

```bash
gcc -std=gnu11 -o myapp myapp.c -lccollections
```

You possibly installed into a prefix that is not standard. If the program
then cannot find `libccollections.so.1` at run time, add that `lib`
directory to the search path of the loader. Use `LD_LIBRARY_PATH`, or an
rpath such as `-Wl,-rpath,$HOME/.local/lib`.

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

An archive records no dependencies. Therefore, your link line must contain each
library that the archive needs. `pkg-config --static` prints the full list,
including the dependencies of OpenSSL on your system. When you install the
shared library and the archive, the linker uses the shared library. Therefore,
give the name of the archive explicitly:

```bash
gcc -std=gnu11 -o myapp myapp.c $(pkg-config --cflags ccollections) \
    -l:libccollections.a $(pkg-config --libs --static ccollections | sed 's/-lccollections//')
```

Or, build a fully static program:

```bash
gcc -std=gnu11 -static -o myapp myapp.c $(pkg-config --cflags --libs --static ccollections)
```

The build compiles the archive with `-fPIC`. Therefore, you can also link it
into your own shared library.

## Module layering

All modules are in one library. Therefore, you never select modules at link
time. But the layers are important when you decide how much of the library to
use, or when you think about the size of a static link:

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

The direction of the dependencies never changes. Only the logger uses zlib
(to compress rotated files). Only the HTTP modules use OpenSSL. A program
that uses only the containers also links with these two libraries, but it
calls neither of them. To remove them fully, leave those modules out.

The shared library exports exactly the symbols that the installed headers
declare. You cannot link with an internal helper accidentally.

## Compile-time configuration

Three switches change the library itself. Give them through `EXTRA_CFLAGS`
when you build the library.

### CCOL_FORK_SAFETY_REQUIRED

The default is `1`. Some modules register `pthread_atfork()` handlers:

- the thread pool
- the thread-communication module
- the logger
- the HTTP client and the HTTP server

These handlers make sure that a `fork()` never gives the child one of their
internal locks in a locked state. This adds some work to each `fork()` in
the process. Your program possibly never forks. Or it possibly forks only in
the two supported patterns that
[Concurrency](concurrency.md#fork) describes. In these two cases, you can
remove the handlers at compile time:

```bash
make EXTRA_CFLAGS="-DCCOL_FORK_SAFETY_REQUIRED=0"
```

This changes nothing else about thread safety. The library keeps all locks.

### CCOL_MEMPOOL_COMPACT_LAYOUT

The default is `0`. This switch sets the distance between the entries of a
`cmempool` pool. By default, the distance is the next power of two. This
makes each allocation and each release a little faster. The compact layout
uses the aligned element size as the distance. This saves memory when your
element sizes are a little larger than a power of two:

| element size | default distance | compact distance |
|---|---|---|
| 16, 64, 4096 | 16, 64, 4096 | 16, 64, 4096 |
| 96 | 128 | 96 |
| 100 | 128 | 112 |
| 5000 | 8192 | 5008 |

```bash
make EXTRA_CFLAGS="-DCCOL_MEMPOOL_COMPACT_LAYOUT=1"
```

The setting applies to the full build. This is intentional. A choice for
each pool would make each allocation slower, also in the pools where the two
layouts are the same.

This setting changes no function and no type. It changes one macro:
`CCOL_DECLARE_PREALLOCATED_MEMPOOL_BUFFER`. This macro sets the size of an
array in your code. Therefore, compile a file that uses the macro with the same
setting as the library. A mismatch does not give a pool of incorrect size.
Instead, the link fails with an undefined reference. The reference gives the
name of the layout that your code expected, for example:

```
undefined reference to `_ccol_mempool_built_with_compact_layout'
```

Build the incorrect side again. A file that declares no such buffer needs
nothing. This setting has no effect on
`CCOL_DECLARE_PREALLOCATED_RMEMPOOL_BUFFER`.

A compact build has an ABI that is different from the default ABI. Therefore,
`make check_abi` skips a compact build, and `make update_abi_baseline`
refuses to record it.

### CCOL_MEMPOOL_DYNAMIC_TLS

This switch selects the thread-local storage model of the fast paths that
the library uses for each thread. If you do not set it, its value is `0`
(initial-exec, the fast model) with glibc and on FreeBSD. On other systems,
its value is `1` (the general model). Set it to `1` only if both of these
conditions are true:

- Your program loads the library with `dlopen()`.
- Your program cannot give the loader the space that the fast model needs.

```bash
make EXTRA_CFLAGS="-DCCOL_MEMPOOL_DYNAMIC_TLS=1"
```

[Platforms](platforms.md#loading-the-library-with-dlopen) explains when
this applies and what it costs.

## Leaving modules out

Five modules are optional. By default, the build includes each of them. To
leave a module out, set its switch to `0`:

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

The purpose is to remove dependencies, not to make the library smaller. When
both HTTP modules are off, the build does not use OpenSSL. When the logger is
also off, the build does not use zlib. The generated `ccollections.pc` follows
the build. Therefore, `pkg-config --libs --static` gives only the libraries
that the build uses.

Both HTTP modules write logs through `clogger`. When one of them is on, the
build ignores `WITH_CLOGGER=0` and prints a warning.

The install does not install the header and the man pages of a module that you
left out. Therefore, `#include <ccollections/cyaml.h>` fails at compile time,
not at link time. `make test` skips the tests of that module. `make check_abi`
also skips.

**A reduced build cannot replace a full build.** It has the same SONAME
(`libccollections.so.1`), but it exports fewer symbols. Therefore, a program
that you linked with a full build does not start with a reduced build. Reduced
builds are for a library that you build and embed yourself. Therefore, `make
install` refuses a reduced build, unless you tell it that the destination is
correct for a reduced build:

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
