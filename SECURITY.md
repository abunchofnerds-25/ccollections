# Security Policy

## Supported versions

We fix security problems in the current `1.x` release line, and the fix
only goes into the newest `1.x` patch release. As soon as a newer patch
release exists, the older one gets no extended support.

The SONAME of the shared library is `libccollections.so.1`, so every `1.x`
release drops in for any earlier one and you never have to relink your
application to apply a security update.

## Reporting a vulnerability

Please report a possible vulnerability privately, and don't open a public
issue, a pull request or a discussion thread for it.

There are two private channels:

1. Private vulnerability reporting on GitHub, which is the one we prefer.
   Open
   <https://github.com/abunchofnerds-25/ccollections/security/advisories/new>
   and write the report there. Only the maintainers can see it, and it gives
   us a private fork where we can write and review the fix.
2. Email to
   [abunchofnerds84@gmail.com](mailto:abunchofnerds84@gmail.com), with
   `SECURITY` at the start of the subject line.

We try to answer within 10 days. If you hear nothing in that time, open a
public issue that only says you are waiting for a security response, with no
detail about the finding, and we'll get back to you through a private
channel.

## What to include

A report helps us most when it has enough in it for us to reproduce the
behavior without guessing:

- The affected module or modules, for example `cyaml`, `chttp1_parser`, or
  `chttpserver`.
- The version of the library, or the commit hash if you build from a checkout.
- The compiler and its version, the architecture, and the build flags.
- Something that reproduces the problem. The best is a `main()` that
  drives the public API or, for a parser finding, a raw input file. A fuzzer
  artefact (a file named `crash-*`, `oom-*` or `timeout-*`) works by itself
  too, because each one replays directly against its harness. Each harness
  is in the test directory of the module that owns the target, and the
  `chttp1_parser` harnesses are under `tests/chttpclient/`.
- Any sanitizer or Valgrind output that you have.

## What we consider a vulnerability

The library parses untrusted input and terminates network connections, so
these findings are in scope:

- A memory safety fault that is reachable from any public API: an access
  outside the bounds of an object, a use after free, a double free, or a read
  of uninitialized memory.
- A fault that is reachable from attacker-controlled bytes in `cjson`, in
  `cyaml`, or in the HTTP/1.1 parser behind `chttpclient` and `chttpserver`.
- Resource exhaustion where the cost to a remote peer is much smaller than
  the cost to us, like an unbounded allocation from a bounded input, a
  parser whose run time grows faster than the size of its input, or a
  connection that can hold a worker or the reactor thread for ever.
- Missing or wrong TLS verification, certificate handling that accepts what
  it must refuse, and a configuration that silently ends up weaker than the
  one you asked for.
- A `Location` that adds the `http+unix` transport to a chain, or points it
  at another socket. `chttpclient` refuses both unconditionally, because
  otherwise any http or https server would get a request-forgery primitive
  against every local socket the calling process can reach, so any way past
  that rule is in scope.
- A way past `chttp_request_t.prevent_tls_downgrade_on_redirect` when a
  caller has set it. With that field set, a chain that starts on `https` must
  stay on `https`.
- A data race or a deadlock in any module that we document as thread-safe.

These findings are out of scope, because each of them is documented,
intended behavior and not a defect:

- `ccol_fatal_err()` stops the process on a programming error, for example a
  type that doesn't match in a container macro, or a key passed to a `_get`
  macro that isn't in the container. That's the documented contract of the
  macro layer, and the raw function layer returns a `ccol_retval_t` instead
  and never stops the process.
- Anything that needs a `fork()` while a `cthreadpool`, `cthreadcomm`,
  `clogger`, `chttpserver` or `chttpclient` handle created before that
  `fork()` is live, with the child then going on without an `exec()` in
  between. We don't support that pattern (see `doc/concurrency.md` and
  `ccollections(7)`). We do support a `fork()` before you create a handle,
  and a `fork()` that is followed right away by an `exec()`.
- Undefined behavior that a caller causes by breaking a documented
  precondition, like using a key whose type doesn't match the type given
  when the container was constructed.
- `chttpclient` following a redirect from `https` to `http` while
  `chttp_request_t.prevent_tls_downgrade_on_redirect` is left at its default
  of false. That default matches curl, whose `CURLOPT_REDIR_PROTOCOLS`
  permits both schemes, and the Go `net/http` client. Set the field wherever
  a downgrade isn't acceptable for your deployment.

If you aren't sure which side of that line your finding is on, report it
privately and we'll decide together. An out-of-scope report costs us much
less than a real finding that somebody discloses in public.

## Disclosure

We try to answer a report within 10 days and send you an assessment within
30 days. When a fix ships, the release notes and `CHANGELOG.md` record the
finding and name the reporter, unless the reporter asks us not to.

We agree the disclosure dates with you and never set them on our own.

## Validating your own build

The security tests of this project run on every push and every pull
request, and you can run all of them on your own machine too:

```bash
make memtest                                                  # Valgrind, every suite
make CC=clang EXTRA_CFLAGS="-fsanitize=address,undefined \
    -fno-sanitize-recover=undefined" test                     # ASan + UBSan
cd tests/cyaml && make fuzz && ./fuzz_cyaml fuzz/corpus       # libFuzzer
```

ThreadSanitizer, the i386 and ARM cross-builds, the coverage threshold,
the namespace check and the ABI check all run in CI as ordinary jobs that
block a merge. `doc/testing.md` has the full list and how to run each one.
