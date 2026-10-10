# chttpserver: an embedded HTTP/1.1 server

`chttpserver` embeds an HTTP/1.1 server in your C program. You register a
handler function for a method and a path pattern, start the server, and the
library does the rest:

- it accepts connections,
- it parses requests,
- it runs your handlers on a pool of worker threads,
- and it writes the responses.

TLS, mutual TLS, middleware, sub-routers and streaming uploads are all part
of the module.

The server is a good fit when:

- your program needs a REST or JSON API, a health or metrics endpoint, a
  webhook receiver, or an admin page;
- you want the routing style of Go's `net/http` package in plain C, that is,
  patterns such as `/users/{id}` and middleware that calls `next`;
- you do not want to run a separate web server or link a large framework.

Reach for a different tool when you need any of these:

- HTTP/2 or HTTP/3,
- WebSockets (a `101 Switching Protocols` upgrade),
- static files with range requests,
- virtual hosts, selected by the `Host` header.

A full web server such as nginx provides these features, and it can also sit
in front of this one.

```c
#include <ccollections/chttpserver.h>
```

The header includes `chttp.h`, and `chttp.h` holds the types that the server
shares with the [HTTP client](chttpclient.md): `chttp_method_t`, the
`CHTTP_STATUS_*` constants and `chttp_tls_config_t`. The module depends on
OpenSSL; [Building](building.md) explains that dependency and how to build
the library without the HTTP modules.

## A first example

This server answers `GET /hello` and stops when you press Ctrl-C:

```c
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <ccollections/chttpserver.h>

static void hello(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
    (void)ctx;
    const char *name = NULL;
    if (chttpsvr_req_query_one(req, "name", &name) != ccol_success || !name)
        name = "world";
    chttpsvr_resp_printf(resp, "Hello, %s!\n", name);
}

static void on_signal(int sig) { (void)sig; chttpsvr_engine_stop(); }

int main(int argc, char **argv) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    chttpsvr srv = ccol_create_chttpsvr(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) return 1;

    chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
    cfg.host = "127.0.0.1";
    cfg.port = argc > 1 ? (uint16_t)atoi(argv[1]) : 8080;
    if (chttpsvr_register_handler(srv, CHTTP_GET, "/hello", hello, NULL) !=
            ccol_success ||
        chttpsvr_start(srv, &cfg) != ccol_success) {
        fprintf(stderr, "cannot listen on port %u\n", (unsigned)cfg.port);
        chttpsvr_destroy(srv);
        chttpsvr_engine_wait();   /* the engine may be in the middle of a stop */
        return 1;
    }
    printf("listening on http://127.0.0.1:%u/hello\n", (unsigned)cfg.port);
    fflush(stdout);

    chttpsvr_engine_wait();   /* returns after SIGINT or SIGTERM */
    chttpsvr_destroy(srv);
    return 0;
}
```

Build the example and try it:

```sh
gcc -std=gnu11 hello.c -lccollections -o hello
./hello 8080 &
curl 'http://127.0.0.1:8080/hello?name=you'    # Hello, you!
kill -INT %1
```

The whole program rests on four calls:

1. `ccol_create_chttpsvr` creates a server. With `CLOG_INVALID`, the server
   uses its own quiet logger, which prints only fatal messages; pass a
   [clogger](clogger.md) handle instead to make the server log through it.
2. `chttpsvr_register_handler` adds a route. A request for any other path
   gets a `404` without running your code.
3. `chttpsvr_start` binds the port and returns once the server is listening.
4. `chttpsvr_engine_wait` blocks until something calls
   `chttpsvr_engine_stop`, which in this example is the signal handler.
   `chttpsvr_destroy` then completes the requests in progress and frees the
   server. On the error path a failed start can have started the engine
   anyway, so the program calls `chttpsvr_engine_wait` after the destroy to
   let the engine stop completely before `main` returns.

## How the server runs your code

All the servers of a process share one background engine, which has two
parts:

- a reactor thread, which accepts connections and parses the request
  headers;
- a sweep thread, which applies the timeouts.

The first `chttpsvr_start` starts the engine, and the engine stops when the
last server is destroyed or when you call `chttpsvr_engine_stop`. You never
start it yourself.

Each server has its own pool of worker threads. When the headers of a
request arrive, the reactor finds the route and hands the request to a
worker, which reads the body and calls your handler. Three consequences
follow:

- **Handlers run concurrently.** Two requests can be inside your handlers at
  the same time on different threads, so protect shared state with a lock.
- **A handler never sees a partly read body.** On an ordinary (buffered)
  route, the whole body is in memory before your handler runs.
- **The response goes out after the handler returns.** The server buffers
  everything you write and, once the handler returns, sends it with the
  correct `Content-Length`.

The `req` and `resp` pointers, and every string you get from `req`, are
valid only until your handler returns, so copy any data you need to keep.

## Configure the listener

`chttpsvr_config_t` holds all the settings. Start from
`CHTTPSVR_CONFIG_DEFAULT` and change only what you need:

```c
chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
cfg.host = "127.0.0.1";            /* loopback only */
cfg.port = 8443;
cfg.worker_thread_count = 8;       /* default: one per CPU core */
cfg.max_body_size = 1024 * 1024;   /* a larger body gets 413 */
```

The defaults are safe for a server on the public internet: every timeout and
every resource limit is finite.
[CHTTPSVR_CONFIG_DEFAULT(3)](../man/chttpserver/CHTTPSVR_CONFIG_DEFAULT.3)
lists each field with its default. Every timeout field has a name ending in
`_us` and is measured in microseconds.

`host` decides where the server listens:

| `host` | The server listens on |
|---|---|
| `NULL` or `""` (the default) | all interfaces, IPv4 and IPv6 |
| `"0.0.0.0"` | all IPv4 interfaces |
| `"127.0.0.1"`, `"::1"`, `"[::1]"` | exactly that address |
| `"localhost"` or another name | its first IPv4 address, or its first IPv6 address if it has no IPv4 address |
| `"unix:///run/myapp.sock"` | a Unix domain socket at that path (the server ignores `port`) |

The port must not be 0, except for a Unix socket. If another program is
already using the address, `chttpsvr_start` fails rather than silently
listening somewhere else. When the server stops, it removes its Unix socket
file, and it never replaces an existing file unless that file is a stale
socket.

One process can run several servers, for example a public API and an admin
interface on different ports. Create one `chttpsvr` for each and start each
with its own configuration; they all share the engine.

## Routing

A pattern is a path made of segments, and a segment written as `{name}`
captures whatever text the request has at that position:

```c
chttpsvr_register_handler(srv, CHTTP_GET,    "/users",           list_users,  NULL);
chttpsvr_register_handler(srv, CHTTP_POST,   "/users",           create_user, NULL);
chttpsvr_register_handler(srv, CHTTP_GET,    "/users/{id}",      get_user,    NULL);
chttpsvr_register_handler(srv, CHTTP_PUT,    "/teams/{t}/{u}",   add_member,  NULL);
```

In the handler, `chttpsvr_req_param(req, "id")` returns the captured value
with its percent-encoding already decoded. The last argument of every
registration is a `void *ctx`, which the server passes to your handler
unchanged; use it to give the handler your application state.

The server answers some requests on its own:

- A path that matches no pattern gets `404 Not Found`.
- A path that matches, but with a method that no route accepts, gets
  `405 Method Not Allowed` with an `Allow` header.
- A `GET` route also answers `HEAD`: your handler runs as it would for
  `GET`, and the server sends the headers without the body.
- A method that is not one of the seven methods of `chttp_method_t` (for
  example `TRACE`) gets `501 Not Implemented`.

`CHTTP_ANY` registers one handler for every method of a pattern, and
`chttpsvr_req_method(req)` tells you which method arrived. Because the server
tries the routes of a router in registration order, register specific
methods before a `CHTTP_ANY` fallback on the same pattern.

A pattern must start with `/`, must not end with `/`, and must not contain
`//`. A request path with a trailing slash does not match a pattern without
one, so `/users/42/` does not match `/users/{id}`.

You can add routes and middleware at any time, even while the server is
running.
[chttpsvr_register_handler(3)](../man/chttpserver/chttpsvr_register_handler.3)
gives the complete matching rules.

## Read the request

```c
chttp_method_t m    = chttpsvr_req_method(req);
const char *path    = chttpsvr_req_path(req);        /* decoded, no query */
const char *ctype   = chttpsvr_req_header(req, "content-type");  /* any case */
const char *id      = chttpsvr_req_param(req, "id");

size_t len;
const void *body    = chttpsvr_req_body(req, &len);  /* buffered routes */
```

The string getters return NULL for a header, parameter or body that the
request does not have. The body is not NUL-terminated, so always use `len`.

The server decodes the query string only when you ask for it. For a key that
appears once, use `chttpsvr_req_query_one`, which fails with
`ccol_not_permitted` if the key appears more than once. For a key that can
repeat (`?tag=a&tag=b`), use `chttpsvr_req_query`:

```c
const char *sort = NULL;
if (chttpsvr_req_query_one(req, "sort", &sort) == ccol_success && sort)
    chttpsvr_resp_printf(resp, "sorted by %s\n", sort);

size_t n;
const char **tags = chttpsvr_req_query(req, "tag", &n);   /* n is 0 if the key is missing */
for (size_t i = 0; i < n; i++)
    chttpsvr_resp_printf(resp, "tag %s\n", tags[i]);
```

`chttpsvr_req_raw_query` returns the undecoded query string, for when you
want to parse it yourself.

## Write the response

The default status is `200 OK`, and you can write the body in as many pieces
as you like:

```c
chttpsvr_resp_set_status(resp, CHTTP_STATUS_CREATED);
chttpsvr_resp_set_header(resp, "location", "/users/42");
chttpsvr_resp_add_header(resp, "set-cookie", "a=1; Path=/");
chttpsvr_resp_add_header(resp, "set-cookie", "b=2; Path=/");
chttpsvr_resp_printf(resp, "created user %d\n", 42);
```

- `chttpsvr_resp_write`, `chttpsvr_resp_write_str` and `chttpsvr_resp_printf`
  append data to the body.
