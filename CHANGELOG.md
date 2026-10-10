# Changelog

This file records every important change to this project. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the version
numbers follow the compatibility rules in `doc/compatibility.md`.

Entries are grouped by the kind of change, in this order: `Added`, `Changed`,
`Deprecated`, `Removed`, `Fixed`, `Security`. A `Changed` or `Removed` entry
that changes the Application Binary Interface (ABI) names the ABI version
that it required.

## 1.0.0 - unreleased

This is the first release with a stable, supported interface. Because there
is no earlier release, the text below describes what `1.0.0` contains rather
than what changed.

The SONAME of the shared library is `libccollections.so.1`. Every later `1.x`
release keeps that SONAME and stays compatible with this release at both the
source level and the binary level.

### Added

- One unit for time across the whole public API: every timeout, deadline,
  interval and duration is a `uint64_t` count of microseconds, and its name
  ends in `_us`. That covers the timed calls of `cthreadcomm` and
  `cthreadpool`, `ccol_select_timed()`, `chttpclient_set_connect_timeout()`
  and `chttpclient_set_request_timeout()`, every duration of
  `chttpsvr_config_t`, and `flush_interval_us` and `rotation_interval_us` of
  `clogger`. No public signature takes a `struct timespec` or a `time_t`, so
  an application and the library never have to agree on the width of
  `time_t`. A value too large to form a deadline saturates and never wraps
  into the past.
- Containers: `cvector`, a dynamic array; `chashmap`, a hash map that uses
  open addressing for integral key and value types and separate chaining for
  every other type; `cbstmap`, an ordered AVL map; and `citerators`, one
  iteration API that all three share.
  - `chmap_create_full()`, `chmap_init_full()` and `chmap_construct_full()`
    take an optional key equality function of type
    `ccol_key_equality_proc_t` beside the custom hash function, so a struct
    key with padding bytes can compare by its members.
  - An open-addressing `chashmap` deletes by backward shift and leaves no
    marker for a deleted slot, so only live entries decide a resize, and a
    steady stream of inserts and deletes never rebuilds the table.
  - `cvec_sort()` sorts the backing array in place.
  - `cvec_push()` accepts each expression whose type converts to the element
    type: a variable, a literal, a computed value, or a struct value that a
    function returns.
  - An iterator that a loop leaves early can outlive its container until the
    end of its scope: the scope-exit cleanup frees it without reading the
    container.
  - `cvec_find()`, `cvec_sort()` and `csort_get_default_comparison_proc()`
    treat a character pointer that is itself `const` or `volatile`, such as
    `const char *const`, as a string.
- Strings and algorithms: `cstring`, a dynamic string; and `csort`, an
  iterative bottom-up mergesort. Its floating-point comparators put NaN
  above each other value, and make NaN equal only to NaN.
- Memory: `cmempool`, which gives you a fixed-size pool and a ranged pool,
  each built either from a heap allocation or from a preallocated buffer that
  you supply. A pool can fall back to dynamic allocation when it is empty,
  and it detects corruption and double frees. Every entry is aligned for any
  object type, the same guarantee that `malloc()` makes.
- Concurrency: `cthreadcomm`, which gives you circular queues, dynamic queues,
  channels, and a persistent `epoll(7)` reactor; `cthreadpool`, which gives
  you a bounded or an unbounded task queue, completion callbacks, and futures;
  and `clrucache`, a thread-safe LRU cache with an optional remote getter and
  setter.
  - A `ccol_event_loop` callback can run the documented drain, remove and
    destroy sequence for a different watched queue or channel, whatever
    `num_reactor_threads` is.
  - `ccol_select_timed()` with a timeout of 0 polls every selectable without
    blocking, and no timed wait ends before its deadline.
  - A send to a queue whose event-loop listener already has a wake pending
    makes no system call, so a steady stream of messages costs one `write(2)`
    for each dispatch and not one for each message.
  - Inside an event-loop callback for a `ccol_selectable_from_chan()`
    selectable, the documented receive and send calls are
    `ccol_circq_try_recv_zc()` and `ccol_circq_try_send_zc()` on `sel->cq`.
  - Every timed call of `cthreadcomm` and `cthreadpool` takes its timeout as
    a `uint64_t` count of microseconds. A timeout of 0 does not wait: the
    call does exactly what its `try_` variant does. A timeout too large to
    form a deadline, such as `UINT64_MAX`, waits with no end instead of
    wrapping into the past. Each one measures its deadline on
    `CLOCK_MONOTONIC`, so a step of the wall clock does not change the wait.
    `ccol_select_timed()` takes microseconds too, and `UINT64_MAX` is its
    wait with no limit.
  - A `ctpool` keeps the task nodes of its recent traffic for reuse, at most
    32768 idle nodes per pool (more only for a pool with over 8192 workers),
    and halves what the traffic does not need at every 256th idle point.
