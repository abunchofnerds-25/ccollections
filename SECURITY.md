# Security Policy

## Supported versions

We correct a security problem in the current `1.x` release line. The newest
`1.x` patch release is the only version that gets such a correction. We give no
extended support to an older patch release after a newer one exists.

The SONAME of the shared library is `libccollections.so.1`. Every `1.x` release
is therefore a direct replacement for any earlier one. You never have to link
your application again to apply a security update.

## Reporting a vulnerability

Report a possible vulnerability privately. Do not open a public issue, a pull
request, or a discussion thread for one.

You can use two private channels:

1. **Private vulnerability reporting on GitHub.** We prefer this channel. Open
   <https://github.com/abunchofnerds-25/ccollections/security/advisories/new>
   and write the report there. Only the maintainers can see it. It also gives
   us a private fork, where we can write and review the correction.
2. **Email.** Write to
   [abunchofnerds84@gmail.com](mailto:abunchofnerds84@gmail.com). Put
   `SECURITY` at the start of the subject line.

We try to answer within 10 days. If you get no answer in that time, open a
public issue. In that issue, write only that you wait for a security response.
Do not write any detail about the finding. We then answer you through a private
channel.

## What to include

A report helps us most when it holds enough for us to reproduce the behavior
without a guess:

- The module or the modules with the problem, for example `cyaml`,
  `chttp1_parser`, or `chttpserver`.
- The version of the library. If you build from a checkout, give the commit
  hash instead.
- The compiler, its version, the architecture, and the build flags.
- Something that reproduces the problem. The best forms are a `main()` that
  drives the public API, or, for a finding in a parser, a raw input file. A
  fuzzer artefact also works on its own. Those files are named `crash-*`,
  `oom-*` or `timeout-*`, and each one replays directly against its harness.
  Each harness is in the test directory of the module that owns the target.
  The `chttp1_parser` harnesses are under `tests/chttpclient/`.
- Any output that you have from a sanitizer or from Valgrind.

## What we consider a vulnerability

The library parses input that you cannot trust, and it ends network
connections. These findings are therefore in scope:

- A memory safety fault that you can reach from any public API. This includes
  an access outside the bounds of an object, a use after free, a double free,
  and a read of memory that nothing initialized.
- A fault that you can reach from bytes that an attacker controls, in `cjson`,
  in `cyaml`, or in the HTTP/1.1 parser behind `chttpclient` and
  `chttpserver`.
- Resource exhaustion where the cost to a remote peer is much smaller than the
  cost to us. Examples are an allocation with no bound from an input with a
  bound, a parser whose run time grows faster than the size of the input, and
  a connection that can hold a worker or the reactor thread for ever.
- A missing or incorrect TLS verification. Also certificate handling that
  accepts what it must refuse, and a configuration that becomes weaker than
  the one you asked for, with no report.
- A `Location` that adds the `http+unix` transport to a chain, or re-points it
  at another socket. `chttpclient` refuses both unconditionally, because
  otherwise any http or https server gets a request-forgery primitive against
  every local socket the calling process can reach. A way past that rule is in
  scope.
- A way past `chttp_request_t.prevent_tls_downgrade_on_redirect` when a caller has set
  it. With that field set, a chain that starts on `https` must stay on
  `https`.
- A data race or a deadlock in any module that we document as thread-safe.

These findings are out of scope. Each one is documented, intended behavior and
not a defect:

- `ccol_fatal_err()` stops the process on a programming error. Two examples are
  a type that does not match in a container macro, and a key that is not in the
  container passed to a `_get` macro. This is the documented contract of the
  macro layer. The raw function layer returns a `ccol_retval_t` instead and
  never stops the process.
- Anything that needs a call to `fork()` while a `cthreadpool`, `cthreadcomm`,
  `clogger`, `chttpserver` or `chttpclient` handle from before that `fork()`
  is live, and that then continues to run in the child with no `exec()`
  between. We do not support that pattern. See `doc/concurrency.md` and
  `ccollections(7)`. We support a `fork()` before you create a handle. We
  also support a `fork()` with an immediate `exec()` after it.
- Undefined behavior that a caller causes when it breaks a documented
  precondition. One example is a key whose type does not match the type that
  you gave when you constructed the container.
- `chttpclient` following a redirect from `https` to `http` with
  `chttp_request_t.prevent_tls_downgrade_on_redirect` left at its default of false. That
  default matches curl, whose `CURLOPT_REDIR_PROTOCOLS` permits both schemes,
  and the Go `net/http` client. Set the field where a downgrade is not an
  acceptable outcome for your deployment.


If you do not know which side of that line your finding is on, report it
privately. We will then decide together. A report that is out of scope costs us
much less than a real finding that somebody discloses in public.

## Disclosure

We try to answer a report within 10 days, and to send you an assessment within
30 days. When a correction ships, the release notes and `CHANGELOG.md` record
the finding and name the reporter. We do not name a reporter who asks us not
to.

We agree the disclosure dates with you. We do not set them ourselves.

## Validating your own build

The security tests of this project run on every push and on every pull request.
You can also run all of them on your own machine:

```bash
make memtest                                                  # Valgrind, every suite
make CC=clang EXTRA_CFLAGS="-fsanitize=address,undefined \
    -fno-sanitize-recover=undefined" test                     # ASan + UBSan
cd tests/cyaml && make fuzz && ./fuzz_cyaml fuzz/corpus       # libFuzzer
```

ThreadSanitizer, the i386 and ARM cross-builds, the coverage threshold, the
namespace check and the ABI check all run in CI as ordinary jobs that block a
merge. See `doc/testing.md` for the full list, and for the steps that run
each one.
