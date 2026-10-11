# Compatibility and versioning

This page explains which properties of c_collections stay the same when you
upgrade: what a release may change, what a major version keeps fixed, and how
the project checks that promise.

## Version numbers

Each release has a number of the form `MAJOR.MINOR.PATCH`:

| Change | Field that increases | SONAME |
|---|---|---|
| A correction with no interface change | `PATCH` | does not change |
| An added function, macro or type | `MINOR` | does not change |
| A removal, or a change to an interface | `MAJOR` | changes |

The SONAME contains only `MAJOR`, so every `1.x` release is
`libccollections.so.1`. A program built against one `1.x` release runs with
every later `1.x` release without being relinked.

## Source compatibility

Code that compiles against `1.0` compiles unchanged against every later
`1.x` release. Within `1.x`, nothing in an installed header is removed,
renamed or given a different meaning:

- No function or macro is removed.
- No parameter changes its type or its position.
- No enumerator changes its value.
- No field of a public struct is removed or reordered.

## Binary compatibility

A program linked against `libccollections.so.1` runs with every later `1.x`
release. Within `1.x`:

- No exported symbol is removed, and no exported symbol changes its
  signature.
- No public struct changes its size or its layout. This also rules out a
  change that looks harmless: a configuration struct that you fill by value,
  such as `chttpsvr_config_t`, `chttp_tls_config_t`, `clog_rotation_cfg_t` or
  `clog_async_cfg_t`, never gains a field in `1.x`. An added field changes the
  size of the struct and breaks every program that allocates one, so such a
  struct grows only in a new major version.
- No enumerator changes its numeric value. Every `ccol_retval_t` value is set
  explicitly, and `ccol_success` is `0`.

Some exported symbols have names that start with an underscore. Do not call
them by name; they exist because the typed macros expand into calls to them
(and, in one case, into a reference to a link-time marker object), which means
your compiled code refers to them directly. They carry the same guarantee as
every other exported symbol.

## Behavioral compatibility

The promise covers behavior as well as the shape of the interface. Within
`1.x`, the following do not change:

- The `ccol_retval_t` that a given input produces, and which conditions are
  fatal (the library calls `ccol_fatal_err()` and does not return).
- Which operations invalidate an iterator or a pointer to an element.
- The ownership rules for keys, values and nodes, including which calls hand
  you a borrowed reference and which calls take ownership.
- The order in which registered callbacks run, and the thread that runs each
  one.
- When the library calls an allocator that you supplied.
- The documented thread-safety class of each type.

Performance is deliberately left off this list: a `1.x` release may replace
an algorithm or a data structure with a faster one, as long as everything
above stays true.

## What the promise does not cover

- Anything that no installed header declares. The internal headers are not
  installed and export nothing, so they can change at any time.
- Anything that is available only when `RUNNING_UNIT_TESTS` is defined; those
  helpers exist only for the project's own test suites.
- The exact text of log output and error strings, and the whitespace of the
  serializers wherever the format does not fix it.
- The list of transitive dependencies of the static archive, which follows
  what OpenSSL and zlib need on the machine that builds it.
- A reduced build made with the `WITH_*` switches (see
  [Building](building.md#leaving-modules-out)). It has the same SONAME but
  exports fewer symbols, so it cannot replace a full build.

## How the promise is checked

The ordinary test suites cannot see most of these properties, because each
suite compiles the library sources directly into its test program, where a
symbol that the library does not export links just like an exported one. CI
therefore runs separate checks on every push and every pull request:

```bash
make check_namespace   # each exported symbol, public macro, typedef,
                       # enumerator and struct/union/enum tag has
                       # its namespace prefix
make check_abi         # the exported ABI matches the baseline under abi/
make check_dso_unload  # fork() works after a dlopen(), a use and a
                       # dlclose() of the library
```

`check_abi` compares two things against the recorded baseline:

- The list of exported symbols, which is the same on every architecture.
- A structural record of the function signatures and the layouts of the
  public structs. This record is kept per architecture (x86-64, i386,
  AArch64, ARM32 and FreeBSD x86-64), because a change can leave every struct
  untouched on one target while moving every field on another.

The check reads the target architecture from the built library, so a cross
build is compared against the right baseline. The structural comparison
needs libabigail (`abidw`, `abidiff`); when libabigail is missing, or the
architecture has no recorded baseline, the check says that the structural
part did not run.

Any difference fails the check, whether it is a removed symbol, a symbol
missing from the record, or a changed signature or layout. The check does not try to decide which differences are safe. When the difference is an addition to the public API, which is a normal, compatible change, record the updated baseline with `make update_abi_baseline` and commit it together with the change.

Behavior is checked by a corpus of tests, `tests/mixed/tests_compat.c`, which
runs as part of `make test` and tests the promises above directly:

- A failed attach leaves ownership where each module's documentation says it
  goes.
- An eviction callback runs on the thread that caused the eviction, before
  that call returns.
- A completion callback runs on a worker, after its task.
- A reference to one key stays valid while you find or update other keys.
- A container that uses your allocator takes every byte from it and gives
  every byte back to it.

The corpus tests only documented promises. It compares return values with
numeric literals rather than with the library's own constants, and its
allocator tests use an allocator that refuses to free a block it did not
hand out.

## Reference

[ccollections(7)](../man/common/ccollections.7) (the return codes and their values)

Related guides:
[Building and linking](building.md),
[Testing, fuzzing and benchmarks](testing.md),
[How the library works](design.md)