- `chttpsvr_resp_write_json` appends a JSON text to the body and sets
  `Content-Type: application/json`.
- `chttpsvr_resp_set_header` gives a header a single value, while
  `chttpsvr_resp_add_header` adds another line with the same name.

The server sets `Date`, `Content-Length` and `Connection` itself. It refuses
any header value that contains CR or LF, so request data that you copy into
a header cannot split the response. A `204`, a `304` and a `1xx` never carry
a body, even if you wrote one.

## Middleware and sub-routers

A middleware runs before the handler and decides whether the request goes
on: it either calls `next` to continue, or answers the request itself and
returns:

```c
static void require_api_key(chttpsvr_req *req, chttpsvr_resp *resp,
                            void *ctx, chttpsvr_next_fn next) {
    const char *key = chttpsvr_req_header(req, "x-api-key");
    if (!key || strcmp(key, ctx) != 0) {
        chttpsvr_resp_set_status(resp, CHTTP_STATUS_UNAUTHORIZED);
        return;              /* the handler does not run */
    }
    next(req, resp);
}
```

`chttpsvr_use` adds a middleware that runs for every request, including the
`404`, `405` and `413` answers that the server makes itself, so an access log
or a rate limiter written as middleware sees that traffic too.

A sub-router groups routes under a path prefix and has its own middleware:

```c
chttpsvr_router *admin = chttpsvr_subrouter(srv, "/admin");
chttpsvr_router_use(admin, require_api_key, "s3cret");
chttpsvr_router_on(admin, CHTTP_GET,  "/stats",  show_stats,  NULL);  /* /admin/stats */
chttpsvr_router_on(admin, CHTTP_POST, "/reload", reload_conf, NULL);  /* /admin/reload */
```

A sub-router owns every path under its prefix, so for a request to
`/admin/anything`:

- the request always passes through the admin middleware, even when it
  matches no admin route (in that case the sub-router answers `404`);
- no route registered on the server itself can answer it.

That makes a sub-router the right place for a guard. Things run in this
order:

1. the global middleware,
2. the middleware of the sub-router,
3. the handler.

## Example: a small JSON service

This is a complete REST service. It:

- keeps JSON documents in memory in a [cjson](cjson.md) dictionary, which a
  mutex protects because handlers run on more than one thread;
- protects the API with a bearer token in a sub-router;
- logs every request with a global middleware;
- stops itself on `POST /admin/shutdown`.

