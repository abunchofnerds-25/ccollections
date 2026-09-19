# Security Policy

## Supported versions

Security fixes are made against the current `1.x` release line. The newest
`1.x` patch release is the only version that receives them; there is no
extended support for an older patch release once a newer one exists.

The shared library's SONAME is `libccollections.so.1`, so every `1.x` release
is a drop-in replacement for any earlier one. Applying a security update never
requires relinking.

## Reporting a vulnerability

Please report suspected vulnerabilities privately. Do not open a public issue,
a pull request, or a discussion thread for one.

Two private channels are available:

1. **GitHub private vulnerability reporting** (preferred). Open
   <https://github.com/abunchofnerds-25/ccollections/security/advisories/new>
   and file the report there. It is visible only to the maintainers, and it
   gives us a private fork to develop and review the fix in.
2. **Email.** `abunchofnerds84@gmail.com`, with `SECURITY` somewhere in the
   subject line.

If you do not receive an acknowledgement within 7 days, please open a public
issue that says only that you are waiting on a security response, with no
detail about the finding itself, and we will follow up through a private
channel.

## What to include

A report is most useful when it carries enough for us to reproduce the
behavior without guessing:

- The affected module or modules (for example `cyaml`, `chttp1_parser`,
  `chttpserver`).
- The library version, or the commit hash if you are building from a checkout.
- The compiler, its version, the architecture, and the build flags.
- A reproducer. A `main()` that drives the public API, or a raw input file for
  a parser finding, is ideal. A fuzzer artefact (`crash-*`, `oom-*`,
  `timeout-*`) replays directly against the matching harness, which lives in
  the test directory of the module that owns the target (the `chttp1_parser`
  harnesses live under `tests/chttpclient/`), so attaching one is enough on
  its own.
- Any sanitizer or Valgrind output you have.

## What we consider a vulnerability

The library parses untrusted input and terminates network connections, so the
following are in scope:

- Memory-safety faults (out-of-bounds access, use-after-free, double free,
  uninitialized reads) reachable from any public API.
- Faults reachable from attacker-controlled bytes in `cjson`, `cyaml`, or the
  HTTP/1.1 parser behind `chttpclient` and `chttpserver`.
- Resource exhaustion that a remote peer can trigger disproportionately to the
  work it performs: unbounded allocation from a bounded input, a parser whose
  running time grows superlinearly in input size, a connection that can hold a
  worker or the reactor thread indefinitely.
- Missing or incorrect TLS verification, certificate handling that accepts
  what it should reject, or a configuration that silently ends up weaker than
  what was asked for.
- Data races and deadlocks in any module documented as thread-safe.

The following are out of scope, because they are documented, intended
behavior rather than defects:

- `ccol_fatal_err()` terminating the process on a programming error, such as a
  type mismatch in a container macro or a missing key passed to a `_get`
  macro. This is the documented contract of the macro layer; the raw function
  layer returns a `ccol_retval_t` instead and never terminates.
- Anything that requires calling `fork()` while a `cthreadpool`,
  `cthreadcomm`, `clogger`, or `chttpserver` handle created before that
  `fork()` is still live, and then continuing to run in the child without an
  intervening `exec()`. That pattern is explicitly unsupported; see the
  `fork()` discussion in `README.md`. Forking before any handle is created, or
  pairing `fork()` with an immediate `exec()`, is supported.
- Undefined behavior caused by a caller violating a documented precondition,
  such as passing a key whose type does not match the one the container was
  constructed with.

If you are unsure which side of that line a finding falls on, report it
privately and we will work it out together. A report that turns out to be out
of scope costs us far less than a real finding disclosed publicly.

## Disclosure

We aim to acknowledge a report within 7 days and to have an assessment back to
you within 30. When a fix ships, the release notes and `CHANGELOG.md` record
the finding and credit the reporter, unless the reporter asks otherwise.

We will coordinate the disclosure timeline with you rather than imposing one.

## Validating your own build

The project's own security testing runs on every push and every pull request,
and you can run all of it locally:

```bash
make memtest                                                  # Valgrind, every suite
make CC=clang EXTRA_CFLAGS="-fsanitize=address,undefined \
    -fno-sanitize-recover=undefined" test                     # ASan + UBSan
cd tests/cyaml && make fuzz && ./fuzz_cyaml fuzz/corpus       # libFuzzer
```

ThreadSanitizer, the i386 and ARM cross-builds, the coverage threshold, and
the namespace and ABI checks all run in CI as ordinary blocking jobs. See
`README.md` for the full list and for how to run each one.