- Serialization: `cjson` and `cyaml`, each a parser, a serializer and a
  mutable DOM with path-based get and set macros. `cyaml` also covers YAML
  1.2 tags, merge keys, and multi-document streams.
  - `cjson_set()` and `cyaml_set()` accept a `char` array, such as a buffer
    that `snprintf` filled, and a pointer that is itself `const`, such as
    `static const char *const NAME`, and store the text of the string.
  - `cyaml` stores a float dictionary key as the shortest decimal text that
    reads back as the same double, so `3.10:` is the key `"3.1"`. Both
    serializers write a key that is the canonical text of an integer or a
    float with no quotes.
  - A `cyaml` node never carries a core-schema tag of another type.
    `cyaml_node_set_tag()` refuses such a tag, and `cyaml_set()` drops a
    core-schema tag that the new value no longer matches, so every
    serialization parses back. A custom tag survives every `cyaml_set()`.
  - The `cyaml` parser accepts a flow collection whose closing `]` or `}`
    sits alone at the indentation of the key or the `- ` that opened it.
  - Both modules read and write numbers the same way whatever the
    `LC_NUMERIC` locale of the process is.
  - `cjson_set()` and `cyaml_set()` store an unsigned 64-bit value above
    `LLONG_MAX` as the nearest float, which is what each parser makes of the
    same literal.
  - A tag on an implicit `cyaml` block-mapping key types that key, as it does
    in flow context.
  - A `cjson` and a `cyaml` dictionary keep their members in insertion order,
    so a parse followed by a serialize keeps the key order of the source
    document, the same in every run, and a clone keeps the order of its
    source. Setting a key that is already there keeps its place.
    `cjson_dictionary_first()`, `cjson_dictionary_next()`,
    `cyaml_dictionary_first()` and `cyaml_dictionary_next()` walk the members
    in that order with no allocation, and the member under the cursor can be
    removed during the walk.
  - The `cyaml` parser accepts a tab wherever YAML 1.2 allows separation
    whitespace, so tab-indented JSON and `key:<TAB>value` parse, and it skips
    a byte order mark before every document of a stream. A tab used as block
    indentation is an error.
  - `cyaml` writes a float with a point in its mantissa (`1.0e+20`), in the
    shortest text that reads back as the same double, so YAML 1.1 readers
    such as PyYAML load it as a float.
  - The `cyaml` parse limits on nodes and on memory grow with the length of
    the input, so an honest document of any size parses. Alias expansion,
    merge copies and collection-key text draw from a separate budget of
    fixed size (4M nodes, 256 MiB), so padding a document never raises how
    far it may expand.
  - `cyaml_serialize_stream()` writes each element of a list as its own
    document, so a multi-document stream round-trips as a stream.
  - `cyaml` quotes every string that a YAML 1.1 reader (PyYAML, libyaml,
    Ruby Psych) would misread, and a byte order mark is accepted only before
    a document; one inside a document is a parse error.
  - Nesting depth counts containers: 500 nested containers are accepted and
    a 501st is refused, the same in parse, serialize and clone. A `cjson`
    parse error message is always printable ASCII.