```c
/* notes: a very small JSON document store.
 *
 *   GET    /api/v1/notes        list all the notes as one JSON object
 *   GET    /api/v1/notes/{id}   read one note
 *   PUT    /api/v1/notes/{id}   store a JSON document with the key id
 *   DELETE /api/v1/notes/{id}   remove a note
 *   POST   /admin/shutdown      stop the server
 *
 * All the paths below /api/v1 need "Authorization: Bearer secret".
 */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ccollections/chttpserver.h>
#include <ccollections/cjson.h>

typedef struct {
    pthread_mutex_t lock;  /* handlers run on several worker threads */
    cjson notes;           /* a dictionary: id -> document */
} store_t;

/* ---- middleware ---------------------------------------------------- */

static void access_log(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                       chttpsvr_next_fn next) {
    (void)ctx;
    fprintf(stderr, "%s %s\n", chttp_method_str(chttpsvr_req_method(req)),
            chttpsvr_req_path(req));
    next(req, resp);   /* also runs for 404 and 405 answers */
}

static void require_token(chttpsvr_req *req, chttpsvr_resp *resp,
                          void *ctx, chttpsvr_next_fn next) {
    const char *expected = ctx;
    const char *auth = chttpsvr_req_header(req, "authorization");
    if (!auth || strcmp(auth, expected) != 0) {
        chttpsvr_resp_set_status(resp, CHTTP_STATUS_UNAUTHORIZED);
        chttpsvr_resp_set_header(resp, "www-authenticate", "Bearer");
        chttpsvr_resp_write_str(resp, "missing or wrong token\n");
        return;            /* no call to next: the chain stops here */
    }
    next(req, resp);
}

/* ---- handlers ------------------------------------------------------ */

static void send_json(chttpsvr_resp *resp, cjson node) {
    char *text = cjson_serialize(node);
    if (!text) {
        chttpsvr_resp_set_status(resp, CHTTP_STATUS_INTERNAL_ERROR);
        return;
    }
    chttpsvr_resp_write_json(resp, text, strlen(text));
    cjson_serialize_free(text);
}

static void list_notes(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
    (void)req;
    store_t *st = ctx;
    pthread_mutex_lock(&st->lock);
    send_json(resp, st->notes);
    pthread_mutex_unlock(&st->lock);
}

static void get_note(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
    store_t *st = ctx;
    const char *id = chttpsvr_req_param(req, "id");
    pthread_mutex_lock(&st->lock);
    cjson doc = cjson_dictionary_get(st->notes, id);   /* borrowed */
    if (doc)
        send_json(resp, doc);
    else
        chttpsvr_resp_set_status(resp, CHTTP_STATUS_NOT_FOUND);
    pthread_mutex_unlock(&st->lock);
}

static void put_note(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
    store_t *st = ctx;
    size_t len = 0;
    const char *body = chttpsvr_req_body(req, &len);
    char *err = NULL;
    cjson doc = body ? cjson_parse_n(body, len, &err) : NULL;
    if (!doc) {
        chttpsvr_resp_set_status(resp, CHTTP_STATUS_BAD_REQUEST);
        chttpsvr_resp_printf(resp, "invalid JSON: %s\n",
                             err ? err : "empty body");
        return;
    }
    pthread_mutex_lock(&st->lock);
    bool existed = cjson_dictionary_get(st->notes, chttpsvr_req_param(req, "id"));
    ccol_retval_t rv = cjson_dictionary_set(st->notes,
                                            chttpsvr_req_param(req, "id"), doc);
    pthread_mutex_unlock(&st->lock);
    if (rv == ccol_invalid_args) {
        cjson_destroy(doc);   /* refused: we keep ownership of the document */
        chttpsvr_resp_set_status(resp, CHTTP_STATUS_BAD_REQUEST);
    } else if (rv != ccol_success) {
        chttpsvr_resp_set_status(resp, CHTTP_STATUS_INTERNAL_ERROR);
    } else {
        chttpsvr_resp_set_status(resp, existed ? CHTTP_STATUS_NO_CONTENT
                                               : CHTTP_STATUS_CREATED);
    }
}

static void delete_note(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
    store_t *st = ctx;
    pthread_mutex_lock(&st->lock);
    ccol_retval_t rv = cjson_dictionary_remove(st->notes,
                                               chttpsvr_req_param(req, "id"));
    pthread_mutex_unlock(&st->lock);
    chttpsvr_resp_set_status(resp, rv == ccol_success ? CHTTP_STATUS_NO_CONTENT
                                                      : CHTTP_STATUS_NOT_FOUND);
}

static void shutdown_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                             void *ctx) {
    (void)req; (void)ctx;
    chttpsvr_resp_write_str(resp, "bye\n");
    chttpsvr_engine_stop();   /* returns at once; main() does the rest */
}

static void on_signal(int sig) { (void)sig; chttpsvr_engine_stop(); }

int main(int argc, char **argv) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    store_t st;
    st.notes = cjson_create_dictionary();
    if (!st.notes) return 1;
    pthread_mutex_init(&st.lock, NULL);

    chttpsvr srv = ccol_create_chttpsvr(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) {
        cjson_destroy(st.notes);
        pthread_mutex_destroy(&st.lock);
        return 1;
    }

    /* Any registration can fail, and a server without its token guard must
       not start, so check the result of every registration. */
    chttpsvr_router *api = chttpsvr_subrouter(srv, "/api/v1");
    bool ok =
        chttpsvr_use(srv, access_log, NULL) == ccol_success && api &&
        chttpsvr_router_use(api, require_token, "Bearer secret") == ccol_success &&
        chttpsvr_router_on(api, CHTTP_GET, "/notes", list_notes, &st) == ccol_success &&
        chttpsvr_router_on(api, CHTTP_GET, "/notes/{id}", get_note, &st) == ccol_success &&
        chttpsvr_router_on(api, CHTTP_PUT, "/notes/{id}", put_note, &st) == ccol_success &&
        chttpsvr_router_on(api, CHTTP_DELETE, "/notes/{id}", delete_note, &st) == ccol_success &&
        chttpsvr_register_handler(srv, CHTTP_POST, "/admin/shutdown",
                                  shutdown_handler, NULL) == ccol_success;

    chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
    cfg.host = "127.0.0.1";
    cfg.port = argc > 1 ? (uint16_t)atoi(argv[1]) : 8080;
    cfg.max_body_size = 64 * 1024;   /* a note is small; refuse a larger body with 413 */
    if (!ok || chttpsvr_start(srv, &cfg) != ccol_success) {
        chttpsvr_destroy(srv);
        chttpsvr_engine_wait();   /* the engine may be in the middle of a stop */
        cjson_destroy(st.notes);
        pthread_mutex_destroy(&st.lock);
        return 1;
    }

    chttpsvr_engine_wait();   /* until /admin/shutdown or a signal */
    chttpsvr_destroy(srv);    /* after this call, all the handlers have returned */

    cjson_destroy(st.notes);
    pthread_mutex_destroy(&st.lock);
    return 0;
}
```

Try the service:

```sh
./notes 8080 &
curl -i http://127.0.0.1:8080/api/v1/notes                       # 401
curl -H 'Authorization: Bearer secret' -X PUT \
     --data '{"title":"milk"}' http://127.0.0.1:8080/api/v1/notes/1  # 201
curl -H 'Authorization: Bearer secret' http://127.0.0.1:8080/api/v1/notes
curl -X POST http://127.0.0.1:8080/admin/shutdown
```

Note how `put_note` handles ownership. `cjson_dictionary_set` takes over the
document on success and on most failures, but when it returns
`ccol_invalid_args` the caller keeps ownership. In this example that happens
for an `id` that is not valid UTF-8.

## Streaming uploads

