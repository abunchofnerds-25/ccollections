# C Collections

[![CI](https://github.com/abunchofnerds-25/ccollections/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/abunchofnerds-25/ccollections/actions/workflows/ci.yml?query=branch%3Amain)
[![Fuzz](https://github.com/abunchofnerds-25/ccollections/actions/workflows/fuzz.yml/badge.svg?branch=main)](https://github.com/abunchofnerds-25/ccollections/actions/workflows/fuzz.yml?query=branch%3Amain)
[![Coverage](https://github.com/abunchofnerds-25/ccollections/actions/workflows/coverage.yml/badge.svg?branch=main)](https://abunchofnerds-25.github.io/ccollections/)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**C Collections** gives C the building blocks that most other languages ship in their standard library: growable arrays, hash maps, ordered maps, strings, memory pools, thread-safe queues, a thread pool, a structured logger, JSON and YAML support, and an HTTP client and server.

If you have used `vector` and `unordered_map` in C++, `ArrayList` and `HashMap` in Java, or `list` and `dict` in Python, these containers will feel familiar. The difference is that they are plain C, and the compiler checks your element types.

```c
#include <ccollections/chashmap.h>
#include <ccollections/cvector.h>
#include <stdio.h>

int main(void) {
  /* A growable array of ints. */
  cvec_construct(scores, int);
  cvec_push(scores, 91);
  cvec_push(scores, 74);
  cvec_push(scores, 88);
  cvec_sort(scores);
  for (size_t i = 0; i < cvec_size(scores); i++)
    printf("%d ", cvec_at(scores, i));
  printf("\n");                                   /* 74 88 91 */

  /* A map from names to ints; the compiler checks both the key type and
   * the value type. */
  chmap_construct(ages, char *, int);
  chmap_insert(ages, "alice", 31);
  chmap_insert(ages, "bob", 27);
  printf("alice is %d\n", chmap_get(ages, "alice"));
  if (chmap_get_ptr(ages, "carol") == NULL) printf("no carol\n");

  chmap_destroy(ages);
  cvec_destroy(scores);
  return 0;
}
```

There is no code generator, no `void *` casting and no hidden runtime: just the headers, one library and a C11 compiler.

---

## Contents

- [Why C Collections](#why-c-collections)
- [What is inside](#what-is-inside)
- [Getting started](#getting-started)
- [Learning the library](#learning-the-library)
- [Reference documentation](#reference-documentation)
- [Platforms](#platforms)
- [Contributing](#contributing)
- [Authors](#authors)
- [License](#license)

---

## Why C Collections

C has no standard containers, so many projects write their own hash map, and many others pass everything around as `void *`, where any cast can be wrong.

This library takes a different approach. Its macros use the C11 `_Generic` keyword to see the types you declared, so a map from `char *` to `int` stays a map from `char *` to `int` at every call site. You write `chmap_insert(ages, "alice", 31)` and the compiler checks it; there are no pointer casts to get wrong.

Every module shares a few properties:

- **One style everywhere.** You create a container with `*_construct` and release it with `*_destroy`, so once you know one module, the next one is easy to pick up.
- **Optional automatic cleanup.** The `*_scoped` variants free a container when it goes out of scope.
- **Optional custom allocators.** Every module accepts your own memory management functions.
- **Thread safety where it matters.** The queues, the cache, the logger, the thread pool and the HTTP modules can be shared between threads. The plain containers are deliberately not thread-safe, which keeps them fast.
- **Production quality.** Each module has its own test suite, and the suites run on several architectures under Valgrind, AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer. The parsers are also fuzz-tested.
- **A man page for every public function and macro.**

[How the library works](doc/design.md) is a short read that explains the ideas behind the macros.

---

## What is inside

Each module has a guide with examples, so you can start with whichever one you need.

| Module | What it gives you | Guide |
|---|---|---|
| `cvector` | A growable array of any type, with sort and search | [doc/cvector.md](doc/cvector.md) |
| `cstring` | A growable string with find, replace, split and related operations | [doc/cstring.md](doc/cstring.md) |
| `chashmap` | A hash map with any key type and any value type | [doc/chashmap.md](doc/chashmap.md) |
| `cbstmap` | A map that keeps its keys in sorted order | [doc/cbstmap.md](doc/cbstmap.md) |
| `citerators` | One method to iterate over vectors and maps | [doc/citerators.md](doc/citerators.md) |
| `csort` | A stable sort for vectors and plain C arrays | [doc/csort.md](doc/csort.md) |
| `cmempool` | Fast fixed-size and ranged memory pools | [doc/cmempool.md](doc/cmempool.md) |
| `cthreadcomm` | Thread-safe queues, channels, `select` and an event loop | [doc/cthreadcomm.md](doc/cthreadcomm.md) |
| `cthreadpool` | A thread pool with futures and completion callbacks | [doc/cthreadpool.md](doc/cthreadpool.md) |
| `clrucache` | A thread-safe LRU cache, with an optional remote store | [doc/clrucache.md](doc/clrucache.md) |
| `clogger` | A structured logger: logfmt, JSON or syslog, with rotation and asynchronous writes | [doc/clogger.md](doc/clogger.md) |
| `cjson` | Parse, edit and write JSON | [doc/cjson.md](doc/cjson.md) |
| `cyaml` | Parse, edit and write YAML 1.2 | [doc/cyaml.md](doc/cyaml.md) |
| `chttpclient` | An HTTP/1.1 client with TLS, keep-alive and three call styles | [doc/chttpclient.md](doc/chttpclient.md) |
| `chttpserver` | An HTTP/1.1 server with routes, middleware, streams and TLS | [doc/chttpserver.md](doc/chttpserver.md) |

The modules are independent of each other: include only the headers you use.

---

## Getting started

### What you need

- Linux (glibc), FreeBSD 14 or later, or macOS 15 or later
- GCC or Clang (Apple clang on macOS), and GNU make (`gmake` on FreeBSD and macOS)
- The development packages of OpenSSL, zlib and pthreads

For example, on Debian or Ubuntu:

```bash
sudo apt install build-essential libssl-dev zlib1g-dev pkg-config
```

### Build and install

```bash
make                                  # builds libccollections.so and libccollections.a
make test                             # optional: runs all test suites
sudo make install                     # installs under /usr/local
make install PREFIX="$HOME/.local"    # or into your own prefix, without sudo
```

The install includes the man pages, so `man cvec_push` works straight away.

### Your first program

Save the example at the top of this page as `hello.c`, then build and run it:

```bash
gcc -o hello hello.c $(pkg-config --cflags --libs ccollections)
./hello
```

If you installed into your own prefix, point `pkg-config` at it before you compile, for example with `export PKG_CONFIG_PATH="$HOME/.local/lib/pkgconfig"`, and set `LD_LIBRARY_PATH="$HOME/.local/lib"` so that the loader can find the library at run time.

Your code always includes the headers through the `ccollections/` directory, as in `#include <ccollections/cvector.h>`.

[Building, installing and linking](doc/building.md) covers everything else: static linking, staged installs for packagers, leaving modules out of the build, and the compile-time options.

---

## Learning the library

We suggest reading the documentation in this order:

1. **[How the library works](doc/design.md):** the handful of ideas that every module shares.
2. **The containers:** [cvector](doc/cvector.md), [cstring](doc/cstring.md), [chashmap](doc/chashmap.md) and [cbstmap](doc/cbstmap.md), followed by [citerators](doc/citerators.md), which shows how to iterate over them.
3. **Whichever module your program needs next:** [communication between threads](doc/cthreadcomm.md), [a thread pool](doc/cthreadpool.md), [memory pools](doc/cmempool.md), [an LRU cache](doc/clrucache.md), [logs](doc/clogger.md), [JSON](doc/cjson.md), [YAML](doc/cyaml.md), [an HTTP client](doc/chttpclient.md) or [an HTTP server](doc/chttpserver.md).

These guides cover the library as a whole:

| Guide | When to read it |
|---|---|
| [How the library works](doc/design.md) | Before any other guide, or when a macro does not behave as you expect |
| [Building, installing and linking](doc/building.md) | When you install, package or configure the library |
| [Threads and fork()](doc/concurrency.md) | Before you share data between threads or call `fork()` |
| [Custom memory management](doc/memory.md) | When you want the library to use your own allocator |
| [Platforms](doc/platforms.md) | To check which platforms the library runs on |
| [Compatibility and versioning](doc/compatibility.md) | Before you upgrade, or to see which guarantees stay stable |
| [Testing, fuzzing and benchmarks](doc/testing.md) | When you work on the library itself |

---

## Reference documentation

The guides teach; the man pages are the reference. Every public function and macro has its own page, which describes each parameter, each return value and every edge case.

```bash
man cvec_push                       # after make install
cd man && man -l cvector/cvec_push.3   # directly from the source tree
```

Most modules also have an overview page in section 7 that collects the rules for the whole module, such as `man 7 chashmap` or `man 7 cyaml`, and `man 7 ccollections` describes the properties that all modules share.

The pages live in [`man/`](man/), one directory per module, and [`man/README`](man/README) explains the layout. Each guide ends with links to the pages of its module.

---

## Platforms

The library supports Linux with glibc (x86_64, i386, aarch64 and armhf), FreeBSD 14 or later (x86_64) and macOS 15 or later (arm64 and x86_64). CI builds and tests Linux and FreeBSD with both GCC and Clang, and macOS with Apple clang. See [Platforms](doc/platforms.md) for the details.

---

## Contributing

Bug reports are always welcome. If you would like to become a direct contributor, you can send an email to [abunchofnerds84@gmail.com](mailto:abunchofnerds84@gmail.com). [Testing, fuzzing and benchmarks](doc/testing.md) shows how to run the same checks as CI. To report a security problem, please follow [SECURITY.md](SECURITY.md). [CHANGELOG.md](CHANGELOG.md) lists the changes between releases.

---

## Authors

This library began as a hobby project on Danis Ozdemir's PC and grew into its current form over around five years. As the license notes, the authors see themselves as a bunch of nerds. Unsurprisingly, they have deep respect for Dennis Ritchie and Ken Thompson, whose work on C and Unix laid the foundation for modern computing. This project is a tribute to those legends.

- [**Danis Ozdemir**](https://www.linkedin.com/in/danis-o-4a0a8841/)
- [**Fikri Kahraman**](https://www.linkedin.com/in/fikrikahraman/)

---

## License

MIT License

Copyright (c) 2026 - A bunch of nerds

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