- Logging: `clogger`, which writes logfmt, JSON or RFC 5424 syslog output and
  also gives you log rotation, derived loggers and optional asynchronous
  batching. Rotation retention always keeps the newest generations, and at
  most `max_rotated_files + 1` generations stay on disk. Compression runs on
  a background thread of each logger, so neither a logging call nor the
  asynchronous writer ever waits for gzip; `clog_close()` and a fatal record
  finish the queued compressions first. A rotating logger keeps working in
  the directory it was opened in after a `chdir()`. Time-based rotation
  counts its interval from when the current file began, taken from the
  newest rotated generation on disk, so a short-lived process that reopens
  the same log still rotates on schedule. An asynchronous logger on a
  datagram or a seqpacket socket sends one record per message. A write error
  that no retry can fix drops the pending batch and writes a record that
  names what was lost, so a destination that fails permanently never makes
  the batch grow without bound. Each thread reads its process ID, thread ID
  and thread name once, so a record costs no system call for them.
- `ccol_set_thread_name()`, which renames the calling thread and makes its
  next `clogger` record carry the new name.
- Every module accepts allocation functions that the caller supplies.
- Networking: `chttpclient`, which speaks HTTP/1.1 over TLS or plain text in a
  synchronous, an asynchronous, and a pooled-synchronous form; and
  `chttpserver`, which speaks HTTP/1.1 with method and pattern routing, named
  path parameters, middleware chains, sub-routers, streaming handlers, and
  optional TLS. Every final response of the server carries a `Date` header
  in UTC, as Go's `net/http` server does, unless the handler sets its own.
  The TLS server resumes sessions for TLS 1.2 and TLS 1.3, with and without
  verification of client certificates, and it answers every request of a
  pipeline that arrives in one TLS record. In all three tiers the client tries
  every address of a host name under one connect timeout (see the Happy
  Eyeballs entry below), so `http://localhost` reaches a server that listens
  only on `127.0.0.1`. Each tier reads a TLS response that the peer sends in
  records larger than one read without waiting on the socket. No TLS write of
  either module raises SIGPIPE, whatever the signal disposition of the
  application. A client response and an asynchronous result can be freed
  after their client is destroyed. A `405` of the server carries an `Allow`
  header, and accepted TCP connections keep the kernel's buffer autotuning.
  - `chttpclient_resp_header()` returns a field that a response repeats as
    one value joined with `, ` in wire order (RFC 9110 section 5.3), except
    `Set-Cookie`, which it never joins; `chttpclient_resp_header_count()` and
    `chttpclient_resp_header_at()` return every occurrence one by one.
  - `chttpsvr_resp_add_header()` adds another field line of a name, so a
    handler can send several `Set-Cookie` headers. A `304` carries a
    `Content-Length` only when the handler sets one.
  - The time that a parked request waits for a free worker is never charged
    to the client's transfer-rate floor or timers.
  - An engine logger that the application installs on the client or the
    server stays in use across every later stop and start of that engine.
  - A sub-router owns every path under its mount prefix: its middleware runs
    for every such request, and a path that none of its routes matches gets
    its 404 or 405 and never reaches a root route.
  - The server answers 505 to an HTTP major version other than 1, 501 to a
    transfer coding other than chunked, and 400 to a `Host` outside the RFC
    3986 grammar; it bounds chunk-extension bytes, and it counts a draining
    socket send queue as progress, so a steady reader of a large response is
    never cut off. `CHTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED` names 505.
  - The client accepts a response header line of up to 64 KiB, and an HTTPS
    program that uses the asynchronous or pooled tier leaves no engine thread
    running when it exits.
  - The server listens on IPv4 and IPv6 by default (a NULL host binds a
    dual-stack socket), `localhost` binds the IPv4 loopback, and a literal
    address binds exactly itself, as Go's `net.Listen` does. A `unix://`
    listener never removes a file that is not a socket or a socket that a
    live server holds, and it removes at stop only the socket it bound.
  - After a rejection, the server closes a connection whose request body is
    still arriving with a bounded lingering close, so the client reads the
    response instead of a connection reset.
  - With `Expect: 100-continue`, every client tier bounds only the wait for
    the first byte of an answer by the continue window, so a final response
    that arrives slowly is read in full and the body is never sent after it.
  - A process forked after its parent used the asynchronous or pooled client
    tier builds its own engine and never waits on the parent's threads.
  - The client reads an empty port in a URL (`http://host:/`) as the default
    port of the scheme, verifies a fully qualified host name with a trailing
    dot, and shares pooled connections between spellings of a host that
    differ only in letter case.