A buffered route holds the whole body in memory, up to `max_body_size`,
before the handler runs. For large uploads, register a **streaming** route
instead. Its handler starts as soon as the headers arrive and reads the body
in pieces with `chttpsvr_req_read`, which works like `read(2)`: it returns
the number of bytes, `0` at the end of the body, or `-1` after an error.

Streaming handlers run on their own pool of `streaming_thread_count`
threads, so slow uploads cannot take the threads that serve your other
routes.

The example below hashes uploads of any size in constant memory. It also
shows a useful property: a client can send `Expect: 100-continue` (curl does
this for large uploads) and then wait for the server's permission, which the
server gives only at the first `chttpsvr_req_read`. A handler that refuses the request before reading therefore stops the client from uploading the body.

```c
/* upload: compute the hash of a request body as it arrives, without
 * keeping the body in a buffer.
 *
 *   PUT /upload/{name}   answers with the byte count and an FNV-1a hash
 *
 * The server stops automatically after three uploads.
 */
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ccollections/chttpserver.h>

#define UPLOADS_BEFORE_EXIT 3

static atomic_int uploads_done;

static void upload(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
    (void)ctx;
    const char *ctype = chttpsvr_req_header(req, "content-type");
    if (ctype && strstr(ctype, "text/html")) {
        /* Refuse before reading, so that a client that sent
           "Expect: 100-continue" does not upload the body. */
        chttpsvr_resp_set_status(resp, CHTTP_STATUS_UNSUPPORTED_MEDIA);
        return;
    }

    unsigned char buf[16 * 1024];
    uint64_t hash = 0xcbf29ce484222325u, total = 0;
    ssize_t n;
    while ((n = chttpsvr_req_read(req, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++)
            hash = (hash ^ buf[i]) * 0x100000001b3u;
        total += (uint64_t)n;
    }

    if (n < 0) {
        ccol_retval_t why = chttpsvr_req_stream_error(req);
        if (why == ccol_msg_too_large)
            chttpsvr_resp_set_status(resp, CHTTP_STATUS_PAYLOAD_TOO_LARGE);
        else if (why == ccol_timed_out)
            chttpsvr_resp_set_status(resp, CHTTP_STATUS_REQUEST_TIMEOUT);
        else
            chttpsvr_resp_set_status(resp, CHTTP_STATUS_BAD_REQUEST);
        return;   /* the server closes the connection after this answer */
    }

    chttpsvr_resp_printf(resp, "%s: %" PRIu64 " bytes, fnv1a %016" PRIx64 "\n",
                         chttpsvr_req_param(req, "name"), total, hash);

    if (atomic_fetch_add(&uploads_done, 1) + 1 == UPLOADS_BEFORE_EXIT)
        chttpsvr_engine_stop();
}

int main(int argc, char **argv) {
    chttpsvr srv = ccol_create_chttpsvr(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) return 1;

    chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
    cfg.host = "127.0.0.1";
    cfg.port = argc > 1 ? (uint16_t)atoi(argv[1]) : 8080;
    cfg.max_body_size = 0;                /* no size limit for this route */
    cfg.streaming_thread_count = 2;       /* at most two uploads at once */
    if (chttpsvr_register_streaming_handler(srv, CHTTP_PUT, "/upload/{name}",
                                            upload, NULL) != ccol_success ||
        chttpsvr_start(srv, &cfg) != ccol_success) {
        chttpsvr_destroy(srv);
        chttpsvr_engine_wait();   /* the engine may be in the middle of a stop */
        return 1;
    }
    chttpsvr_engine_wait();
    chttpsvr_destroy(srv);
    return 0;
}
```

```sh
./upload 8080 &
curl -T big.iso http://127.0.0.1:8080/upload/big.iso
```

When `chttpsvr_req_read` returns `-1`, `chttpsvr_req_stream_error` tells you
why:

- `ccol_msg_too_large`: the body is larger than `max_body_size`.
- `ccol_timed_out`: the client stopped sending, or was too slow.
- `ccol_http_transfer_aborted`: the connection broke, or the body was
  malformed.

A sub-router registers streaming routes with `chttpsvr_router_on_stream`.

## Slow and hostile clients

A server on the internet meets clients that:

- send one byte per second,
- open connections and never use them,
- upload more data than you can hold.

The defaults protect against all of these, and you usually do not need to
change them. In summary:

- **On a buffered route, no thread waits for a slow client.** When a body
  stalls or a client reads a response slowly, the server parks the
  connection, without a thread, until the socket is ready again. The thread
  that completes a request may therefore not be the one that started it, so
  do not keep request state in thread-local storage.
- **Each phase has a deadline.** The phases are:
  - the headers (`max_header_read_duration_us`),
  - the body (`stream_read_timeout_us`, `max_body_read_duration_us`),
  - the response (`response_write_timeout_us`,
    `max_response_write_duration_us`),
  - the idle time between keep-alive requests (`idle_timeout_us`).
- **There is a minimum transfer rate** (`min_transfer_rate_bps`) of 240
  bytes per second, after a grace period of 5 seconds, so the server
  disconnects a client that stays within every deadline but moves almost no
  data.
