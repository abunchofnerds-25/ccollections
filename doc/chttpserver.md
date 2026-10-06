# chttpserver: an embedded HTTP/1.1 server

`chttpserver` puts an HTTP/1.1 server in your C program. You register a
handler function for a method and a path pattern, and you start the server.
The library does all the other work:

- It accepts connections.
- It parses requests.
- It runs your handlers on a pool of worker threads.
- It writes the responses.

TLS, mutual TLS, middleware, sub-routers and streaming uploads are part of
the module.

Use the server in these cases:

- Your program needs a REST or JSON API, a health or metrics endpoint, a
  webhook receiver, or an admin page.
- You want the routing style of the Go `net/http` package in plain C. That
  is, a pattern such as `/users/{id}`, and middleware with a `next` call.
- You do not want to run a separate web server or link a large framework.

Use a different tool when you need one of these features:

- HTTP/2 or HTTP/3,
- WebSockets (a `101 Switching Protocols` upgrade),
- static files with range requests,
- virtual hosts, selected by the `Host` header.

A full web server such as nginx gives these features. Such a server can
also be in front of this one.

```c
#include <ccollections/chttpserver.h>
```

The header includes `chttp.h`. `chttp.h` holds the types that the server
shares with the [HTTP client](chttpclient.md): `chttp_method_t`, the
`CHTTP_STATUS_*` constants and `chttp_tls_config_t`. The module needs
OpenSSL. [Building](building.md) explains the dependency. It also explains
how to build the library without the HTTP modules.

## A first example

This server answers `GET /hello`. It stops when you push Ctrl-C:

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
        chttpsvr_engine_wait();   /* the engine can be in the process of a stop */
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

Four calls do the work of the full program:

1. `ccol_create_chttpsvr` makes a server. With `CLOG_INVALID`, the server
   keeps its own quiet logger, which prints only fatal messages. To make the
   server log through a [clogger](clogger.md) handle, give that handle.
2. `chttpsvr_register_handler` adds a route. A request for a different path
   gets a `404`, and your code does not run.
3. `chttpsvr_start` binds the port. It returns when the server listens.
4. `chttpsvr_engine_wait` blocks until a call to `chttpsvr_engine_stop`
   occurs. In this example, the signal handler makes that call. Then
   `chttpsvr_destroy` completes the requests that run, and it frees the
   server. On the error path, a failed start can start the engine. Therefore,
   the program calls `chttpsvr_engine_wait` after the destroy. This lets the
   engine stop fully before `main` returns.

## How the server runs your code

All the servers of a process share one background engine. The engine has
two parts:

- A reactor thread. It accepts connections and parses the request headers.
- A sweep thread. It applies the timeouts.

The first `chttpsvr_start` starts the engine. The engine stops when the last
server is destroyed, or when you call `chttpsvr_engine_stop`. You do not
start the engine manually.

Each server has its own pool of worker threads. When the headers of a
request arrive, the reactor finds the route. Then it gives the request to a
worker. The worker reads the body and calls your handler. These three facts
are the result:

- **Handlers run at the same time.** Two requests can be in your handlers at
  the same time, on different threads. Use a lock to protect shared state.
- **A handler does not see a body that is only partly read.** For an
  ordinary (buffered) route, the full body is in memory before your handler
  runs.
- **The server sends the response after the handler returns.** The server
  keeps all the data that you write in a buffer. When the handler returns,
  the server sends the data with the correct `Content-Length`.

The `req` and `resp` pointers are valid only until your handler returns.
Each string that you get from `req` is also valid only until then. Copy the
data that you must keep.

## Configure the listener

`chttpsvr_config_t` holds all the settings. Start from
`CHTTPSVR_CONFIG_DEFAULT`, and change only the settings that you must
change:

```c
chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
cfg.host = "127.0.0.1";            /* loopback only */
cfg.port = 8443;
cfg.worker_thread_count = 8;       /* default: one for each CPU core */
cfg.max_body_size = 1024 * 1024;   /* a larger body gets 413 */
```

The defaults are safe for a server on the public internet. Each timeout and
each resource limit is finite.
[CHTTPSVR_CONFIG_DEFAULT(3)](../man/chttpserver/CHTTPSVR_CONFIG_DEFAULT.3)
gives each field and its default. The name of each timeout field ends in
`_us`, and its unit is microseconds.

`host` sets where the server listens:

