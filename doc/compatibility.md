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

The SONAME only holds `MAJOR`, so every `2.x` release is
`libccollections.so.2`, and a program built against one `2.x` release runs
with every later `2.x` release without a relink.

## Source compatibility

Code that compiles against `2.0` compiles as it is against every later
`2.x` release. Within `2.x` nothing in an installed header is removed,
renamed or given a different meaning, so:

- No function or macro is removed.
- No parameter changes its type or its position.
- No enumerator changes its value.
- No field of a public struct is removed or reordered.

## Binary compatibility

A program linked against `libccollections.so.2` runs with every later
`2.x` release, because within `2.x`:

- No exported symbol is removed, and no exported symbol changes its
  signature.
- No public struct changes its size or its layout. That rules out a change
  that looks harmless too: A configuration struct that you fill by value,
  like `chttpsvr_config`, `chttp_tls_config`, `clog_rotation_cfg` or
  `clog_async_cfg`, never gets a new field in `2.x`. A new field changes
  the size of the struct and breaks every program that allocates one, so
  such a struct only grows in a new major version. That limits how the API
  can grow, but it's the price of letting you declare and fill these
  structs yourself.
- No enumerator changes its numeric value. Every `ccol_retval` value is set
  explicitly, and `ccol_success` is `0`.

Some exported symbols have a double underscore after their prefix, such as
`cvector__destroy` or `csort__merge_sort`. That's how the library marks a
name as internal. Don't call them by name. They are exported because the
typed macros expand into calls to them (and in one case into a reference to
a link-time marker object), so your compiled code refers to them directly,
and they have the same guarantee as every other exported symbol. No name of
the library, public or internal, begins with an underscore or takes any
other form that ISO C reserves for the compiler and the C library, and none
ends in `_t` or `_MAX`, which POSIX reserves for the system's headers, so no
macro, type or symbol of theirs can stand in for one of ours.

Every exported symbol carries the version node `CCOL_2`. Your program
records each symbol it uses together with that node, for example
`cvector_push_back@CCOL_2`, and the dynamic loaders of glibc and FreeBSD
bind such a reference only to a definition with the same node. Another
versioned library that exports a function of the same name can't take the
call. A library without any version information can, which is how ELF
loaders keep old binaries working, and musl and OpenBSD don't look at
versions at all. The library binds its own calls to its exported functions
when it's linked, so on every system an object loaded ahead of it can't
take over a call that the library makes itself. On macOS a program records
the library of every symbol it imports, which gives the same protection
without versions.

## Behavioral compatibility

The promise is about behavior as well as the shape of the interface.
Within `2.x` none of these change:

- The `ccol_retval` that a given input produces, and which conditions are
  fatal (the library calls `ccol__fatal_err()` and does not return).
- Which operations invalidate an iterator or a pointer to an element.
- The ownership rules for keys, values and nodes, including which calls hand
  you a borrowed reference and which calls take ownership.
- The order in which registered callbacks run, and the thread that runs each
  one.
- When the library calls an allocator that you supplied.
- The documented thread-safety class of each type.

Performance is left off this list on purpose. A `2.x` release may replace
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
                       # its namespace prefix, and no name in the
                       # headers or sources is one that ISO C
                       # reserves or the compiler predefines
make check_abi         # the exported ABI matches the baseline under abi/
make check_layout      # each public struct and union has the recorded
                       # LP64 layout on this system
make check_dso_unload  # fork() works after a dlopen(), a use and a
                       # dlclose() of the library
make check_symbol_binding  # every exported symbol carries the
                           # version node, and a preloaded library
                           # that defines the same functions catches
                           # no call of the library (and, where the
                           # loader reads versions, none of the
                           # program)
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

[ccollections(7)](https://abunchofnerds-25.github.io/ccollections/man/common/ccollections.7.html) (the return codes and their values)

Related guides:
[Building and linking](building.md),
[Testing, fuzzing and benchmarks](testing.md),
[How the library works](design.md)