- **Body memory is limited** (`max_partial_body_memory`) to 256 MiB for all
  buffered bodies together. A request that does not fit waits in a queue
  instead of failing, and gets `503` only if it waits too long.
- **The queues are bounded.** When the worker queue or the streaming queue
  is full, the client gets `503 Service Unavailable` with `Retry-After: 5`
  at once.

The server refuses malformed or ambiguous requests before your code runs,
for example a bad `Host`, framing headers that disagree, or an unsupported
HTTP version. If it refuses a request while the body is arriving, it
keeps reading and discarding the body for at most 2 seconds before closing
the connection, so that the client can read the answer instead of getting a
connection reset.

[chttpsvr_start(3)](../man/chttpserver/chttpsvr_start.3) describes every
limit, every answer that the server makes itself, and the request validation
rules.
[CHTTPSVR_CONFIG_DEFAULT(3)](../man/chttpserver/CHTTPSVR_CONFIG_DEFAULT.3)
explains how to turn each limit off. For most fields the value 0 turns the
limit off, but for the defenses from `min_transfer_rate_bps` to
`streaming_queue_timeout_us`, 0 means the default value, so a configuration
that leaves these fields unset keeps the defenses. To turn one of them off,
use a named constant such as `CHTTPSVR_NO_RATE_FLOOR`.

## TLS and client certificates

Point `cfg.tls` at a `chttp_tls_config_t` with a certificate and a key, and
the server speaks HTTPS:

```c
chttp_tls_config_t tls = {
    .cert_path = "/etc/myapp/server.crt",
    .key_path  = "/etc/myapp/server.key",
};
cfg.tls = &tls;
```

`chttpsvr_start` fails if:

- a file is missing,
- the server cannot read a file,
- or the certificate does not match the key.

The server never falls back to plain HTTP.

To require **client certificates** (mutual TLS), set `ca_bundle_path` as
well; a client without a certificate that verifies against that bundle then
cannot complete the handshake. With `client_cert_optional = true`, the
server also accepts clients without a certificate and leaves the decision to
each handler, which can find out who the client is with these functions:

- `chttpsvr_req_peer_cert_verified`,
- `chttpsvr_req_peer_cert_subject` (for example `CN=alice,O=Example`),
- `chttpsvr_req_peer_cert_sha256` (a fingerprint to compare with an allow
  list),
- `chttpsvr_req_peer_cert_der`.

This server shows a public page to every client and a private page only to
clients with a certificate:

```c
/* mtls: an HTTPS service that knows the identity of its clients.
 *
 *   ./mtls PORT server.crt server.key clients-ca.pem
 *
 *   GET /public    any client can call it
 *   GET /private   only a client with a certificate from clients-ca.pem
 *
 * The server stops automatically after four requests.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <ccollections/chttpserver.h>

static atomic_int requests;

static void count_request(void) {
    if (atomic_fetch_add(&requests, 1) + 1 == 4)
        chttpsvr_engine_stop();
}

static void public_page(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
    (void)ctx;
    /* NULL for a client without a verified certificate. */
    const char *who = chttpsvr_req_peer_cert_subject(req);
    chttpsvr_resp_printf(resp, "hello, %s\n", who ? who : "anonymous");
    count_request();
}

static void private_page(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
    (void)ctx;
    unsigned char fp[CHTTPSVR_PEER_CERT_SHA256_LEN];
    const char *who = chttpsvr_req_peer_cert_subject(req);
    if (!chttpsvr_req_peer_cert_verified(req)) {
        chttpsvr_resp_set_status(resp, CHTTP_STATUS_FORBIDDEN);
        chttpsvr_resp_write_str(resp, "a client certificate is required\n");
    } else if (!who || chttpsvr_req_peer_cert_sha256(req, fp) != ccol_success) {
        /* The certificate is verified, so only an allocation can fail here. */
        chttpsvr_resp_set_status(resp, CHTTP_STATUS_INTERNAL_ERROR);
    } else {
        /* A real service would find the fingerprint in an allow list. */
        chttpsvr_resp_printf(resp, "welcome, %s\nfingerprint ", who);
        for (size_t i = 0; i < sizeof(fp); i++)
            chttpsvr_resp_printf(resp, "%02x", fp[i]);
        chttpsvr_resp_write_str(resp, "\n");
    }
    count_request();
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s PORT CERT KEY CLIENT_CA\n", argv[0]);
        return 2;
    }
    chttp_tls_config_t tls = {
        .cert_path = argv[2],
        .key_path = argv[3],
        .ca_bundle_path = argv[4],     /* turns on client certificates */
        .client_cert_optional = true,  /* ...but also serves anonymous clients */
    };

    chttpsvr srv = ccol_create_chttpsvr(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) return 1;

    chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
    cfg.host = "127.0.0.1";
    cfg.port = (uint16_t)atoi(argv[1]);
    cfg.tls = &tls;
    if (chttpsvr_register_handler(srv, CHTTP_GET, "/public", public_page,
                                  NULL) != ccol_success ||
        chttpsvr_register_handler(srv, CHTTP_GET, "/private", private_page,
                                  NULL) != ccol_success ||
        chttpsvr_start(srv, &cfg) != ccol_success) {
        fprintf(stderr, "start failed (port in use, or a TLS file "
                        "that does not load)\n");
        chttpsvr_destroy(srv);
        chttpsvr_engine_wait();   /* the engine may be in the middle of a stop */
        return 1;
    }
    chttpsvr_engine_wait();
    chttpsvr_destroy(srv);
    return 0;
}
```

