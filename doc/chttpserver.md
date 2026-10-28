# chttpserver: an embedded HTTP/1.1 server

`chttpserver` puts an HTTP/1.1 server inside your C program. You register a
handler function for a method and a path pattern and start the server, and
the library takes care of the rest: it accepts connections, parses the
requests, runs your handlers on a pool of worker threads and writes the
responses back. TLS, mutual TLS, middleware, sub-routers and streaming
uploads are all part of the module.

It fits well when your program needs a REST or JSON API, a health or metrics
endpoint, a webhook receiver or an admin page, and you'd like the routing
style of Go's `net/http` (patterns such as `/users/{id}` and middleware that
calls `next`) in plain C, without running a separate web server or linking a
large framework.

It doesn't do HTTP/2 or HTTP/3, WebSockets (a `101 Switching Protocols`
upgrade), static files with range requests, or virtual hosts selected by the
`Host` header. A full web server such as nginx has all of those, and it can
also sit in front of this one.

```c
#include <ccollections/chttpserver.h>
```

The header includes `chttp.h`, which holds the types that the server shares
with the [HTTP client](chttpclient.md): `chttp_method_t`, the
`CHTTP_STATUS_*` constants and `chttp_tls_config_t`. The module depends on
OpenSSL, and [Building](building.md) explains that dependency and how to
build the library without the HTTP modules.

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

    chttpsvr srv = chttpsvr_create(CLOG_NULL, NULL);
    if (srv == CHTTPSVR_NULL) return 1;

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

Build it and try it:

```sh
gcc -std=gnu11 hello.c -lccollections -o hello
./hello 8080 &
curl 'http://127.0.0.1:8080/hello?name=you'    # Hello, you!
kill -INT %1
```

The whole program comes down to four calls.

1. `chttpsvr_create` creates a server. With `CLOG_NULL` the server uses
   its own quiet logger, which only prints fatal messages; pass a
   [clogger](clogger.md) handle instead if you want the server to log
   through it. A logger handle that is already closed makes the call fail
   with an error string.
2. `chttpsvr_register_handler` adds a route. A request for any other path
   gets a `404` and your code never runs.
3. `chttpsvr_start` binds the port and returns once the server is listening.
4. `chttpsvr_engine_wait` blocks until something calls
   `chttpsvr_engine_stop`, the signal handler in this example. Then
   `chttpsvr_destroy` completes the requests in progress and frees the
   server. A start that failed can have started the engine anyway, so on the
   error path the program calls `chttpsvr_engine_wait` after the destroy to
   let the engine stop completely before `main` returns.

## How the server runs your code

All the servers of a process share one background engine. It has a reactor
thread, which accepts connections and parses the request headers, and a
sweep thread, which applies the timeouts. The first `chttpsvr_start` starts
the engine, and the engine stops when the last server is destroyed or when
you call `chttpsvr_engine_stop`. You never start it yourself.