| `host` | The server listens on |
|---|---|
| `NULL` or `""` (the default) | all interfaces, IPv4 and IPv6 |
| `"0.0.0.0"` | all IPv4 interfaces |
| `"127.0.0.1"`, `"::1"`, `"[::1]"` | exactly that address |
| `"localhost"` or a different name | its first IPv4 address. If it has no IPv4 address, its first IPv6 address. |
| `"unix:///run/myapp.sock"` | a Unix domain socket at that path. The server ignores `port`. |

The port must not be 0. A Unix socket is the only exception. If a different
program uses the address, `chttpsvr_start` fails. It does not silently
listen on a different address. When the server stops, it removes its Unix
socket file. The server does not replace a file that exists, if that file is
not a stale socket.

One process can run more than one server. For example, it can have a public
API and an admin interface on different ports. Create one `chttpsvr` for
each, and start each with its own configuration. The servers share the
engine.

## Routing

A pattern is a path that has segments. A segment that you write as `{name}`
captures the text that the request has at that position:

```c
chttpsvr_register_handler(srv, CHTTP_GET,    "/users",           list_users,  NULL);
chttpsvr_register_handler(srv, CHTTP_POST,   "/users",           create_user, NULL);
chttpsvr_register_handler(srv, CHTTP_GET,    "/users/{id}",      get_user,    NULL);
chttpsvr_register_handler(srv, CHTTP_PUT,    "/teams/{t}/{u}",   add_member,  NULL);
```

In the handler, `chttpsvr_req_param(req, "id")` gives the captured value.
The server has decoded the percent-encoding of that value. The last argument
of each registration is a `void *ctx`. The server gives this pointer to your
handler without a change. Use it to give the state of your application to
the handler.

The server gives these answers automatically:

- A path that matches no pattern gets `404 Not Found`.
- A path that matches, with a method that no route accepts, gets
  `405 Method Not Allowed` with an `Allow` header.
- A `GET` route also answers `HEAD`. Your handler runs as for `GET`, and the
  server sends the headers without the body.
- A method that is not one of the seven methods of `chttp_method_t` (for
  example `TRACE`) gets `501 Not Implemented`.

`CHTTP_ANY` registers one handler for all the methods of a pattern. To find
which method arrived, call `chttpsvr_req_method(req)`. The server tries the
routes of one router in the order of registration. Therefore, register specific
methods before a `CHTTP_ANY` fallback on the same pattern.

A pattern must start with `/`. It must not end with `/`, and it must not
contain `//`. A request path that ends with a slash does not match a pattern
without that slash. For example, `/users/42/` does not match `/users/{id}`.

You can add routes and middleware at any time, also while the server runs.
[chttpsvr_register_handler(3)](../man/chttpserver/chttpsvr_register_handler.3)
gives the full rules for a match.

## Read the request

```c
chttp_method_t m    = chttpsvr_req_method(req);
const char *path    = chttpsvr_req_path(req);        /* decoded, no query */
const char *ctype   = chttpsvr_req_header(req, "content-type");  /* all cases */
const char *id      = chttpsvr_req_param(req, "id");

size_t len;
const void *body    = chttpsvr_req_body(req, &len);  /* buffered routes */
```

The string getters return NULL for a header, a parameter or a body that the
request does not have. The body does not end with a NUL. Always use `len`.

The server decodes the query string only when you ask for it. For a key that
occurs one time, use `chttpsvr_req_query_one`. This call fails with
`ccol_not_permitted` when the key occurs more than one time. For a key that
can occur more than one time (`?tag=a&tag=b`), use `chttpsvr_req_query`:

```c
const char *sort = NULL;
if (chttpsvr_req_query_one(req, "sort", &sort) == ccol_success && sort)
    chttpsvr_resp_printf(resp, "sorted by %s\n", sort);

size_t n;
const char **tags = chttpsvr_req_query(req, "tag", &n);   /* n is 0 if the key is missing */
for (size_t i = 0; i < n; i++)
    chttpsvr_resp_printf(resp, "tag %s\n", tags[i]);
```

`chttpsvr_req_raw_query` gives the query string without decoding. Use it
when you want to parse the query string yourself.

## Write the response

The default status is `200 OK`. You can write the body in as many parts as
you want:

```c
chttpsvr_resp_set_status(resp, CHTTP_STATUS_CREATED);
chttpsvr_resp_set_header(resp, "location", "/users/42");
chttpsvr_resp_add_header(resp, "set-cookie", "a=1; Path=/");
chttpsvr_resp_add_header(resp, "set-cookie", "b=2; Path=/");
chttpsvr_resp_printf(resp, "created user %d\n", 42);
```

- `chttpsvr_resp_write`, `chttpsvr_resp_write_str` and `chttpsvr_resp_printf`
  add data to the end of the body.
- `chttpsvr_resp_write_json` adds a JSON text to the end of the body, and it
  sets `Content-Type: application/json`.
- `chttpsvr_resp_set_header` gives a header one value.
  `chttpsvr_resp_add_header` adds one more line with the same name.

The server sets `Date`, `Content-Length` and `Connection` itself. The server
refuses a header value that contains CR or LF. Therefore, if you copy request
data into a header, that data cannot split the response. A `204`, a `304`
and a `1xx` never have a body, also when you wrote one.

## Middleware and sub-routers

A middleware runs before the handler. It decides if the request continues.
To continue, it calls `next`. Or it answers the request itself and returns:

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

`chttpsvr_use` adds a middleware that runs for each request. It also runs
for the `404`, `405` and `413` answers that the server makes itself. Therefore,
an access log or a rate limiter that is a middleware also sees that
traffic.

A sub-router puts routes in a group below a path prefix. It has its own
middleware:

```c
chttpsvr_router *admin = chttpsvr_subrouter(srv, "/admin");
chttpsvr_router_use(admin, require_api_key, "s3cret");
chttpsvr_router_on(admin, CHTTP_GET,  "/stats",  show_stats,  NULL);  /* /admin/stats */
chttpsvr_router_on(admin, CHTTP_POST, "/reload", reload_conf, NULL);  /* /admin/reload */
```

A sub-router owns all the paths below its prefix. These rules apply to a
request for `/admin/anything`:

- The request always goes through the admin middleware. This is also true
  when the request matches no admin route. Then the request gets a `404`
  from the sub-router.
- No route that you register on the server itself can answer the request.

Therefore, a sub-router is the correct location for a guard. The order of
execution is:

1. the global middleware,
2. the middleware of the sub-router,
3. the handler.

## Example: a small JSON service

This is a complete REST service. It does these tasks:

- It keeps JSON documents in memory, in a [cjson](cjson.md) dictionary. A
  mutex protects the dictionary, because handlers run on more than one
  thread.
- It protects the API with a bearer token in a sub-router.
- It logs each request with a global middleware.
- It stops itself on `POST /admin/shutdown`.

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
    pthread_mutex_t lock;  /* handlers run on more than one worker thread */
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
        cjson_destroy(doc);   /* refused: we continue to own the document */
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
    chttpsvr_engine_stop();   /* returns immediately; main() does the remaining work */
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

    /* Each registration can fail. A server without its token guard must not
       start. Therefore, examine the result of each registration. */
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
        chttpsvr_engine_wait();   /* the engine can be in the process of a stop */
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

Look at how `put_note` handles ownership. `cjson_dictionary_set` takes the
document on success and on most failures. But when it gives
`ccol_invalid_args`, the caller continues to own the document. In this
example, that occurs for an `id` that is not valid UTF-8.

## Streaming uploads

A buffered route keeps the full body in memory before the handler runs. The
maximum is `max_body_size`. For large uploads, register a **streaming**
route. Its handler starts immediately when the headers arrive. The handler
reads the body in parts with `chttpsvr_req_read`. This function operates
like `read(2)`. It gives the number of bytes, `0` at the end of the body,
or `-1` after an error.

Streaming handlers run on their own pool of `streaming_thread_count`
threads. Therefore, slow uploads cannot use the threads that serve your other
routes.

The example below calculates a hash of uploads of all sizes in constant
memory. It also shows a useful property. A client can send
`Expect: 100-continue`, and curl does this for large uploads. Then the client
waits for permission from the server. The server gives this permission only
at the first `chttpsvr_req_read`. Therefore, a handler that refuses the request
before it reads prevents the upload of the body.