```sh
./mtls 8443 server.crt server.key clients-ca.pem &
curl --cacert ca.crt https://127.0.0.1:8443/public            # hello, anonymous
curl --cacert ca.crt --cert alice.crt --key alice.key \
     https://127.0.0.1:8443/private                           # welcome, CN=alice,O=Example
```

The bundle can also contain certificate revocation lists, which the server
then applies. The [HTTP client guide](chttpclient.md) and
[CHTTP_TLS_DEFAULT(3)](../man/chttp/CHTTP_TLS_DEFAULT.3) explain the fields
of `chttp_tls_config_t`.

## Start, stop and shut down

These calls stop a server:

- `chttpsvr_stop(srv)` closes only the listening socket; the server keeps
  serving open connections until they close or time out. To start it again,
  call `chttpsvr_start` again, optionally with a new configuration. The
  server listens again immediately while the requests of the previous run
  complete in the background.
- `chttpsvr_destroy(srv)` closes the listener and every connection, waits
  for the handlers that are running, then frees the server and sets the
  handle to `CHTTPSVR_INVALID`.
- `chttpsvr_engine_stop()` stops every server in the process. It does not
  block and is async-signal-safe, so it belongs in a `SIGTERM` handler, and
  it is also safe to call from a handler, as the shutdown endpoint above
  does.
- `chttpsvr_engine_wait()` blocks until the engine has stopped. Call it
  after you destroy your last server and before `main` returns, so that the
  engine's threads are gone by then.

A typical `main` therefore:

1. starts the server,
2. calls `chttpsvr_engine_wait()`, which returns when a signal or an endpoint
   stops the engine,
3. calls `chttpsvr_destroy()`.

Three process-wide settings tune the engine:

- `chttpsvr_set_engine_logger(log)` sends the engine's diagnostics, such as
  TLS handshake failures, bind failures and connections closed after the
  idle timeout, to a [clogger](clogger.md) handle. Without it, the engine
  does not print these diagnostics.
- `chttpsvr_set_engine_num_reactor_threads(n)` gives the reactor more
  threads. One thread is right for most traffic; more help only when many
  new TLS connections arrive continuously. Call it before the first start.
- `chttpsvr_set_engine_mem_mgmt_procs(mp)` makes the engine take its memory
  from a custom allocator, while each server sets its own allocator with
  `ccol_create_chttpsvr_mp`. See [Memory management](memory.md).

## Good to know

- **Do not destroy a server from its own handler.** Calling
  `chttpsvr_destroy` or `chttpsvr_engine_wait` from a handler or middleware
  of that server stops the program, because these calls would wait for the
  very request that makes them. Call `chttpsvr_engine_stop()` and return, or
  let another thread do the teardown. For the same reason, the server
  refuses a restart (`chttpsvr_stop` followed by `chttpsvr_start`) from a
  handler.
- **Protect data that handlers share with a lock.** The handlers of one
  server run on many threads at once, and streaming handlers run on other
  threads as well.
- **Do not trust paths as file names.** The server decodes
  `chttpsvr_req_path` and `chttpsvr_req_param`, so they can contain `..` and
  a `/` that arrived as `%2F`. Check them before you use them in the file
  system.
- **Be careful with a sub-router on `"/"`.** It owns only the exact path
  `/`, so its middleware does not protect the rest of the site. For a guard
  that must cover every path, use `chttpsvr_use`.
- **The server does not report duplicate routes.** If you register the same
  method and pattern twice, both calls succeed, but only the first handler
  ever runs.
- **Trailers are not headers.** The server checks the trailer fields of a
  chunked request body and then discards them, so `chttpsvr_req_header`
  never returns them.
- **SIGPIPE is safe.** The server neither raises it nor changes its
  disposition, so a client that disconnects during a response cannot kill
  your process.
- **fork().** Call `fork()` before you create a server, or call `exec`
  immediately after the fork. A child that keeps running with a server
  inherited from its parent is not supported; see
  [Concurrency](concurrency.md).
- **The handle is a value.** `chttpsvr` is an integer handle, not a pointer,
  so compare it with `CHTTPSVR_INVALID`. `chttpsvr_destroy` sets your
  variable to `CHTTPSVR_INVALID`, and a second destroy of that variable does
  nothing. A destroy through a different copy of the same handle stops the
  program rather than corrupting memory.

## Reference

