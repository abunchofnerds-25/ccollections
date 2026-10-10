# Security Policy

## Supported versions

Security problems are corrected in the current `1.x` release line, and only
the newest `1.x` patch release receives the correction. Once a newer patch
release exists, an older one gets no extended support.

The SONAME of the shared library is `libccollections.so.1`, so every `1.x`
release is a direct replacement for any earlier one, and you never have to
relink your application to apply a security update.

## Reporting a vulnerability

Report a possible vulnerability privately. Do not open a public issue, a pull request or a discussion thread for it.

There are two private channels:

1. **Private vulnerability reporting on GitHub.** This is the channel we
   prefer. Open
   <https://github.com/abunchofnerds-25/ccollections/security/advisories/new>
   and write the report there. Only the maintainers can see it, and it gives
   us a private fork in which we can write and review the correction.
2. **Email.** Write to
   [abunchofnerds84@gmail.com](mailto:abunchofnerds84@gmail.com) and put
   `SECURITY` at the start of the subject line.

We try to answer within 10 days. If you hear nothing in that time, open a
public issue that says only that you are waiting for a security response,
without any detail about the finding, and we will answer you through a private
channel.

## What to include

A report helps us most when it holds enough for us to reproduce the behavior
without guessing:

- The affected module or modules, for example `cyaml`, `chttp1_parser`, or
  `chttpserver`.
- The version of the library, or the commit hash if you build from a checkout.
- The compiler and its version, the architecture, and the build flags.
- Something that reproduces the problem. The best forms are a `main()` that
  drives the public API or, for a parser finding, a raw input file. A fuzzer
  artefact (a file named `crash-*`, `oom-*` or `timeout-*`) also works on its
  own, because each one replays directly against its harness. Each harness
  lives in the test directory of the module that owns the target; the
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
- Resource exhaustion where the cost to a remote peer is much smaller than the
  cost to us, such as an unbounded allocation from a bounded input, a parser
  whose run time grows faster than the size of its input, or a connection that
  can hold a worker or the reactor thread for ever.
- Missing or incorrect TLS verification, certificate handling that accepts
  what it must refuse, and a configuration that silently ends up weaker than
  the one you asked for.
- A `Location` that adds the `http+unix` transport to a chain, or re-points it
  at another socket. `chttpclient` refuses both unconditionally, because
  otherwise any http or https server would get a request-forgery primitive
  against every local socket that the calling process can reach, so any way
  past that rule is in scope.
- A way past `chttp_request_t.prevent_tls_downgrade_on_redirect` when a caller
  has set it: with that field set, a chain that starts on `https` must stay on
  `https`.
- A data race or a deadlock in any module that we document as thread-safe.

The following findings are out of scope, because each one is documented,
intended behavior rather than a defect:

- `ccol_fatal_err()` stops the process on a programming error, for example a
  type that does not match in a container macro, or a key passed to a `_get`
  macro that is not in the container. This is the documented contract of the
  macro layer; the raw function layer returns a `ccol_retval_t` instead and
  never stops the process.
- Anything that needs a `fork()` while a `cthreadpool`, `cthreadcomm`,
  `clogger`, `chttpserver` or `chttpclient` handle created before that
  `fork()` is live, with the child then continuing to run without an `exec()`
  in between. We do not support that pattern (see `doc/concurrency.md` and
  `ccollections(7)`). We do support a `fork()` before you create a handle, and
  a `fork()` that is immediately followed by an `exec()`.
- Undefined behavior that a caller causes by breaking a documented
  precondition, such as using a key whose type does not match the type given
  when the container was constructed.
- `chttpclient` following a redirect from `https` to `http` while
  `chttp_request_t.prevent_tls_downgrade_on_redirect` is left at its default
  of false. That default matches curl, whose `CURLOPT_REDIR_PROTOCOLS` permits
  both schemes, and the Go `net/http` client. Set the field wherever a
  downgrade is not an acceptable outcome for your deployment.


If you are not sure which side of that line your finding falls on, report it
privately and we will decide together. An out-of-scope report costs us far
less than a real finding that somebody discloses in public.

## Disclosure

We try to answer a report within 10 days and to send you an assessment within
30 days. When a correction ships, the release notes and `CHANGELOG.md` record
the finding and name the reporter, unless the reporter asks us not to.

Disclosure dates are agreed with you; we do not set them on our own.

## Validating your own build

The security tests of this project run on every push and every pull request,
and you can also run all of them on your own machine:

```bash
make memtest                                                  # Valgrind, every suite
make CC=clang EXTRA_CFLAGS="-fsanitize=address,undefined \
    -fno-sanitize-recover=undefined" test                     # ASan + UBSan
cd tests/cyaml && make fuzz && ./fuzz_cyaml fuzz/corpus       # libFuzzer
```

ThreadSanitizer, the i386 and ARM cross-builds, the coverage threshold, the
namespace check and the ABI check all run in CI as ordinary jobs that block a
merge. `doc/testing.md` has the full list and the steps that run each one.
