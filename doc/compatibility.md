# Compatibility and versioning

This page is about what stays the same when you upgrade c_collections:
what a release is allowed to change, what a major version keeps fixed, and
how we check that we keep the promise.

## Version numbers

Every release has a number of the form `MAJOR.MINOR.PATCH`:

| Change | Field that increases | SONAME |
|---|---|---|
| A correction with no interface change | `PATCH` | does not change |
| An added function, macro or type | `MINOR` | does not change |
| A removal, or a change to an interface | `MAJOR` | changes |

The SONAME only holds `MAJOR`, so every `1.x` release is
`libccollections.so.1`, and a program built against one `1.x` release runs
with every later `1.x` release without a relink.

## Source compatibility

Code that compiles against `1.0` compiles as it is against every later
`1.x` release. Within `1.x` nothing in an installed header is removed,
renamed or given a different meaning, so:

- No function or macro is removed.
- No parameter changes its type or its position.
- No enumerator changes its value.
- No field of a public struct is removed or reordered.

## Binary compatibility

A program linked against `libccollections.so.1` runs with every later
`1.x` release, because within `1.x`:

- No exported symbol is removed, and no exported symbol changes its
  signature.
- No public struct changes its size or its layout. That rules out a change
  that looks harmless too: a configuration struct that you fill by value,
  like `chttpsvr_config_t`, `chttp_tls_config_t`, `clog_rotation_cfg_t` or
  `clog_async_cfg_t`, never gets a new field in `1.x`. A new field changes
  the size of the struct and breaks every program that allocates one, so
  such a struct only grows in a new major version. That limits how the API
  can grow, but it's the price of letting you declare and fill these
  structs yourself.
- No enumerator changes its numeric value. Every `ccol_retval_t` value is set
  explicitly, and `ccol_success` is `0`.

Some exported symbols have names that start with an underscore. Don't call
them by name. They are exported because the typed macros expand into calls
to them (and in one case into a reference to a link-time marker object), so
your compiled code refers to them directly, and they have the same
guarantee as every other exported symbol.

## Behavioral compatibility

The promise is about behavior as well as the shape of the interface.
Within `1.x` none of these change:

- The `ccol_retval_t` that a given input produces, and which conditions are
  fatal (the library calls `_ccol_fatal_err()` and does not return).
- Which operations invalidate an iterator or a pointer to an element.
- The ownership rules for keys, values and nodes, including which calls hand
  you a borrowed reference and which calls take ownership.
- The order in which registered callbacks run, and the thread that runs each
  one.
- When the library calls an allocator that you supplied.
- The documented thread-safety class of each type.

Performance is left off this list on purpose. A `1.x` release may replace
an algorithm or a data structure with a faster one, as long as everything
above stays true.

## What the promise does not cover

- Anything that no installed header declares. The internal headers aren't
  installed and export nothing, so they can change at any time.
- Anything that only exists when `RUNNING_UNIT_TESTS` is defined. Those
  helpers are declared in internal headers and only the project's own test
  suites use them.
- The exact text of log output and error strings, and the whitespace of the
  serializers where the format doesn't fix it. If your program has to react
  to a failure, test the return code and not the text of the message.
- The transitive dependencies of the static archive, which follow whatever
  OpenSSL and zlib need on the machine that builds it.
- A reduced build made with the `WITH_*` switches (see
  [Building](building.md#leaving-modules-out)). It has the same SONAME but
  exports fewer symbols, so it can't replace a full build.

## How the promise is checked

The ordinary test suites can't see most of these properties. Each suite
compiles the library sources straight into its test program, and there a
symbol that the library doesn't export links just like an exported one. So
CI runs separate checks on every push and every pull request:

```bash
make check_namespace   # each exported symbol, public macro, typedef,
                       # enumerator and struct/union/enum tag has
                       # its namespace prefix
make check_abi         # the exported ABI matches the baseline under abi/
make check_layout      # each public struct and union has the recorded
                       # LP64 layout on this system
make check_dso_unload  # fork() works after a dlopen(), a use and a
                       # dlclose() of the library
```

`check_abi` compares the list of exported symbols, which is the same on
every architecture, and a structural record of the function signatures and
the layouts of the public structs against the recorded baseline. The
structural record is kept per architecture (x86-64, i386, AArch64, ARM32,
s390x and FreeBSD x86-64), because one change can leave every struct as it
is on one target and move every field on another.

The check reads the target architecture from the built library, so a cross
build is compared against the right baseline. The structural comparison
needs libabigail (`abidw`, `abidiff`), and when libabigail is missing or the
architecture has no recorded baseline, the check says that the structural
part didn't run.

Any difference fails the check, a removed symbol, a symbol missing from the
record or a changed signature or layout alike, and the check doesn't try to
decide which differences are safe. That's stricter than it has to be, but
nobody then has to trust a guess about what a program can survive. When the
difference is an addition to the
public API, which is a normal compatible change, you record the new baseline
with `make update_abi_baseline` and commit it together with the change.

The structural comparison reads ELF debug information, so it can't run on
macOS, and OpenBSD has no libabigail. `check_layout` covers the layouts on
those two. Every public struct and union is built only from fixed-width
integers, `size_t`, `int`, `unsigned`, `_Bool`, enums, pointers and other
public records, which gives it one layout on every LP64 system, and
`abi/public_layout.lp64` records that layout with the size, the alignment
and the offset of every member. `check_layout` asks the system's clang for
the same facts and fails on any difference, so a member whose width depends
on the system (`long double`, a pthread type, `time_t`) fails on the first
system where it differs. CI runs it on Linux, FreeBSD, OpenBSD and macOS,
and the ILP32 targets are covered by the structural comparison of
`check_abi`. After a deliberate change to a public struct, record the layout
again with `make update_layout_baseline` and commit it with the change.

Behavior is checked by a corpus of tests, `tests/mixed/tests_compat.c`,
which runs as part of `make test` and tests the promises above directly:

- A failed attach leaves ownership where each module's documentation says it
  goes.
- An eviction callback runs on the thread that caused the eviction, before
  that call returns.
- A completion callback runs on a worker, after its task.
- A reference to one key stays valid while you find or update other keys.
- A container that uses your allocator takes every byte from it and gives
  every byte back to it.

The corpus only tests documented promises. It compares return values with
numeric literals instead of the library's own constants, and its allocator
tests use an allocator that refuses to free a block it didn't hand out.

## Reference

[ccollections(7)](../man/common/ccollections.7) (the return codes and their values)

Related guides:
[Building and linking](building.md),
[Testing, fuzzing and benchmarks](testing.md),
[How the library works](design.md)