Create and destroy a server:
[ccol_create_chttpsvr(3)](../man/chttpserver/ccol_create_chttpsvr.3),
[ccol_create_chttpsvr_mp(3)](../man/chttpserver/ccol_create_chttpsvr_mp.3),
[chttpsvr_destroy(3)](../man/chttpserver/chttpsvr_destroy.3),
[chttpsvr_declare(3)](../man/chttpserver/chttpsvr_declare.3),
[chttpsvr_declare_scoped(3)](../man/chttpserver/chttpsvr_declare_scoped.3),
[chttpsvr_construct(3)](../man/chttpserver/chttpsvr_construct.3),
[chttpsvr_construct_scoped(3)](../man/chttpserver/chttpsvr_construct_scoped.3)

Start and stop:
[CHTTPSVR_CONFIG_DEFAULT(3)](../man/chttpserver/CHTTPSVR_CONFIG_DEFAULT.3),
[chttpsvr_start(3)](../man/chttpserver/chttpsvr_start.3),
[chttpsvr_stop(3)](../man/chttpserver/chttpsvr_stop.3)

The shared engine:
[chttpsvr_engine_stop(3)](../man/chttpserver/chttpsvr_engine_stop.3),
[chttpsvr_engine_wait(3)](../man/chttpserver/chttpsvr_engine_wait.3),
[chttpsvr_set_engine_logger(3)](../man/chttpserver/chttpsvr_set_engine_logger.3),
[chttpsvr_set_engine_num_reactor_threads(3)](../man/chttpserver/chttpsvr_set_engine_num_reactor_threads.3),
[chttpsvr_set_engine_mem_mgmt_procs(3)](../man/chttpserver/chttpsvr_set_engine_mem_mgmt_procs.3)

Routes, middleware and sub-routers:
[chttpsvr_register_handler(3)](../man/chttpserver/chttpsvr_register_handler.3),
[chttpsvr_register_streaming_handler(3)](../man/chttpserver/chttpsvr_register_streaming_handler.3),
[chttpsvr_use(3)](../man/chttpserver/chttpsvr_use.3),
[chttpsvr_subrouter(3)](../man/chttpserver/chttpsvr_subrouter.3),
[chttpsvr_router_on(3)](../man/chttpserver/chttpsvr_router_on.3),
[chttpsvr_router_on_stream(3)](../man/chttpserver/chttpsvr_router_on_stream.3),
[chttpsvr_router_use(3)](../man/chttpserver/chttpsvr_router_use.3)

The request:
[chttpsvr_req_method(3)](../man/chttpserver/chttpsvr_req_method.3),
[chttpsvr_req_path(3)](../man/chttpserver/chttpsvr_req_path.3),
[chttpsvr_req_param(3)](../man/chttpserver/chttpsvr_req_param.3),
[chttpsvr_req_header(3)](../man/chttpserver/chttpsvr_req_header.3),
[chttpsvr_req_raw_query(3)](../man/chttpserver/chttpsvr_req_raw_query.3),
[chttpsvr_req_query(3)](../man/chttpserver/chttpsvr_req_query.3),
[chttpsvr_req_query_one(3)](../man/chttpserver/chttpsvr_req_query_one.3),
[chttpsvr_req_query_oom(3)](../man/chttpserver/chttpsvr_req_query_oom.3),
[chttpsvr_req_body(3)](../man/chttpserver/chttpsvr_req_body.3),
[chttpsvr_req_read(3)](../man/chttpserver/chttpsvr_req_read.3),
[chttpsvr_req_stream_error(3)](../man/chttpserver/chttpsvr_req_stream_error.3)

Client certificates:
[chttpsvr_req_peer_cert_verified(3)](../man/chttpserver/chttpsvr_req_peer_cert_verified.3),
[chttpsvr_req_peer_cert_subject(3)](../man/chttpserver/chttpsvr_req_peer_cert_subject.3),
[chttpsvr_req_peer_cert_sha256(3)](../man/chttpserver/chttpsvr_req_peer_cert_sha256.3),
[chttpsvr_req_peer_cert_der(3)](../man/chttpserver/chttpsvr_req_peer_cert_der.3)

The response:
[chttpsvr_resp_set_status(3)](../man/chttpserver/chttpsvr_resp_set_status.3),
[chttpsvr_resp_set_header(3)](../man/chttpserver/chttpsvr_resp_set_header.3),
[chttpsvr_resp_add_header(3)](../man/chttpserver/chttpsvr_resp_add_header.3),
[chttpsvr_resp_write(3)](../man/chttpserver/chttpsvr_resp_write.3),
[chttpsvr_resp_write_str(3)](../man/chttpserver/chttpsvr_resp_write_str.3),
[chttpsvr_resp_printf(3)](../man/chttpserver/chttpsvr_resp_printf.3),
[chttpsvr_resp_write_json(3)](../man/chttpserver/chttpsvr_resp_write_json.3)

Shared HTTP types:
[CHTTP_STATUS(3)](../man/chttp/CHTTP_STATUS.3),
[chttp_method_str(3)](../man/chttp/chttp_method_str.3),
[CHTTP_TLS_DEFAULT(3)](../man/chttp/CHTTP_TLS_DEFAULT.3)

Related guides:
[HTTP client](chttpclient.md),
[JSON](cjson.md),
[Logging](clogger.md),
[Concurrency](concurrency.md),
[Memory management](memory.md),
[Design of the library](design.md),
[Building](building.md)
