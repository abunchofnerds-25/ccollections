# Compatibility and versioning

This page tells you which properties of c_collections stay the same when you
upgrade. It gives the changes that a release can contain and the properties
that do not change in a major version. It also tells you how the project
checks this promise.

## Version numbers

Each release has a number with the form `MAJOR.MINOR.PATCH`:

| Change | Field that increases | SONAME |
|---|---|---|
| A correction, with no interface change | `PATCH` | does not change |
| An added function, macro or type | `MINOR` | does not change |
| A removal or a change of an interface | `MAJOR` | changes |

The SONAME contains only `MAJOR`. Therefore, each `1.x` release is
`libccollections.so.1`. A program that you built with one `1.x` release runs
with each later `1.x` release, and you do not need to link it again.

## Source compatibility

Code that compiles with `1.0` compiles without changes with each later
`1.x` release. In `1.x`, no item in an installed header is removed, renamed
or given a different meaning:

- No function or macro is removed.
- No parameter changes its type or its position.
- No enumerator changes its value.
- No field of a public struct is removed or put in a different order.

## Binary compatibility

A program that you linked with `libccollections.so.1` runs with each later
`1.x` release. In `1.x`:

- No exported symbol is removed, and no exported symbol changes its
  signature.
- No public struct changes its size or its layout. This also forbids a
  change that does not look dangerous. In `1.x`, a configuration struct that
  you fill by value never gets an added field. Examples are
  `chttpsvr_config_t`, `chttp_tls_config_t`, `clog_rotation_cfg_t` and
  `clog_async_cfg_t`. An added field changes the size of the struct. This
  breaks each program that allocates one. Such a struct gets larger only in
  a new major version.
- No enumerator changes its numeric value. The library sets each
  `ccol_retval_t` value explicitly, and `ccol_success` is `0`.

The names of some exported symbols start with an underscore. Do not call these
symbols by name. They exist because the typed macros expand into calls to
them. In one case, a macro expands into a reference to a link-time marker
object. Therefore, your compiled code refers to these symbols directly. They
have the same guarantee as all other exported symbols.

## Behavioral compatibility

The promise applies to the behavior, not only to the form of the interface.
In `1.x`, these items do not change:

- The `ccol_retval_t` that an input gives, and the conditions that are
  fatal (the library calls `ccol_fatal_err()` and does not return).
- The operations that make an iterator or a pointer to an element invalid.
- The ownership rules for keys, values and nodes. This includes which calls
  give you a borrowed reference and which calls take ownership.
- The order in which registered callbacks run, and the thread that runs
  each callback.
- When the library calls an allocator that you gave.
- The documented thread-safety class of each type.

Performance is intentionally not on this list. A `1.x` release can replace
an algorithm or a data structure with a faster one, if all items above stay
true.

## What the promise does not cover

- Items that no installed header declares. The library does not install the
  internal headers, and they export nothing. They can change at any time.
- Items that are available only when `RUNNING_UNIT_TESTS` is defined. These
  helpers are only for the test suites of the project.
- The exact text of log output and of error strings. Also the whitespace of
  the serializers, where the format does not set it.
- The list of the transitive dependencies of the static archive. This list
  follows what OpenSSL and zlib need on the machine that builds the archive.
- A reduced build that you make with the `WITH_*` switches (see
  [Building](building.md#leaving-modules-out)). It has the same SONAME but
  it exports fewer symbols. Therefore, it cannot replace a full build.

## How the promise is checked

The usual test suites cannot see most of these properties. Each suite
compiles the sources of the library directly into its test program. There,
a symbol that the library does not export links in the same way as an
exported symbol. Therefore, CI runs separate checks on each push and each pull
request:

```bash
make check_namespace   # each exported symbol, public macro, typedef,
                       # enumerator and struct/union/enum tag has
                       # its namespace prefix
make check_abi         # the exported ABI matches the baseline under abi/
make check_dso_unload  # fork() works after a dlopen(), a use and a
                       # dlclose() of the library
```

`check_abi` compares two items with the recorded baseline:

- The list of exported symbols. This list is the same on all architectures.
- A structural record of the function signatures and the layouts of the
  public structs. The project records this item for each architecture
  (x86-64, i386, AArch64, ARM32 and FreeBSD x86-64). The reason is that a
  change can keep all structs the same on one target and move all fields on
  a different target.

The check reads the target architecture from the built library. Therefore, a
cross build uses the correct baseline. The structural comparison needs
libabigail (`abidw`, `abidiff`). If libabigail is not available, or if the
architecture has no recorded baseline, the check tells you that the
structural part did not run.

Each difference causes the check to fail. Examples are a removed symbol, a
symbol that is not in the record, and a change of a signature or a layout.
The check does not try to find which differences are safe. An addition to
the public API is a usual, compatible change. Record the updated baseline
with `make update_abi_baseline`. Commit it together with the change.

A corpus of tests, `tests/mixed/tests_compat.c`, checks the behavior. It
runs as part of `make test`. It tests the promises above directly:

- A failed attach leaves the ownership where the documentation of each
  module says.
- An eviction callback runs on the thread that caused the eviction, before
  that call returns.
- A completion callback runs on a worker, after its task.
- A reference to one key stays valid when you find or update other keys.
- A container that uses your allocator gets each byte from it and gives each
  byte back to it.

The corpus tests only documented promises. It compares return values with
numeric literals, not with the constants of the library. For the allocator
tests, it uses an allocator that refuses to free a block that it did not
give.

## Reference

[ccollections(7)](../man/common/ccollections.7) (the return codes and their values)

Related guides:
[Building and linking](building.md),
[Testing, fuzzing and benchmarks](testing.md),
[How the library works](design.md)