- The readable, writable and error callbacks of a `ccol_event_loop` receive
  their own registration handle, so a callback that runs before
  `ccol_event_loop_add()` has returned can remove or pause itself.
  `ccol_event_loop_destroy()` drains while the handle still resolves, so a
  callback that runs during the drain can remove its registrations before it
  frees their argument.
- The `on_complete` callback of every `ctpool` submit takes a `bool ran`
  argument and runs exactly once for every accepted task, with `ran` false
  for a task that `ctpool_shutdown_immediate()` discards, so it can always
  release what the task owns.
- An asynchronous `clogger` target delivers its queued records when the
  process calls `exit()` or returns from `main` without `clog_close()`, and a
  compressed generation is written under a temporary name and published only
  once complete, so no truncated `.gz` is ever left behind. A second open of
  a file that a rotating logger of the process already writes joins that
  logger.
- Every public destroy and free macro evaluates its argument exactly once,
  so an argument such as `handles[i++]` destroys and clears one element.
- A queue listener of a `ccol_event_loop` that moves one message per
  callback is woken again while its queue stays ready, so it drains a
  backlog without further sends.
- `CLOG_FATAL` always terminates the process, even when its own output or
  another logger's output has stopped draining; the exit drain bounds every
  wait, including writes to pipes and sockets. A rotated log file keeps the
  mode, owner and group of the file it replaces.
- `ccol_mutex_timedlock()` and `ccol_rw_lock_timedwrlock()`: timed lock
  wrappers in the set of thread-primitive wrappers of `common.h`.
- A `ccol_event_loop` registration that has no `on_error` handler receives an
  error or a hang-up of its descriptor through its read or write handler, as
  libevent and libuv deliver it, so a connected UDP socket keeps receiving
  after an ICMP error.
- Packaging: a shared library with a version and a SONAME, a static archive,
  `pkg-config` metadata that the build generates, support for `PREFIX`,
  `DESTDIR`, `LIBDIR`, `INCLUDEDIR`, `MANDIR` and `PKGCONFIGDIR` in
  `make install`, and 521 manual pages (510 in section 3 and 11 in section 7).
  The public headers install into `include/ccollections/` and include each
  other by relative name, and an application writes
  `#include <ccollections/NAME.h>`, so a header of the same name in the
  application never collides with one of the library's.
- `make check_namespace`, which stops the build when any of these items has
  no namespace prefix: an exported symbol, a public macro, a public typedef, an
  enumerator, or a struct, union or enum tag of an installed header.
- `make check_abi`, which compares the exported ABI against the committed
  baseline under `abi/` and fails on an addition that nobody recorded and on
  any removal.
- `make check_filenames`, which refuses a name under `include/` or `man/`
  that `make install` and `make uninstall` would paste onto the installation
  directory as a shell pattern rather than as one literal path, because such
  a name makes those recipes reach files that belong to other packages.
- `make bench`, a benchmark suite for the modules whose run-time cost is worth
  watching. It keeps a baseline for each machine and detects regressions
  against it, and where uthash, GLib, jansson and libyaml are installed, it
  can also compare against them.
- `CCOL_FORK_SAFETY_REQUIRED`, a compile-time switch for the fork protection
  that `cthreadpool`, `cthreadcomm`, `clogger` and `chttpserver` build on
  `pthread_atfork()`.
- `CCOL_MEMPOOL_COMPACT_LAYOUT`, a compile-time switch that gives you a stride
  the library does not round up to a power of two, at the cost of a little
  speed on every `cmempool` allocation and release. Because it is part of the
  interface between an application and the library, a disagreement between
  the two sides gives a link error, not a pool of the wrong size.