A shutdown doesn't wait for clients that keep sending. The moment
`chttpsvr_destroy` or `chttpsvr_engine_stop` starts, the server takes no new
request: idle keep-alive connections get closed, a request that arrives after
that gets `503` with `Connection: close` instead of a handler, and the
response of a request that is already running carries `Connection: close`.
So the shutdown is over once the running handlers return, and after 30
seconds the server closes the connections of handlers that are busy,
so that a handler blocked on its client returns too. See
[chttpsvr_destroy(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_destroy.3.html).

Every server has its own pool of worker threads. When the headers of a
request arrive, the reactor finds the route and hands the request to a
worker, which reads the body and calls your handler.

That means handlers run concurrently. Two requests can be inside your
handlers at the same time on different threads, so shared state needs a
lock. On an ordinary (buffered) route a handler never sees a partly read
body, because the whole body is in memory before the handler runs. And the
response goes out after the handler returns: the server buffers everything
you write and sends it with the right `Content-Length` once you're done.

The `req` and `resp` pointers, and every string you get from `req`, are only
valid until your handler returns, so copy whatever you need to keep.

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

The defaults are safe for a server on the public internet, since every
timeout and every resource limit is finite.
[CHTTPSVR_CONFIG_DEFAULT(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/CHTTPSVR_CONFIG_DEFAULT.3.html)
lists every field with its default. All the timeout fields end in `_us` and
are in microseconds.

`host` decides where the server listens:

| `host` | The server listens on |
|---|---|
| `NULL` or `""` (the default) | all interfaces, IPv4 and IPv6 (IPv4 only on OpenBSD, which has no dual-stack sockets) |
| `"0.0.0.0"` | all IPv4 interfaces |
| `"127.0.0.1"`, `"::1"`, `"[::1]"` | exactly that address |
| `"localhost"` or another name | its first IPv4 address, or its first IPv6 address if it has no IPv4 address |
| `"unix:///run/myapp.sock"` | a Unix domain socket at that path (the server ignores `port`) |

A port of 0 asks the system for a free port, which is handy in tests and for
a service that announces its address somewhere else. Once `chttpsvr_start`
has returned you can ask the server which port it got:

```c
chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
cfg.host = "127.0.0.1";
cfg.port = 0;
if (chttpsvr_start(srv, &cfg) == ccol_success) {
    uint16_t port = 0;
    chttpsvr_bound_port(srv, &port);
    printf("listening on http://127.0.0.1:%u/\n", (unsigned)port);
}
```

The system picks a new port at every start, so read it again after a
restart. `chttpsvr_bound_address` writes the address the listener is bound
to (or the `unix://` path), and `chttpsvr_is_running` tells you whether the
server is listening at all. If another program already uses the address,
`chttpsvr_start` fails instead of quietly listening somewhere else. When the
server stops it removes its Unix socket file, and it never replaces a file
that exists unless that file is a stale socket.

One process can run several servers, for example a public API and an admin
interface on different ports. Create one `chttpsvr` for each and start each
one with its own configuration, and they all share the engine.

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
registration is a `void *ctx`, which the server passes to your handler as it
is, so that's how you give the handler your application state.

The server answers some requests on its own. A path that matches no pattern
gets `404 Not Found`. A path that matches but with a method that no route
accepts gets `405 Method Not Allowed` with an `Allow` header. A `GET` route
also answers `HEAD`: your handler runs as it would for `GET` and the server
sends the headers without the body. And a method that isn't one of the
seven methods of `chttp_method_t` (`TRACE`, for example) gets
`501 Not Implemented`.

`CHTTP_ANY` registers one handler for every method of a pattern, and
`chttpsvr_req_method(req)` tells you which method came in. The server tries
the routes of a router in the order you registered them, so register the
specific methods before a `CHTTP_ANY` fallback on the same pattern. A
registration with any other value of `chttp_method_t` fails with
`ccol_invalid_args`.

A pattern has to start with `/`, must not end with `/` and must not contain
`//`. A request path with a trailing slash doesn't match a pattern without
one, so `/users/42/` doesn't match `/users/{id}`.

You can add routes and middleware at any time, also while the server is
running. The complete matching rules are in
[chttpsvr_register_handler(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_register_handler.3.html).

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
request doesn't have. The body isn't NUL-terminated, so always go by `len`.

The query string is only decoded when you ask for it. For a key that
appears once, use `chttpsvr_req_query_one`, which fails with
`ccol_key_already_present` if the key appears more than once. For a key that
can repeat (`?tag=a&tag=b`), use `chttpsvr_req_query`:

```c
const char *sort = NULL;
if (chttpsvr_req_query_one(req, "sort", &sort) == ccol_success && sort)
    chttpsvr_resp_printf(resp, "sorted by %s\n", sort);

size_t n;
const char **tags = chttpsvr_req_query(req, "tag", &n);   /* n is 0 if the key is missing */
for (size_t i = 0; i < n; i++)
    chttpsvr_resp_printf(resp, "tag %s\n", tags[i]);
```

A pair whose key or value has a malformed percent-escape (`a=%zz`), or an
escape that decodes to a NUL byte (`a=%00`), is left out of the decoded
query as if the client never sent it. `chttpsvr_req_raw_query` returns the
query string as it arrived, for when you want to parse it yourself or need
to see such a pair.

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

`chttpsvr_resp_write`, `chttpsvr_resp_write_str` and `chttpsvr_resp_printf`
append data to the body, and `chttpsvr_resp_write_json` appends a JSON text
and sets `Content-Type: application/json`. `chttpsvr_resp_set_header` gives
a header a single value, while `chttpsvr_resp_add_header` adds another line
with the same name.

The server sets `Date`, `Content-Length` and `Connection` itself. It adds no
`Content-Type` though: a response has one only when your handler sets it,
with `chttpsvr_resp_set_header` or `chttpsvr_resp_write_json`. It doesn't
guess a type from the body the way Go's net/http does, and there is no
default type like nginx's `default_type`. Set one on every response that
has a body, because a client that has to guess the type can guess wrong.

A header value that contains a control byte (any byte below 0x20 other than
the tab, and 0x7F) is refused, as RFC 9110 requires, so request data you
copy into a header can't split the response and a client never sees a value
it would have to reject. A `204`, a `205` and a `304` never carry a body,
even if you wrote one, and a `205` carries `Content-Length: 0`. A `1xx` is
an interim status, so a handler that sets one as its final status gets
`500`.

## Middleware and sub-routers

A middleware runs before the handler and decides whether the request goes
on. It either calls `next` to continue, or answers the request itself and
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
`404`, `405` and `413` answers that the server makes itself, so an access
log or a rate limiter written as middleware sees that traffic as well.

A sub-router groups routes under a path prefix and has its own middleware:

```c
chttpsvr_router *admin = chttpsvr_subrouter(srv, "/admin");
chttpsvr_router_use(admin, require_api_key, "s3cret");
chttpsvr_router_on(admin, CHTTP_GET,  "/stats",  show_stats,  NULL);  /* /admin/stats */
chttpsvr_router_on(admin, CHTTP_POST, "/reload", reload_conf, NULL);  /* /admin/reload */
```

A sub-router lives as long as its server. A pointer to it is safe to pass
even after the server is destroyed (the call then fails with
`ccol_invalid_args`), and the price for that is about 100 bytes per
sub-router until the process exits. That's why sub-routers belong in the
setup code that runs once, and not in code that creates them over and over.

A sub-router owns every path under its prefix. A request to
`/admin/anything` always passes through the admin middleware, also when it
matches no admin route (the sub-router answers `404` in that case), and no
route registered on the server itself can answer it. That makes a sub-router
the right place for a guard. The order is:

1. the global middleware,
2. the middleware of the sub-router,
3. the handler.

A middleware often finds out something the handler needs, such as the user
behind an API key. It attaches it to the request with
`chttpsvr_req_set_context`, under a key that is the address of a static
object, and every later middleware and the handler read it back with
`chttpsvr_req_context`. Keys are compared by address, like the keys of Go's
`context.WithValue`, so two middleware written by different people never
overwrite each other's values:

```c
static const char user_key;   /* only its address matters */

static void authenticate(chttpsvr_req *req, chttpsvr_resp *resp,
                         void *ctx, chttpsvr_next_fn next) {
    const user_t *user = find_user(ctx, chttpsvr_req_header(req, "x-api-key"));
    if (!user) {
        chttpsvr_resp_set_status(resp, CHTTP_STATUS_UNAUTHORIZED);
        return;
    }
    if (chttpsvr_req_set_context(req, &user_key, (void *)user) !=
        ccol_success) {
        chttpsvr_resp_set_status(resp, CHTTP_STATUS_INTERNAL_ERROR);
        return;
    }
    next(req, resp);
}

static void show_stats(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
    (void)ctx;
    const user_t *user = chttpsvr_req_context(req, &user_key);
    chttpsvr_resp_printf(resp, "stats for %s", user->name);
}
```

The values belong to one request and stay readable until the whole chain has
returned, so a middleware can also read, after `next` returns, a value that
the handler stored. The library never frees a value, so a middleware that
allocated one frees it after `next` returns.

## Example: a small JSON service

This is a complete REST service. It keeps JSON documents in memory in a
[cjson](cjson.md) dictionary, behind a mutex because handlers run on more
than one thread. A sub-router protects the API with a bearer token, a global
middleware logs every request, and `POST /admin/shutdown` stops the whole
thing.

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

    chttpsvr srv = chttpsvr_create(CLOG_NULL, NULL);
    if (srv == CHTTPSVR_NULL) {
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

Try it:

```sh
./notes 8080 &
curl -i http://127.0.0.1:8080/api/v1/notes                       # 401
curl -H 'Authorization: Bearer secret' -X PUT \
     --data '{"title":"milk"}' http://127.0.0.1:8080/api/v1/notes/1  # 201
curl -H 'Authorization: Bearer secret' http://127.0.0.1:8080/api/v1/notes
curl -X POST http://127.0.0.1:8080/admin/shutdown
```

Look at how `put_note` deals with ownership. `cjson_dictionary_set` takes
over the document on success and on most failures, but when it returns
`ccol_invalid_args` the caller keeps it. In this example that happens for an
`id` that isn't valid UTF-8.

## Streaming uploads

A buffered route holds the whole body in memory, up to `max_body_size`,
before the handler runs. For large uploads you register a **streaming**
route instead. Its handler starts as soon as the headers arrive and reads the
body in pieces with `chttpsvr_req_read`, which works like `read(2)`: it
returns the number of bytes, `0` at the end of the body, or `-1` after an
error.

Streaming handlers run on their own pool of `streaming_thread_count`
threads, so slow uploads can't take away the threads that serve your other
routes.

The example below hashes uploads of any size in constant memory. It also
shows something useful about `Expect: 100-continue`. A client can send it
(curl does for large uploads) and then wait for the server's permission, and
the server only gives that permission at the first `chttpsvr_req_read`. So a
handler that refuses the request before reading keeps the client from
uploading the body at all.

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
    chttpsvr srv = chttpsvr_create(CLOG_NULL, NULL);
    if (srv == CHTTPSVR_NULL) return 1;

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
why: `ccol_msg_too_large` when the body is larger than `max_body_size`,
`ccol_timed_out` when the client stopped sending or was too slow, and
`ccol_http_transfer_aborted` when the connection broke or the body was
malformed.

A sub-router registers streaming routes with `chttpsvr_router_on_stream`.

## Slow and hostile clients

A server on the internet will meet clients that send one byte per second,
open connections and never use them, or upload more data than you can hold.
The defaults protect against all of these and you usually don't need to
touch them, but here is what they do.

On a buffered route no thread waits for a slow client. When a body stalls or
a client reads a response slowly, the server parks the connection, without a
thread, until the socket is ready again. The thread that finishes a request
may therefore not be the one that started it, so don't keep request state
in thread-local storage.

Each phase has a deadline:

- the headers (`max_header_read_duration_us`),
- the body (`stream_read_timeout_us`, `max_body_read_duration_us`),
- the response (`response_write_timeout_us`,
  `max_response_write_duration_us`),
- the idle time between keep-alive requests (`idle_timeout_us`).

There is also a minimum transfer rate (`min_transfer_rate_bps`) of 240 bytes
per second after a grace period of 5 seconds, so a client that stays within
every deadline but moves almost no data gets disconnected.

Body memory is limited (`max_partial_body_memory`) to 256 MiB for all
buffered bodies together. A body reserves memory as its bytes arrive, so a
client that declares a large body and then sends it slowly only holds what
it has sent. A request that doesn't fit waits in a queue instead of failing,
and gets `503` only if it waits too long.

The queues are bounded. When the worker queue or the streaming queue is
full, the client gets `503 Service Unavailable` with `Retry-After: 5` at
once.

Malformed or ambiguous requests are refused before your code runs, a bad
`Host` for example, framing headers that disagree, or an HTTP version that
isn't supported. If the server refuses a request while its body is arriving,
it keeps reading and throwing away the body for at most 2 seconds before
closing the connection, so the client gets to read the answer instead of a
connection reset.

[chttpsvr_start(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_start.3.html) describes every
limit, every answer the server makes on its own and the request validation
rules, and
[CHTTPSVR_CONFIG_DEFAULT(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/CHTTPSVR_CONFIG_DEFAULT.3.html)
explains how to turn each limit off. For most fields 0 turns the limit off,
but for the defenses from `min_transfer_rate_bps` to
`streaming_queue_timeout_us` 0 means the default value, so a configuration
that leaves those fields unset keeps the defenses. We chose that so a
zeroed field can never leave a public server without its protection by
accident. To turn one of them off, use a named constant such as
`CHTTPSVR_NO_RATE_FLOOR`.

## TLS and client certificates

Point `cfg.tls` at a `chttp_tls_config_t` with a certificate and a key and
the server speaks HTTPS:

```c
chttp_tls_config_t tls = {
    .cert_path = "/etc/myapp/server.crt",
    .key_path  = "/etc/myapp/server.key",
};
cfg.tls = &tls;
```

`chttpsvr_start` fails if a file is missing, if the server can't read a
file, or if the certificate doesn't match the key. It never falls back to
plain HTTP, on purpose: a server that quietly drops TLS when its certificate
is broken would send its clients' data in the clear.

To require **client certificates** (mutual TLS), set `ca_bundle_path` too.
A client without a certificate that verifies against that bundle then can't
complete the handshake. With `client_cert_optional = true` the server also
accepts clients without a certificate and leaves the decision to each
handler, which can find out who the client is with
`chttpsvr_req_peer_cert_verified`, `chttpsvr_req_peer_cert_subject` (for
example `CN=alice,O=Example`), `chttpsvr_req_peer_cert_sha256` (a
fingerprint to compare with an allow list) and `chttpsvr_req_peer_cert_der`.

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

    chttpsvr srv = chttpsvr_create(CLOG_NULL, NULL);
    if (srv == CHTTPSVR_NULL) return 1;

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

The bundle may also contain certificate revocation lists, and the server
then applies them. The fields of `chttp_tls_config_t` are explained in the
[HTTP client guide](chttpclient.md) and in
[CHTTP_TLS_DEFAULT(3)](https://abunchofnerds-25.github.io/ccollections/man/chttp/CHTTP_TLS_DEFAULT.3.html).

## Start, stop and shut down

`chttpsvr_stop(srv)` closes only the listening socket, and the server keeps
serving the open connections until they close or time out. To start it
again you call `chttpsvr_start` again, with a new configuration if you like.
The server listens again immediately while the requests of the previous run
complete in the background.

`chttpsvr_destroy(srv)` closes the listener and every connection, waits for
the handlers that are running, then frees the server and sets the handle to
`CHTTPSVR_NULL`.

`chttpsvr_engine_stop()` stops every server in the process. It doesn't
block and it's async-signal-safe, so it belongs in a `SIGTERM` handler, and
it's also safe to call from a handler, as the shutdown endpoint above does.
It only stops the engine that is running at the moment of the call: a call
while no engine runs does nothing, and it never stops an engine that a later
`chttpsvr_start` brings up. If it lands while `chttpsvr_destroy` is draining
a server, `chttpsvr_engine_wait()` returns only after that drain.

`chttpsvr_engine_wait()` blocks until the engine has stopped. Call it after
you destroy your last server and before `main` returns, so that the engine's
threads are gone by then. A typical `main` starts the server, calls
`chttpsvr_engine_wait()`, which returns when a signal or an endpoint stops
the engine, and then calls `chttpsvr_destroy()`.

Three process-wide settings tune the engine.

`chttpsvr_set_engine_logger(log)` sends the engine's diagnostics (TLS
handshake failures, bind failures, connections closed after the idle
timeout) to a [clogger](clogger.md) handle, and without it the engine
doesn't print them at all. The engine keeps a logger derived from `log`,
which uses the allocator of `log` and, for a logger from
`clog_create_from_fd`, writes to your descriptor, so keep that descriptor
open while the logger is installed.
`chttpsvr_set_engine_logger(CLOG_NULL)` releases it at once. Call it
after `chttpsvr_engine_wait()` returns and before you destroy the allocator
of `log` or close its descriptor. If you never release it, the engine closes
the derived logger when the process exits. Passing a `log` that is already
closed gives `ccol_invalid_args`, and the engine keeps the logger it had.

`chttpsvr_set_engine_num_reactor_threads(n)` gives the reactor more threads.
One thread is right for most traffic, and more only help when lots of new
TLS connections keep arriving. Call it before the first start.

`chttpsvr_set_engine_mem_mgmt_procs(mp)` makes the engine take its memory
from a custom allocator, while each server sets its own allocator with
`chttpsvr_create_mp`. See [Memory management](memory.md).

## Good to know

Don't destroy a server from its own handler. Calling `chttpsvr_destroy` or
`chttpsvr_engine_wait` from a handler or middleware of that server stops the
program, because those calls would wait for the very request that makes
them. Call `chttpsvr_engine_stop()` and return, or let another thread do the
teardown. For the same reason the server refuses a restart (`chttpsvr_stop`
followed by `chttpsvr_start`) from a handler.

Data that handlers share needs a lock. The handlers of one server run on many
threads at once, and streaming handlers run on other threads again.

Don't trust paths as file names. `chttpsvr_req_path` and
`chttpsvr_req_param` are decoded, so they can contain `..` and a `/` that
arrived as `%2F`. Check them before you use them in the file system.

Be careful with a sub-router on `"/"`. It owns only the exact path `/`, so
its middleware doesn't protect the rest of the site, and for a guard that
must cover every path you want `chttpsvr_use`.

Duplicate routes aren't reported. If you register the same method and
pattern twice both calls succeed, but only the first handler ever runs.

Trailers are not headers. The server checks the trailer fields of a chunked
request body and then discards them, so `chttpsvr_req_header` never returns
them.

SIGPIPE is not a concern. The server neither raises it nor changes its
disposition, so a client that disconnects in the middle of a response can't
kill your process.

For `fork()`, call it before you create a server, or call `exec` right after
the fork. A child that keeps running with a server inherited from its parent
is not supported, see [Concurrency](concurrency.md).

`chttpsvr` is an integer handle and not a pointer, so compare it with
`CHTTPSVR_NULL`. `chttpsvr_destroy` sets your variable to
`CHTTPSVR_NULL` and a second destroy of that variable does nothing, but a
destroy through a different copy of the same handle stops the program
instead of corrupting memory.

## Reference

Overview: [chttpserver(7)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpserver.7.html)

Create and destroy a server:
[chttpsvr_create(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_create.3.html),
[chttpsvr_create_mp(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_create_mp.3.html),
[chttpsvr_destroy(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_destroy.3.html),
[chttpsvr_declare(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_declare.3.html),
[chttpsvr_declare_scoped(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_declare_scoped.3.html),
[chttpsvr_construct(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_construct.3.html),
[chttpsvr_construct_scoped(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_construct_scoped.3.html)

Start and stop:
[CHTTPSVR_CONFIG_DEFAULT(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/CHTTPSVR_CONFIG_DEFAULT.3.html),
[chttpsvr_start(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_start.3.html),
[chttpsvr_stop(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_stop.3.html),
[chttpsvr_is_running(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_is_running.3.html),
[chttpsvr_bound_port(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_bound_port.3.html),
[chttpsvr_bound_address(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_bound_address.3.html)

The shared engine:
[chttpsvr_engine_stop(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_engine_stop.3.html),
[chttpsvr_engine_wait(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_engine_wait.3.html),
[chttpsvr_set_engine_logger(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_set_engine_logger.3.html),
[chttpsvr_set_engine_num_reactor_threads(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_set_engine_num_reactor_threads.3.html),
[chttpsvr_set_engine_mem_mgmt_procs(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_set_engine_mem_mgmt_procs.3.html)

Routes, middleware and sub-routers:
[chttpsvr_register_handler(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_register_handler.3.html),
[chttpsvr_register_streaming_handler(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_register_streaming_handler.3.html),
[chttpsvr_use(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_use.3.html),
[chttpsvr_subrouter(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_subrouter.3.html),
[chttpsvr_router_on(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_router_on.3.html),
[chttpsvr_router_on_stream(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_router_on_stream.3.html),
[chttpsvr_router_use(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_router_use.3.html)

The request:
[chttpsvr_req_method(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_req_method.3.html),
[chttpsvr_req_path(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_req_path.3.html),
[chttpsvr_req_param(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_req_param.3.html),
[chttpsvr_req_header(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_req_header.3.html),
[chttpsvr_req_raw_query(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_req_raw_query.3.html),
[chttpsvr_req_query(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_req_query.3.html),
[chttpsvr_req_query_one(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_req_query_one.3.html),
[chttpsvr_req_query_oom(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_req_query_oom.3.html),
[chttpsvr_req_body(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_req_body.3.html),
[chttpsvr_req_read(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_req_read.3.html),
[chttpsvr_req_stream_error(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_req_stream_error.3.html),
[chttpsvr_req_set_context(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_req_set_context.3.html),
[chttpsvr_req_context(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_req_context.3.html)

Client certificates:
[chttpsvr_req_peer_cert_verified(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_req_peer_cert_verified.3.html),
[chttpsvr_req_peer_cert_subject(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_req_peer_cert_subject.3.html),
[chttpsvr_req_peer_cert_sha256(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_req_peer_cert_sha256.3.html),
[chttpsvr_req_peer_cert_der(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_req_peer_cert_der.3.html)

The response:
[chttpsvr_resp_set_status(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_resp_set_status.3.html),
[chttpsvr_resp_set_header(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_resp_set_header.3.html),
[chttpsvr_resp_add_header(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_resp_add_header.3.html),
[chttpsvr_resp_write(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_resp_write.3.html),
[chttpsvr_resp_write_str(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_resp_write_str.3.html),
[chttpsvr_resp_printf(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_resp_printf.3.html),
[chttpsvr_resp_write_json(3)](https://abunchofnerds-25.github.io/ccollections/man/chttpserver/chttpsvr_resp_write_json.3.html)

Shared HTTP types:
[chttp(7)](https://abunchofnerds-25.github.io/ccollections/man/chttp/chttp.7.html),
[CHTTP_STATUS(3)](https://abunchofnerds-25.github.io/ccollections/man/chttp/CHTTP_STATUS.3.html),
[chttp_method_str(3)](https://abunchofnerds-25.github.io/ccollections/man/chttp/chttp_method_str.3.html),
[CHTTP_TLS_DEFAULT(3)](https://abunchofnerds-25.github.io/ccollections/man/chttp/CHTTP_TLS_DEFAULT.3.html)

Related guides:
[HTTP client](chttpclient.md),
[JSON](cjson.md),
[Logging](clogger.md),
[Concurrency](concurrency.md),
[Memory management](memory.md),
[Design of the library](design.md),
[Building](building.md)