```c
/* upload: calculate the hash of a request body while it arrives. Do not
 * keep the body in a buffer.
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
        /* Refuse before you read. A client that sent
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
    cfg.streaming_thread_count = 2;       /* a maximum of two uploads at the same time */
    if (chttpsvr_register_streaming_handler(srv, CHTTP_PUT, "/upload/{name}",
                                            upload, NULL) != ccol_success ||
        chttpsvr_start(srv, &cfg) != ccol_success) {
        chttpsvr_destroy(srv);
        chttpsvr_engine_wait();   /* the engine can be in the process of a stop */
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

When `chttpsvr_req_read` gives `-1`, `chttpsvr_req_stream_error` gives the
cause:

- `ccol_msg_too_large`: the body is larger than `max_body_size`.
- `ccol_timed_out`: the client stopped, or it was too slow.
- `ccol_http_transfer_aborted`: the connection broke, or the body was not
  correct.

A sub-router registers streaming routes with `chttpsvr_router_on_stream`.

## Slow and hostile clients

A server on the internet gets clients that do these things:

- They send one byte each second.
- They open connections and do not use them.
- They upload more data than you can keep.

The defaults protect against these cases. Usually, you do not have to
change them. This is a summary:

- **On a buffered route, no thread waits for a slow client.** Sometimes a
  body stops, or the client reads a response slowly. Then the server parks
  the connection without a thread until the socket is ready again. Therefore,
  the thread that completes a request is possibly not the thread that
  started it. Do not keep request state in thread-local storage.
- **Each phase has a deadline.** The phases are:
  - the headers (`max_header_read_duration_us`),
  - the body (`stream_read_timeout_us`, `max_body_read_duration_us`),
  - the response (`response_write_timeout_us`,
    `max_response_write_duration_us`),
  - the idle time between keep-alive requests (`idle_timeout_us`).
- **There is a minimum transfer rate** (`min_transfer_rate_bps`). It is 240
  bytes each second, after a grace period of 5 seconds. The server
  disconnects a client that stays in each deadline but moves almost no
  data.
- **There is a limit on body memory** (`max_partial_body_memory`). The limit
  is 256 MiB for all the buffered bodies together. A request that does not
  fit waits in a queue. It does not fail. It gets `503` only if it waits too
  long.
- **The queues have a limit.** When the worker queue or the streaming queue
  is full, the client immediately gets `503 Service Unavailable` with
  `Retry-After: 5`.

The server refuses requests that are not correct or not clear before your code
runs. Examples are a bad `Host`, framing headers that do not agree, and an HTTP
version that the server does not support. Sometimes the server refuses a request
while its body continues to arrive. Then the server continues to read and
discard the body for a maximum of 2 seconds before it closes the connection.
Therefore, the client can read the answer, and it does not get a connection
reset.

[chttpsvr_start(3)](../man/chttpserver/chttpsvr_start.3) describes each
limit, each answer that the server makes itself, and the rules for request
validation.
[CHTTPSVR_CONFIG_DEFAULT(3)](../man/chttpserver/CHTTPSVR_CONFIG_DEFAULT.3)
explains how to turn off each limit. For most fields, the value 0 turns the
limit off. But for the defenses from `min_transfer_rate_bps` to
`streaming_queue_timeout_us`, 0 means the default value. Therefore, a
configuration that does not set these fields keeps the defenses. To turn
off one of these defenses, use a named constant such as
`CHTTPSVR_NO_RATE_FLOOR`.

## TLS and client certificates

Set `cfg.tls` to point to a `chttp_tls_config_t` with a certificate and a
key. Then the server uses HTTPS:

```c
chttp_tls_config_t tls = {
    .cert_path = "/etc/myapp/server.crt",
    .key_path  = "/etc/myapp/server.key",
};
cfg.tls = &tls;
```

`chttpsvr_start` fails in these cases:

- A file is missing.
- The server cannot read a file.
- The certificate does not agree with the key.

The server never uses plain HTTP in place of HTTPS.

To require **client certificates** (mutual TLS), also set `ca_bundle_path`.
Then a client without a certificate that verifies against that bundle
cannot complete the handshake. With `client_cert_optional = true`, the
server also accepts clients without a certificate, and each handler
decides. A handler finds the identity of the client with these functions:

- `chttpsvr_req_peer_cert_verified`,
- `chttpsvr_req_peer_cert_subject` (for example `CN=alice,O=Example`),
- `chttpsvr_req_peer_cert_sha256` (a fingerprint to compare with an allow
  list),
- `chttpsvr_req_peer_cert_der`.

This server gives a public page to all clients. It gives a private page only
to clients with a certificate:

```c
/* mtls: an HTTPS service that knows the identity of its clients.
 *
 *   ./mtls PORT server.crt server.key clients-ca.pem
 *
 *   GET /public    all clients can call it
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
        /* The certificate is verified. Therefore, only an allocation can fail. */
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
        chttpsvr_engine_wait();   /* the engine can be in the process of a stop */
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