- `CCOL_MEMPOOL_DYNAMIC_TLS`, a compile-time switch that selects the general
  thread-local storage model instead of initial-exec for the library's
  thread-local fast paths. Use it when you must `dlopen()` the library into a
  process whose static thread-local block is already full.
- `WITH_CJSON`, `WITH_CYAML`, `WITH_CLOGGER`, `WITH_CHTTPCLIENT` and
  `WITH_CHTTPSERVER`, build switches that leave a module out of the library.
  With both HTTP modules off, the library does not need OpenSSL; with
  `clogger` also off, it does not need zlib. A reduced build is not interchangeable
  with a full build at the ABI level.
- `chttpclient` connects to a host with several addresses by racing them
  (Happy Eyeballs, RFC 8305) in every tier: the families alternate, the next
  address starts after 250 ms or at once on a failure, and the connect timeout
  bounds the whole race. It reads a final response that arrives while it still
  sends the body, so a `401` or `413` sent early reaches the caller instead of
  a transfer error. Every client socket is close-on-exec. A timeout of 0
  means no limit, and every other value, up to `UINT64_MAX` microseconds, is
  honoured.
- `chttpsvr_start()` on a running server swaps in its new pools at once. The
  pools of the previous run finish the requests they hold on a thread of their
  own, and a request that arrives during the swap is always served.
- `cjson` and `cyaml` keep their own copy of every allocator procs struct
  that a `*_mp` function receives, so the caller's struct may go out of scope.
  A process can use up to 64 distinct procs structs with these modules.
  `clrucache` keeps its copy in the cache.
- `cjson_set()` and `cyaml_set()` accept the C23 `nullptr`.
- `cyaml` refuses a document that holds invalid UTF-8 or a raw character
  that YAML 1.2 does not allow in a stream, and its error names the bytes
  and the line and column. Its API refuses a string that is not valid UTF-8.

### Security

- The build uses `-fstack-protector-strong`, `-fstack-clash-protection`,
  `-D_FORTIFY_SOURCE=3`, and RELRO with BIND_NOW. It also uses
  `-fvisibility=hidden`, so that the set of exported symbols equals the
  declared public API and nothing else. The build probes each hardening flag
  against the compiler and the target and passes only the flags that this
  pair really implements; `make hardening_report` prints the result, so a
  flag that a toolchain cannot provide is visible instead of silently
  missing.
- The `cjson` parser, the `cyaml` parser, the HTTP/1.1 parser and the URL and
  redirect layer of `chttpclient` are fuzzed continuously, as a blocking job
  on every push and every pull request. The URL target also
  asserts, on every input, that a redirect can never move a request from a
  network transport onto an AF_UNIX socket.
- The HTTP server never lets a slow client hold a worker thread of a buffered
  route. A body that stops arriving, or a response that meets a full socket,
  parks its connection with no thread, and any free worker continues it once
  the socket is ready; a parked body wakes the reactor through `SO_RCVLOWAT`
  only once a useful part of it is queued. A minimum transfer rate
  (`min_transfer_rate_bps`, 240 bytes a second after a grace of 5 s by
  default) ends a body with `408` and cuts off a response that moves slower,
  and it ends the read of a streaming handler with `ccol_timed_out`. The
  bodies of buffered requests that are still arriving share a memory limit
  (`max_partial_body_memory`, 256 MiB by default); a body that does not fit
  waits with no thread, unread and without a `100 Continue`, is admitted in
  arrival order, and gets `503` after `body_memory_wait_timeout_us`. When
  every body that holds memory waits, the oldest goes ahead, so the limit is
  exceeded by at most one `max_body_size` and the server cannot deadlock.
  Streaming handlers run on a pool of their own, created at the first
  streaming request and fed by a bounded queue with a deadline, so slow
  streaming clients never take the threads of buffered routes. Every `503`
  that the server writes itself has `Retry-After: 5`. For each of these
  settings of `chttpsvr_config_t`, 0 selects the default. Only a named
  constant turns a setting off: `CHTTPSVR_NO_RATE_FLOOR`,
  `CHTTPSVR_NO_MEMORY_CAP`, `CHTTPSVR_NO_DEADLINE` and
  `CHTTPSVR_STREAMING_POOL_OFF`.
