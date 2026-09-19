# Changelog

All notable changes to this project are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the version
numbers follow the compatibility rules described in the "Compatibility and
Versioning" section of `README.md`.

Each entry is grouped by the kind of change it is, in the order `Added`,
`Changed`, `Deprecated`, `Removed`, `Fixed`, `Security`. A `Changed` or
`Removed` entry that affects the ABI names the ABI version it required.

## 1.0.0 - unreleased

The first release with a stable, supported interface. Everything below
describes what `1.0.0` contains, not how it differs from an earlier release:
there is no earlier release to differ from.

The shared library's SONAME is `libccollections.so.1`. Every later `1.x`
release keeps that SONAME and stays both source-compatible and
binary-compatible with this one.

### Added

- Containers: `cvector` (dynamic array), `chashmap` (hash map, open-addressed
  for integral key and value types and separately chained otherwise),
  `cbstmap` (ordered AVL map), and `citerators`, one iteration API shared by
  all three.
- Strings and algorithms: `cstring` (dynamic string) and `csort` (iterative
  bottom-up mergesort with correct NaN ordering for the floating-point
  comparators).
- Memory: `cmempool`, providing a fixed-size and a ranged pool, each usable
  from a heap allocation or a caller-supplied preallocated buffer, with
  optional fallback to dynamic allocation on exhaustion and corruption and
  double-free detection. Entries are aligned for any object type, the same
  guarantee `malloc()` makes.
- Concurrency: `cthreadcomm` (circular and dynamic queues, channels, and a
  persistent `epoll(7)` reactor), `cthreadpool` (bounded or unbounded task
  queue, completion callbacks, futures), and `clrucache` (thread-safe LRU
  cache with optional remote getter and setter).
- Serialization: `cjson` and `cyaml`, each a parser, a serializer, and a
  mutable DOM with path-based get and set macros. `cyaml` covers YAML 1.2
  tags, merge keys, and multi-document streams.
- Logging: `clogger`, with logfmt, JSON, and RFC 5424 syslog output, log
  rotation, derived loggers, and optional asynchronous batching.
- Every module accepts caller-supplied allocation functions.
- Networking: `chttpclient` (synchronous, asynchronous, and pooled-synchronous
  HTTP/1.1 over TLS or plaintext) and `chttpserver` (HTTP/1.1 with method and
  pattern routing, named path parameters, middleware chains, sub-routers,
  streaming handlers, and optional TLS).
- Packaging: a versioned shared library with a SONAME, a static archive,
  `pkg-config` metadata generated at build time, `PREFIX` and `DESTDIR`
  support in `make install`, and 446 manual pages (445 in section 3, one in
  section 7).
- `make check_namespace`, which fails the build if any exported symbol, public
  macro, or public typedef loses its namespace prefix.
- `make check_abi`, which compares the exported ABI against the committed
  baseline under `abi/` and fails on an unrecorded addition or on any removal.
- `make bench`, a benchmark suite covering the modules with a run-time cost
  worth tracking, with per-machine baselines and regression detection, and
  optional comparisons against uthash, GLib, jansson and libyaml where those
  are installed.
- `CCOL_FORK_SAFETY_REQUIRED`, a compile-time switch for the
  `pthread_atfork()`-based fork protection in `cthreadpool`, `cthreadcomm`,
  `clogger`, and `chttpserver`.
- `CCOL_MEMPOOL_COMPACT_LAYOUT`, a compile-time switch trading a little speed
  on every `cmempool` allocation and release for a stride that is not rounded
  up to a power of two. It is part of the interface between an application and
  the library, and a mismatch between the two sides is a link error rather
  than a wrongly sized pool.
- `CCOL_MEMPOOL_DYNAMIC_TLS`, a compile-time switch selecting the general
  thread-local storage model over initial-exec for the library's thread-local
  fast paths, for a library that has to be `dlopen()`ed into a process whose
  static thread-local block is already exhausted.
- `WITH_CJSON`, `WITH_CYAML`, `WITH_CLOGGER`, `WITH_CHTTPCLIENT` and
  `WITH_CHTTPSERVER`, build switches that leave a module out of the library
  entirely. Turning off both HTTP modules removes the OpenSSL dependency and
  turning off `clogger` removes zlib. A reduced build is not
  ABI-interchangeable with a full one.

### Security

- The library is built with `-fstack-protector-strong`,
  `-fstack-clash-protection`, `-D_FORTIFY_SOURCE=3`, and RELRO with BIND_NOW,
  and with `-fvisibility=hidden` so that the exported symbol set equals the
  declared public API and nothing else.
- Continuous fuzzing of the `cjson`, `cyaml`, and HTTP/1.1 parsers runs as a
  blocking job on every push and pull request.
- See `SECURITY.md` for the vulnerability reporting process.