The bundle can also contain certificate revocation lists. The server then
applies them. The [HTTP client guide](chttpclient.md) and
[CHTTP_TLS_DEFAULT(3)](../man/chttp/CHTTP_TLS_DEFAULT.3) explain the fields
of `chttp_tls_config_t`.

## Start, stop and shut down

These calls stop a server:

- `chttpsvr_stop(srv)` closes only the listening socket. The server
  continues to serve the connections that are open until they close or time
  out. To start the server again, call `chttpsvr_start` again. You can give
  a new configuration. The server listens again immediately, while the
  requests of the previous run complete in the background.
- `chttpsvr_destroy(srv)` closes the listener and all the connections. It
  waits for the handlers that run. Then it frees the server and sets the
  handle to `CHTTPSVR_INVALID`.
- `chttpsvr_engine_stop()` stops all the servers of the process. It does not
  block, and it is async-signal-safe. Therefore, use it in a `SIGTERM` handler.
  You can also safely call it from a handler, as the shutdown endpoint above
  does.
- `chttpsvr_engine_wait()` blocks until the engine has stopped. Call it
  after you destroy your last server, before `main` returns. Then the
  threads of the engine do not exist any more.

Therefore, `main` usually does these steps:

1. Start the server.
2. Call `chttpsvr_engine_wait()`. It returns when a signal or an endpoint
   stops the engine.
3. Call `chttpsvr_destroy()`.

Three settings for the full process tune the engine:

- `chttpsvr_set_engine_logger(log)` sends the diagnostics of the engine to a
  [clogger](clogger.md) handle. Examples are TLS handshake failures, bind
  failures, and connections that the server closes after the idle timeout.
  Without this setting, the engine does not print these diagnostics.
- `chttpsvr_set_engine_num_reactor_threads(n)` gives the reactor more
  threads. One thread is correct for most traffic. More threads help only
  when many new TLS connections arrive continuously. Call this function
  before the first start.
- `chttpsvr_set_engine_mem_mgmt_procs(mp)` makes the engine get its memory
  from a custom allocator. Each server sets its own allocator with
  `ccol_create_chttpsvr_mp`. See [Memory management](memory.md).

## Good to know

- **Do not destroy a server from its own handler.** A call to
  `chttpsvr_destroy` or `chttpsvr_engine_wait` from a handler or middleware
  of that server stops the program. The reason is that these calls would wait
  for the request that calls them. Call `chttpsvr_engine_stop()` and return, or
  let a different thread do the teardown. The server also refuses a restart
  (`chttpsvr_stop` and then `chttpsvr_start`) from a handler, for the same
  reason.
- **Use a lock for the data that handlers share.** The handlers of one
  server run on many threads at the same time. Streaming handlers run on
  other threads too.
- **Do not trust paths as file names.** The server decodes `chttpsvr_req_path`
  and `chttpsvr_req_param`. Therefore, they can contain `..`, and they can
  contain a `/` that arrived as `%2F`. Examine them before you use them in the
  file system.
- **Be careful with a sub-router on `"/"`.** It owns only the exact path
  `/`. Therefore, its middleware does not protect the other paths of the site.
  For a guard that must apply to all paths, use `chttpsvr_use`.
- **The server does not report duplicate routes.** If you register the same
  method and pattern two times, the call succeeds. But only the first
  handler runs.
- **Trailers are not headers.** The server examines the trailer fields of a
  chunked request body, and then it discards them. `chttpsvr_req_header`
  never gives them.
- **SIGPIPE is safe.** The server does not raise it, and it does not change
  its disposition. Therefore, a client that disconnects during a response cannot
  stop your process.
- **fork().** Call `fork()` before you create a server, or call `exec`
  immediately after the fork. The library does not support a child that
  continues to run with a server from its parent. See
  [Concurrency](concurrency.md).
- **The handle is a value.** `chttpsvr` is an integer handle, not a pointer.
  Compare it with `CHTTPSVR_INVALID`. `chttpsvr_destroy` sets your variable
  to `CHTTPSVR_INVALID`, and a second destroy of that variable does nothing.
  A destroy through a different copy of the same handle stops the program.
  It does not corrupt memory.

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