- The HTTP server holds a request to the `Host` rule of RFC 7230 section 5.4.
  More than one `Host` header, and an HTTP/1.1 request with none, each get
  `400 Bad Request`. Two `Host` lines are what let a front-end forward one
  authority while the back-end reads another.
- The HTTP server accepts a request-target only in origin-form, which begins
  with `/`, or in absolute-form with the `http` or `https` scheme. Every other
  target gets `400 Bad Request` before any middleware runs, so
  `chttpsvr_req_path()` always begins with `/` and a middleware that guards a
  path prefix sees every request that the guarded routes serve. The one
  exception is `OPTIONS *`, which the server answers itself with `200 OK` and
  a `Content-Length` of 0, exactly as Go's `net/http` server does, with no
  middleware and no handler. Any other method with the target `*`, an
  unrecognised one included, gets `400` and a close.
- The HTTP/1.1 parser refuses an HTTP/1.0 request that carries
  `Transfer-Encoding`, which the server answers with `400 Bad Request` and a
  close, and it never keeps a connection alive after an HTTP/1.0 response that
  carries one (RFC 9112 section 6.1). A `close` token in `Connection` wins
  over `keep-alive` for every HTTP version. A chunk-size line must follow the
  full chunk-extension grammar of RFC 9112 section 7.1.1, so a bare CR, a NUL
  or any other control byte in an extension is refused.
- The type-inferred macros of `chashmap` and `cbstmap` convert each argument
  through the declared key type and value type of the container. They never
  copy the raw bytes of an expression of another type that happens to share a
  width.
- A zero-initialised `chttp_tls_config_t` is the strict configuration. It
  verifies the certificate chain of the server and matches its hostname. Both
  relaxations are named for what turning them on gives up,
  `insecure_skip_verify` and `insecure_skip_hostname_check`, so a caller
  cannot reach a weaker setting by writing less than they meant to.
  `chttpclient_set_tls()` refuses `insecure_skip_verify` beside a
  `ca_bundle_path`, because that pair states two incompatible policies.
- A redirect can never add the `http+unix` transport to a chain and can never
  re-point it at another socket. That rule is unconditional. Without it, any
  http or https server gets a request-forgery primitive against every local
  socket the calling process can reach.
- `chttp_request_t.prevent_tls_downgrade_on_redirect` refuses a redirect that would take
  a chain from `https` to `http`, and reports `ccol_http_invalid_url` without
  opening the plaintext connection. It defaults to false, so such a redirect
  is followed unless a caller asks otherwise; that matches curl and the Go
  `net/http` client. An upgrade from `http` to `https` is always followed.
- On a redirect, `chttpclient` follows the rules of curl for the headers that
  the caller set. A hop whose scheme, host or port differs from the original
  request carries none of the caller's `Authorization`, `Cookie`, `Cookie2`
  and `WWW-Authenticate` headers, and a hop whose host differs carries none
  of the caller's `Host` header; the client writes its own. Every hop is
  compared with the original request, so a hop back to the original origin
  carries them again. All three tiers apply the rule.
- `chttpclient` percent-encodes every byte of a path or a query that RFC 3986
  does not allow literally on the request line, in the caller's URL and in a
  `Location` alike, so a space in a `Location` cannot split the request
  line. An existing `%XX` escape is kept, and a `%` that two hex digits do not
  follow is `ccol_http_invalid_url`.
- The `cyaml` parser refuses a raw C0 control byte, DEL included, in an anchor
  name, an alias name and a tag token, as it does in scalar content.
  Each of those is stored as a NUL-terminated string, so a raw NUL inside one
  would end it early: two anchors that differ only after such a byte would
  collapse into one name, and an alias would then resolve to the wrong node.
- `chashmap` hashes in two modes. Every map starts in a fast mode: a
  Fibonacci multiply for integral, floating-point and pointer keys, XXH64
  with a secret seed that each process chooses at random for every key type
  without a fixed width, and the murmur3 finalizer for the result of a custom
  hash function. Every insert of a new key measures the probes or chain
  nodes that it cost, and a map whose keys collide more than a random
  function lets them switches to a keyed mode: SipHash-1-3, a keyed
  pseudorandom function, for keys without a fixed width, and a mixer seeded
  with a secret for the others and for custom hashes. `long double` keys go
  through SipHash-1-3 in both modes. Each growth of a keyed map tries the
  fast mode again on the new table and keeps it only when the fill shows no
  collision; a failed attempt stops early, allocates nothing more and costs
  work linear in the count, so an attack pays a constant amount for each
  insert even when every growth fails, and a map that returns to the fast
  mode switches again within two windows of 256 inserts if the attack
  resumes. A lookup or a delete never examines more slots than the largest
  displacement in the table. A JSON object or a YAML mapping from an untrusted peer
  therefore cannot make `cjson_parse()` or `cyaml_parse()` take quadratic
  time, and neither can integer keys that a map or a cache takes from a
  request.
- A `chttpserver` whose TLS configuration names a `ca_bundle_path` requires
  every client to present a certificate that the bundle verifies. The field
  `client_cert_optional` of `chttp_tls_config_t` turns that into a request
  that a client may decline, and `chttpsvr_req_peer_cert_verified()` tells a
  handler whether the client of a request presented a verified certificate.
  `chttpsvr_req_peer_cert_der()`, `chttpsvr_req_peer_cert_sha256()` and
  `chttpsvr_req_peer_cert_subject()` tell it which certificate that was, so
  a handler can authorise each client on its own.
- Certificate revocation lists in a `ca_bundle_path` are enforced, by the
  server against client certificates and by the client against server
  certificates, as the `ssl_crl` directive of nginx does: every certificate
  of the chain below its trust anchor needs a CRL of its issuer in the
  bundle, and a revoked certificate, a missing CRL or an expired CRL fails
  the handshake.
- A certificate, key or CA bundle path must name a regular file. A FIFO, a
  terminal or any other special file fails at once with
  `ccol_http_tls_cert_load_failed` and never blocks the caller.
- The `cjson` parser refuses a string that is not well-formed UTF-8 and a
  `\u` escape of a surrogate that is not one half of a pair, with an error
  that names the bytes and the position. Nothing is replaced with U+FFFD, so
  two keys that differ in the input can never become one key in the tree.
- The `cyaml` parser stops building the canonical text of a collection key
  at its 64 KiB limit, so a key built from many aliases costs at most that
  much before it is refused. Every `cyaml` error message is printable ASCII:
  input text that a message quotes shows every other byte as `<0xNN>`, so a
  document cannot put a terminal escape or a line break into a log.
- A rotating `clogger` never opens a name that it has just vacated. It
  creates each new live file under a private name and renames it into
  place, so a link or a FIFO planted at the live name receives nothing and
  blocks nothing. Rotation naming, retention and compression act only on
  regular files that the process or the owner of the live file owns, with a
  plausible time stamp; every other entry in the log directory is never
  opened, compressed, renamed or deleted, and is reported in a WARN record.
- The memory limit of `chttpserver` for request bodies,
  `max_partial_body_memory`, bounds every body of a buffered route from its
  first byte until its handler returns, and a body buffer never holds more
  than its reservation. Every response that the server writes on its own,
  rejections and `100 Continue` included, parks on a full socket instead of
  holding a thread, so a client that stops reading cannot tie up the reject
  pool or the reactor.
- `chttpclient` accepts a URL host, and a `Location` host, only in the
  RFC 3986 `uri-host` grammar, and userinfo only in the RFC 3986 userinfo
  set, so a host such as `good.com\@evil.com` cannot be read one way here
  and another way elsewhere. An unsolicited `101 Switching Protocols` fails
  the request. A final response that arrives with a `100 Continue` stops the
  body before any byte of it is sent, in every tier. A pooled connection
  with input that nobody asked for is never reused. An upload over TLS waits
  on the socket while the server holds a partial record, and never spins.
- `chttp_base64_decode_mp()` refuses a final group whose unused bits are not
  zero, so each decoded value has exactly one encoding.
- See `SECURITY.md` for how to report a vulnerability.
