# HTTP client (`chttpclient`)

`chttpclient` lets a C program communicate through HTTP/1.1. For example, a
program can get a page, call a JSON API, upload a file or download a large
file. The client supports plain HTTP, HTTPS (through OpenSSL) and HTTP
through a Unix domain socket. It has these features:

- It keeps connections open and uses them again.
- It follows redirects.
- It verifies certificates by default.
- It gives you three ways to run a request: blocking, asynchronous with
  futures, or blocking on a shared event loop.

Use the client when your program must call web services and you want a small
C API. Its defaults are safe and secure. It needs no dependency other than
the dependencies that the library links.

The client is not correct for all tasks:

- It supports only HTTP/1.1. It does not support HTTP/2, HTTP/3 or
  WebSockets.
- It does not support proxies. It does not keep cookies. It does not
  decompress `gzip` bodies.
- If you need one of these features, libcurl is the usual tool.

This guide also describes the small shared header `chttp.h`. This header
gives status codes, request bodies, TLS settings, and helpers for base64 and
Basic auth. You usually use it first through the client. The server side,
[`chttpserver`](chttpserver.md), uses the same types.

## Your first request

```c
#include <ccollections/chttpclient.h>
#include <stdio.h>

int main(int argc, char **argv) {
    const char *url = argc > 1 ? argv[1] : "http://127.0.0.1:8080/hello";

    chttpcli_response *resp = NULL;
    ccol_retval_t rc = chttp_get(url, &resp);
    if (rc != ccol_success) {
        fprintf(stderr, "GET %s failed: %s\n", url, ccol_retval_to_str(rc));
        return 1;
    }

    const char *type = chttpclient_resp_header(resp, "Content-Type");
    printf("status: %d\n", resp->status_code);
    printf("type:   %s\n", type ? type : "(none)");
    printf("body:   %s\n", resp->body ? resp->body : "(empty)");

    chttpclient_resp_free(resp);
    return 0;
}
```

Build and run the example:

```sh
cc -std=gnu11 first.c -o first $(pkg-config --cflags --libs ccollections)
./first https://example.com/
```

The example shows these rules:

- `#include <ccollections/chttpclient.h>` also gives you `chttp.h`.
- Each request gives a `ccol_retval_t`. `ccol_success` is 0.
  `ccol_retval_to_str()` changes each code into a name that you can read.
- `chttp_get()` runs on a *default client* for the full process. The library
  makes this client when you use it the first time. You do not create or
  destroy it.
- The response is yours. Free it with `chttpclient_resp_free()`.

To link against the shared library, `-lccollections` is enough. The
library itself links OpenSSL. A static link needs
`pkg-config --static --libs ccollections`. See [Building](building.md).

## Read a response

A `chttpcli_response` has four fields that you read directly:

| Field | Contents |
|---|---|
| `status_code` | The HTTP status of the final response, for example 200 or 404. |
| `body` | The body in a heap buffer that ends with a NUL, or `NULL`. |
| `body_len` | The number of bytes in the body. Use it for binary data. |
| `headers` | A map from the lower-case header name to the value. |

Obey these two rules to prevent crashes:

1. `body` is `NULL` when the body is empty, for example after a 204, a
   `HEAD` or a `Content-Length: 0`. Examine `body` before you print it.
2. After a failure, the call sets your response pointer to `NULL`.
   `chttpclient_resp_free(NULL)` does nothing. Therefore, it is always correct
   to call `chttpclient_resp_free(resp)` after each request.

A status that is not 2xx is **not** an error. A 404 is a successful HTTP
exchange, and its answer is "not found". The call gives `ccol_success`, and
you examine `status_code`. Compare it with the named constants of `chttp.h`,
for example `CHTTP_STATUS_OK`, `CHTTP_STATUS_CREATED` or
`CHTTP_STATUS_NOT_FOUND`.

The header lookups ignore case. A server can send a header, for example
`Set-Cookie`, on more than one line. You can read such a header one line at
a time (a part of a program):

```c
const char *type = chttpclient_resp_header(resp, "content-type");

size_t n = chttpclient_resp_header_count(resp, "Set-Cookie");
for (size_t i = 0; i < n; i++)
    printf("cookie: %s\n", chttpclient_resp_header_at(resp, "Set-Cookie", i));

/* All the headers, one entry for each name. */
ccol_for_each(resp->headers, it, {
    printf("%s: %s\n", *ccol_iter_key_ptr(it), *ccol_iter_val_ptr(it));
});
```

For a name that occurs more than one time, `chttpclient_resp_header()` gives
all the values with `", "` between them. For `Set-Cookie`, it gives only the
first value.

## Send data, set headers and use your own client

For a request that is more than a simple GET, build a request and set
headers on it. Then run it on a client that you own. On your own client, you
can set timeouts, a limit on the size of the response, and TLS settings. These
settings apply only to that client.

```c
#include <ccollections/chttpclient.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *base = argc > 1 ? argv[1] : "http://127.0.0.1:8080";
    char url[256];
    snprintf(url, sizeof(url), "%s/items", base);

    chttpcli_construct(cli);                            /* stops the program on failure */
    chttpclient_set_connect_timeout(cli, 2000000);      /* 2 s, in us */
    chttpclient_set_request_timeout(cli, 10000000);     /* 10 s, in us */
    chttpclient_set_max_response_body_size(cli, 1 << 20); /* 1 MiB */

    const char *json = "{\"name\":\"widget\",\"qty\":3}";
    chttp_request_t *req = chttp_request_new(
        CHTTP_POST, url, &CHTTP_JSON_BODY(json, strlen(json)), NULL);
    if (!req) {
        chttpclient_destroy(cli);
        return 1;
    }
    if (chttp_request_set_header(req, "Authorization", "Bearer my-token") !=
            ccol_success ||
        chttp_request_set_header(req, "X-Request-Id", "42") != ccol_success) {
        chttp_request_free(req);
        chttpclient_destroy(cli);
        return 1;
    }

    chttpcli_response *resp = NULL;
    ccol_retval_t rc = chttpclient_do(cli, req, &resp);
    chttp_request_free(req);

    if (rc == ccol_success) {
        const char *where = chttpclient_resp_header(resp, "location");
        printf("%d, location=%s\n", resp->status_code, where ? where : "-");
        printf("%.*s\n", (int)resp->body_len, resp->body ? resp->body : "");
    } else {
        fprintf(stderr, "request failed: %s\n", ccol_retval_to_str(rc));
    }

    chttpclient_resp_free(resp);  /* safe: resp is NULL after a failure */
    chttpclient_destroy(cli);
    return rc == ccol_success ? 0 : 1;
}
```

The example does these steps:

- `chttpcli_construct(cli)` declares and creates a client. If this fails, it
  stops the program. To handle the failure yourself, use
  `ccol_create_chttpclient()`. `chttpcli_construct_scoped(cli)` destroys the
  client at the end of the scope.
- `CHTTP_JSON_BODY(data, len)` describes a body and sets `Content-Type` to
  `application/json`. Other macros are `CHTTP_TEXT_BODY`, `CHTTP_FORM_BODY`,
  the general `CHTTP_BODY(data, len, content_type)` and `CHTTP_NO_BODY`.
  `chttp_request_new()` copies the body. Therefore, you can free your buffer
  immediately.
- Only `POST`, `PUT` and `PATCH` send a body. The client adds `Host`,
  `Accept`, `User-Agent`, `Content-Length` and `Content-Type` if you do not
  set them.
- `chttp_request_set_header()` refuses a header value that contains a CR or
  LF byte. Therefore, data that you forward from a different source cannot add
  header lines. The call can also fail when there is not enough memory.
  Examine its return value.
- Timeouts and sizes are plain numbers. Timeouts are in microseconds, and
  the body limit is in bytes. The value 0 means "no limit". This is the
  default for all three settings.
- The request must stay valid while `chttpclient_do()` runs. Free the
  request after the call.
- `chttpclient_destroy()` waits for the requests that run on other threads.
  Then it frees the client and sets `cli` to `CHTTPCLI_INVALID`.

The client of the full process also runs requests: `chttp_do(req, &resp)`.
`chttp_run_query(method, url, body, headers, &resp)` runs a single request
with a header map in one call (a part of a program):

```c
chmap_construct(headers, char *, char *);
chmap_insert(headers, "X-Api-Version", "2");

chttpcli_response *resp = NULL;
chttp_run_query(CHTTP_POST, url, &CHTTP_TEXT_BODY("ping", 4), headers, &resp);
chmap_destroy(headers);          /* the call only borrowed the map */
chttpclient_resp_free(resp);
```

A `chttpcli` is a *value handle* (a 64-bit number). It is not a pointer.
Compare it with `CHTTPCLI_INVALID`. `CHTTPCLI_INVALID` is 0. Therefore,
`if (!cli)` also operates correctly. Do not cast the handle to a pointer.

## Errors

This table shows the usual codes and their usual causes:

| Code | Usual cause |
|---|---|
| `ccol_http_invalid_url` | A typing error, a space in the host, or a scheme that the client does not support. |
| `ccol_http_host_resolution_failed` | DNS cannot find the host. |
| `ccol_http_connection_failed` | No server listens at that address, or all the addresses refused the connection. |
| `ccol_timed_out` | Your connect timeout or request timeout expired. |
| `ccol_http_tls_cert_verification_failed` | The client does not trust the server certificate, or the certificate names a different host. |
| `ccol_http_tls_cert_load_failed` | The client cannot read a CA bundle, certificate or key file that you set. |
| `ccol_http_transfer_aborted` | The connection broke during the transfer, or the server sent HTTP that is not correct. |
| `ccol_msg_too_large` | The body is larger than the limit of `chttpclient_set_max_response_body_size()`. |
| `ccol_http_too_many_redirects` | More than 50 redirects. |

[chttpclient_do(3)](../man/chttpclient/chttpclient_do.3) gives the full list,
with all the causes of each code.

## Large downloads: stream the body

A buffered response keeps the full body in memory. For a large file, tell
the client to give you the body in parts. Your callback gets each part when
it arrives. The callback gives the number of bytes that it used. All other
return values stop the transfer.

```c
/* download.c: write the body of a URL into a file. Do not keep the full
 * body in memory. */
#include <ccollections/chttpclient.h>
#include <stdio.h>

typedef struct {
    FILE  *out;
    size_t bytes;
} sink_t;

static size_t write_chunk(const void *data, size_t len, void *ctx) {
    sink_t *s = ctx;
    size_t n = fwrite(data, 1, len, s->out);
    s->bytes += n;
    return n;              /* a value other than len stops the transfer */
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s URL FILE\n", argv[0]);
        return 2;
    }

    sink_t sink = {.out = fopen(argv[2], "wb")};
    if (!sink.out) {
        perror(argv[2]);
        return 1;
    }

    chttpcli_construct(cli);
    chttpclient_set_connect_timeout(cli, 5000000);   /* 5 s */

    chttp_request_t *req = chttp_request_new(CHTTP_GET, argv[1], NULL, NULL);
    int status = 0;
    ccol_retval_t rc = req ? chttpclient_do_streaming(cli, req, write_chunk,
                                                      &sink, &status)
                           : ccol_not_enough_memory;
    chttp_request_free(req);
    chttpclient_destroy(cli);

    if (fclose(sink.out) != 0 && rc == ccol_success)
        rc = ccol_unexpected_failure;
    if (rc != ccol_success) {
        fprintf(stderr, "download failed: %s\n", ccol_retval_to_str(rc));
        return 1;
    }
    printf("HTTP %d, %zu bytes written to %s\n", status, sink.bytes, argv[2]);
    return status == CHTTP_STATUS_OK ? 0 : 1;
}
```

Run the program as `./download https://example.com/big.iso big.iso`. The
streaming calls give you the status code. They do not give you the response
headers. The limit on the body size does not apply to them. Your callback
controls the transfer.

## Redirects

The client follows a maximum of 50 redirects automatically. Then it gives you
the final response. These rules apply:

- A 301, 302 or 303 changes the request into a GET with no body. A `HEAD`
  stays a `HEAD`.
- A 307 or 308 sends the same method and the same body again.
- The client gives you a redirect that has no `Location` as the response.

The client does not send credentials to a different site. A redirect can
go to a scheme, host or port that is different from the original request.
Then the client stops the transmission of `Authorization`, `Cookie` and
similar headers. If the chain goes back to the original scheme, host and port, the
client sends these headers again.

By default, the client follows a redirect from `https` to `http`. curl and
Go do the same. If a downgrade must not occur, refuse it for each request
(a part of a program):

```c
req->prevent_tls_downgrade_on_redirect = true;  /* a downgrade: ccol_http_invalid_url */
```

## HTTPS and certificates

By default, the client verifies the certificate chain of the server against
the CA store of the system. It also makes sure that the certificate names the
host that you requested. To change this behavior, use
`chttpclient_set_tls()` and a `chttp_tls_config_t`.

In this struct, **zero is the safe value**. Each field that you do not set
keeps verification on. Each field that makes the security weaker says so in
its name (a part of a program):

```c
/* Trust a private CA, not the system store. The client verifies fully. */
chttp_tls_config_t tls = {.ca_bundle_path = "/etc/myapp/ca.pem"};
chttpclient_set_tls(cli, &tls);

/* Send a client certificate (mutual TLS). Set the two paths together. */
chttp_tls_config_t mtls = {
    .ca_bundle_path = "/etc/myapp/ca.pem",
    .cert_path      = "/etc/myapp/client.pem",
    .key_path       = "/etc/myapp/client.key",
};
chttpclient_set_tls(cli, &mtls);

/* Keep the chain check, but do not check the host name. Use this for a
   server that you connect to by an IP address that its certificate does
   not contain. */
chttp_tls_config_t by_ip = {
    .ca_bundle_path = "/etc/myapp/ca.pem",
    .insecure_skip_hostname_check = true,
};
chttpclient_set_tls(cli, &by_ip);

/* Accept all certificates. Use this only for temporary test servers. */
chttp_tls_config_t insecure = {.insecure_skip_verify = true};
chttpclient_set_tls(cli, &insecure);

chttpclient_set_tls(cli, NULL);   /* set the defaults again */
```

The client reads the files when a request needs them for the first time.
Therefore, an incorrect path causes `ccol_http_tls_cert_load_failed` from the
request, not from `chttpclient_set_tls()`. A CA bundle can also contain
certificate revocation lists. See
[chttpclient_set_tls(3)](../man/chttpclient/chttpclient_set_tls.3).

## Many requests at the same time

There are three ways to run a request. All three obey the same rules for
URLs, redirects, TLS and errors. They are different in which thread waits.

| Tier | Functions | Which thread waits |
|---|---|---|
| 1. Blocking | `chttpclient_do`, `chttpclient_do_streaming` | Your thread, for the full request. |
| 2. Asynchronous | `chttpclient_do_async`, `chttpclient_do_async_streaming` | No thread. You get a future and you get the result from it later. |
| 3. Pooled blocking | `chttpclient_do_pooled`, `chttpclient_do_pooled_streaming` | Your thread, but the work runs on a small shared event loop. |

Tiers 2 and 3 run on one engine for each process. All the clients share this
engine. The engine starts when you use it the first time. It stops
automatically when it has no work.

### More than one thread, one client (Tier 1)

A client is thread-safe. Many threads can run requests on it at the same
time. `chttpclient_set_pool_size()` sets the maximum number of blocking
requests that run at the same time. Other callers wait until a request
completes. This program examines many URLs in parallel and tells if each
server operates:

```c
/* updown.c: examine many URLs in parallel and print the answer of each. */
#include <ccollections/chttpclient.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

typedef struct {
    chttpcli      cli;
    const char   *url;
    int           status;
    double        ms;
    ccol_retval_t rc;
} check_t;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void *check(void *arg) {
    check_t *c = arg;
    double t0 = now_ms();
    chttp_request_t *req = chttp_request_new(CHTTP_HEAD, c->url, NULL, NULL);
    chttpcli_response *resp = NULL;
    c->rc = req ? chttpclient_do(c->cli, req, &resp) : ccol_not_enough_memory;
    c->status = resp ? resp->status_code : 0;
    c->ms = now_ms() - t0;
    chttpclient_resp_free(resp);
    chttp_request_free(req);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s URL...\n", argv[0]);
        return 2;
    }
    int n = argc - 1;

    chttpcli_construct(cli);
    chttpclient_set_pool_size(cli, 4);               /* a maximum of 4 at the same time */
    chttpclient_set_connect_timeout(cli, 3000000);   /* 3 s */
    chttpclient_set_request_timeout(cli, 5000000);   /* 5 s */

    check_t   checks[n];
    pthread_t tids[n];
    int       started = 0;
    for (int i = 0; i < n; i++) {
        checks[i] = (check_t){.cli = cli, .url = argv[i + 1]};
        if (pthread_create(&tids[i], NULL, check, &checks[i]) != 0)
            break;
        started++;
    }
    for (int i = 0; i < started; i++)
        pthread_join(tids[i], NULL);

    int down = 0;
    for (int i = 0; i < started; i++) {
        if (checks[i].rc == ccol_success) {
            printf("UP    %3d %7.1f ms  %s\n", checks[i].status, checks[i].ms,
                   checks[i].url);
        } else {
            printf("DOWN  %s  (%s)\n", checks[i].url,
                   ccol_retval_to_str(checks[i].rc));
            down++;
        }
    }

    chttpclient_destroy(cli);
    return down == 0 && started == n ? 0 : 1;
}
```

```sh
$ ./updown http://127.0.0.1:8080/hello http://127.0.0.1:8080/slow \
           http://127.0.0.1:1/ http://no-such-host.invalid/
UP    200     0.4 ms  http://127.0.0.1:8080/hello
UP    200  2000.5 ms  http://127.0.0.1:8080/slow
DOWN  http://127.0.0.1:1/  (ccol_http_connection_failed)
DOWN  http://no-such-host.invalid/  (ccol_http_host_resolution_failed)
```

With hundreds of threads, use `chttpclient_do_pooled` in place of
`chttpclient_do`. The arguments and the result are the same. But the network
work runs on the few threads of the engine. It does not keep one thread busy
for each request.

### Start now, get the result later (Tier 2)

With `chttpclient_do_async()`, one thread can start many requests, do other
work, and get the answers later. Each call immediately gives a
`ctpool_future`. `chttpclient_async_result_get()` waits for the future.

```c
/* fanout.c: start many requests at the same time from one thread. Then
 * get the results. */
#include <ccollections/chttpclient.h>
#include <stdio.h>

int main(int argc, char **argv) {
    const char *base = argc > 1 ? argv[1] : "http://127.0.0.1:8080";
    const char *paths[] = {"/hello", "/slow", "/status/404", "/redirect"};
    enum { N = sizeof(paths) / sizeof(paths[0]) };

    chttpcli_construct(cli);
    chttpclient_set_request_timeout(cli, 5000000);   /* 5 s for each request */

    ctpool_future *futures[N] = {0};
    for (int i = 0; i < N; i++) {
        char url[256];
        snprintf(url, sizeof(url), "%s%s", base, paths[i]);
        chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
        if (!req)
            continue;
        futures[i] = chttpclient_do_async(cli, req);   /* returns immediately */
        chttp_request_free(req);   /* the engine keeps its own copy */
    }

    /* The requests run now. This thread can do other work. */

    for (int i = 0; i < N; i++) {
        if (!futures[i]) {
            printf("%-12s not submitted\n", paths[i]);
            continue;
        }
        chttpcli_async_result_t *r = chttpclient_async_result_get(futures[i]);
        if (r && r->rv == ccol_success) {
            printf("%-12s %d %zu bytes\n", paths[i], r->resp->status_code,
                   r->resp->body_len);
            chttpclient_resp_free(r->resp);
        } else {
            printf("%-12s failed: %s\n", paths[i],
                   ccol_retval_to_str(r ? r->rv : ccol_unexpected_failure));
        }
        chttpclient_async_result_free(r);
        ctpool_future_free(futures[i]);
    }

    chttpclient_destroy(cli);
    return 0;
}
```

Free a result in this fixed order:

1. Free `r->resp`. It is there only when `r->rv == ccol_success`.
2. Free the result.
3. Free the future.

You can free the request immediately after `chttpclient_do_async()`
returns.

With `chttpclient_do_async_streaming()`, your write callback runs on one of
the threads of the engine. The callback must be fast, and it must not block.
It must not start a different asynchronous request, and it must not wait
for one.

## A realistic API client

This program communicates with a small JSON API. It does these steps:

1. It authenticates with HTTP Basic auth.
2. It reads the current user.
3. It creates an item from a document that it builds with
   [`cjson`](cjson.md).

Most programs have a function similar to the `call_api()` helper. This
helper is the one location that sends a request, examines the answer and
parses it.

```c
/* inventory.c: communicate with a small JSON API with Basic auth. */
#include <ccollections/chttpclient.h>
#include <ccollections/cjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Send one request and parse a JSON answer. Return the parsed document
   (NULL after all failures), and store the HTTP status in *status. */
static cjson call_api(chttpcli cli, chttp_method_t method, const char *url,
                      const char *auth, const char *json_body, int *status) {
    *status = 0;   /* 0 means "no HTTP answer" */
    chttp_request_body_t body = CHTTP_NO_BODY;
    if (json_body)
        body = CHTTP_JSON_BODY(json_body, strlen(json_body));

    chttp_request_t *req = chttp_request_new(method, url, &body, NULL);
    if (!req)
        return NULL;
    if (chttp_request_set_header(req, "Authorization", auth) != ccol_success ||
        chttp_request_set_header(req, "Accept", "application/json") !=
            ccol_success) {
        chttp_request_free(req);
        return NULL;
    }

    chttpcli_response *resp = NULL;
    ccol_retval_t rc = chttpclient_do(cli, req, &resp);
    chttp_request_free(req);
    if (rc != ccol_success) {
        fprintf(stderr, "%s %s: %s\n", chttp_method_str(method), url,
                ccol_retval_to_str(rc));
        return NULL;
    }

    *status = resp->status_code;
    cjson doc = NULL;
    if (resp->body) {
        char *err = NULL;   /* the library owns it; we do not free it */
        doc = cjson_parse_n(resp->body, resp->body_len, &err);
        if (!doc)
            fprintf(stderr, "bad JSON from %s: %s\n", url, err);
    }
    chttpclient_resp_free(resp);
    return doc;
}

int main(int argc, char **argv) {
    const char *base = argc > 1 ? argv[1] : "http://127.0.0.1:8080";
    char url[256];
    int status = 0;

    char *auth = chttp_basic_auth("alice", "s3cret");   /* "Basic ..." */
    if (!auth)
        return 1;

    chttpcli_construct(cli);
    chttpclient_set_request_timeout(cli, 5000000);

    /* 1. Get the current user. */
    snprintf(url, sizeof(url), "%s/me", base);
    cjson me = call_api(cli, CHTTP_GET, url, auth, NULL, &status);
    /* The server sets the document. Examine each type before you read it. */
    cjson user = me ? cjson_get(me, "user") : NULL;   /* borrowed */
    if (status == CHTTP_STATUS_OK && cjson_type(user) == CJSON_STRING)
        printf("logged in as %s\n", cjson_str_val(user));
    cjson_destroy(me);

    /* 2. Create an item from a JSON document that we build. */
    cjson item = cjson_create_dictionary();
    char *text = NULL;
    if (item && cjson_set(item, "name", "widget") == ccol_success &&
        cjson_set(item, "qty", 3) == ccol_success)
        text = cjson_serialize(item);
    cjson_destroy(item);

    cjson created = NULL;
    status = 0;
    if (text) {
        snprintf(url, sizeof(url), "%s/items", base);
        created = call_api(cli, CHTTP_POST, url, auth, text, &status);
        cjson_serialize_free(text);
    }
    cjson name = created ? cjson_get(created, "name") : NULL;
    cjson qty = created ? cjson_get(created, "qty") : NULL;
    if (status == CHTTP_STATUS_CREATED && cjson_type(name) == CJSON_STRING &&
        cjson_type(qty) == CJSON_INTEGER)
        printf("created %s x%lld\n", cjson_str_val(name), cjson_int_val(qty));
    else
        printf("create failed with HTTP %d\n", status);
    cjson_destroy(created);

    chttpclient_destroy(cli);
    free(auth);
    return 0;
}
```

`chttp_basic_auth()` gives a complete `Basic ...` header value. Free it
with `free()`. The function refuses a user name that contains a colon,
because the receiver cannot find the end of the name. You can also put the
credentials in the URL (`https://alice:s3cret@api.example.com/`). Then the
client makes the same header.

## Unix domain sockets

Many local services (Docker, systemd, your own daemons) listen on a Unix
socket, not on a TCP port. Put the socket path, with percent-encoding, in the
position of the host (a part of a program):

```c
/* Server socket: /run/app.sock */
chttp_get("http+unix://%2Frun%2Fapp.sock/api/status", &resp);
```

All the tiers support Unix sockets, and they use connections again. These
rules apply:

- The default `Host` header is `localhost`.
- There is no TLS through a Unix socket.
- A redirect cannot move a request to a Unix socket.
- On Linux, the socket path must fit in 108 bytes.

## Upload only when the server agrees

A server can refuse an upload before it reads the upload. For example, the
upload is too large, or the user is not logged in. Set `expect_continue`.
Then the client first sends only the headers. It sends the body when the
server answers `100 Continue`. If the server does not answer, the client
sends the body after one second (a part of a program):

```c
chttp_request_t *req = chttp_request_new(CHTTP_PUT, url,
    &CHTTP_BODY(data, len, "application/octet-stream"), NULL);
if (req) {
    req->expect_continue = true;
    chttpclient_do(cli, req, &resp);   /* a 413 here means that the client sent no body */
    chttp_request_free(req);
}
```

If you do not set `expect_continue`, the client also monitors the
connection for an early answer during the upload. It gives that answer,
not a broken-pipe error.

## The shared helpers of chttp.h

You can also use `chttp.h` alone, without the client or the server:

```c
#include <ccollections/chttp.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    /* A complete Authorization header value. */
    char *auth = chttp_basic_auth("Aladdin", "open sesame");
    if (!auth)
        return 1;           /* not enough memory */
    printf("%s\n", auth);   /* Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ== */
    free(auth);

    /* A user name with a colon is not clear to the receiver: NULL. */
    printf("%s\n", chttp_basic_auth("a:b", "c") ? "built" : "refused");

    /* Base64 of binary data, and back. */
    const unsigned char raw[] = {0x00, 0xff, 0x10, 0x00, 0x7f};
    size_t enc_len = 0, dec_len = 0;
    char *enc = chttp_base64_encode(raw, sizeof(raw), &enc_len);
    if (!enc)
        return 1;
    unsigned char *dec = chttp_base64_decode(enc, &dec_len);
    if (dec)
        printf("%s (%zu chars) -> %zu bytes\n", enc, enc_len, dec_len);
    free(enc);
    free(dec);

    /* The decoder accepts only canonical base64. */
    printf("%s\n", chttp_base64_decode("QR==", NULL) ? "decoded" : "refused");
    return 0;
}
```

Each helper has an `_mp` form that takes a custom allocator. The decoded
buffer ends with a NUL to make it easier to use. But binary data can contain
zero bytes. Therefore, use `out_len`, not `strlen()`.
`chttp_method_str(CHTTP_PATCH)` gives `"PATCH"`.

## Good to know

- **Do not destroy the default client.** `chttp_default_client()` and the
  `chttp_get` family belong to the library. The library cleans them at exit.
  If you destroy the default client, all the later convenience calls fail.
- **Destroy your clients before `main` returns.** If a client exists at
  exit, the engine of Tiers 2 and 3 continues to run.
- **An empty body is `NULL`.** `resp->body` is `NULL` for an empty body.
- **Only POST, PUT and PATCH send a body.** The client does not send a body
  that you give to a GET.
- **Requests and responses are not shared objects.** A client is
  thread-safe. A single `chttp_request_t` or `chttpcli_response` is not
  thread-safe. Do not change one from two threads at the same time.
- **Asynchronous callbacks run on the engine.** Keep the streaming
  callbacks of Tiers 2 and 3 short, and do not let them block.
- **`fork()`:** Create clients after `fork()`, or call `exec()` immediately
  after `fork()`. The library does not support a child that continues to
  run with a client from before the fork. See [Concurrency](concurrency.md).
- **SIGPIPE is safe.** The client does not raise it, and it does not change
  your signal settings.

## Reference

Overview: [chttpclient(7)](../man/chttpclient/chttpclient.7)

Clients:
[ccol_create_chttpclient(3)](../man/chttpclient/ccol_create_chttpclient.3),
[ccol_create_chttpclient_mp(3)](../man/chttpclient/ccol_create_chttpclient_mp.3),
[chttpcli_construct(3)](../man/chttpclient/chttpcli_construct.3),
[chttpcli_construct_scoped(3)](../man/chttpclient/chttpcli_construct_scoped.3),
[chttpcli_declare(3)](../man/chttpclient/chttpcli_declare.3),
[chttpcli_declare_scoped(3)](../man/chttpclient/chttpcli_declare_scoped.3),
[chttpclient_destroy(3)](../man/chttpclient/chttpclient_destroy.3),
[chttp_default_client(3)](../man/chttpclient/chttp_default_client.3)

Settings:
[chttpclient_set_pool_size(3)](../man/chttpclient/chttpclient_set_pool_size.3),
[chttpclient_set_connect_timeout(3)](../man/chttpclient/chttpclient_set_connect_timeout.3),
[chttpclient_set_request_timeout(3)](../man/chttpclient/chttpclient_set_request_timeout.3),
[chttpclient_set_max_response_body_size(3)](../man/chttpclient/chttpclient_set_max_response_body_size.3),
[chttpclient_set_tls(3)](../man/chttpclient/chttpclient_set_tls.3)

Requests:
[chttp_request_new(3)](../man/chttpclient/chttp_request_new.3),
[chttp_request_new_mp(3)](../man/chttpclient/chttp_request_new_mp.3),
[chttp_request_set_header(3)](../man/chttpclient/chttp_request_set_header.3),
[chttp_request_get_header(3)](../man/chttpclient/chttp_request_get_header.3),
[chttp_request_free(3)](../man/chttpclient/chttp_request_free.3)

Run requests: [chttpclient_do(3)](../man/chttpclient/chttpclient_do.3),
[chttpclient_do_streaming(3)](../man/chttpclient/chttpclient_do_streaming.3),
[chttpclient_do_async(3)](../man/chttpclient/chttpclient_do_async.3),
[chttpclient_do_async_streaming(3)](../man/chttpclient/chttpclient_do_async_streaming.3),
[chttpclient_async_result_get(3)](../man/chttpclient/chttpclient_async_result_get.3),
[chttpclient_async_result_free(3)](../man/chttpclient/chttpclient_async_result_free.3),
[chttpclient_do_pooled(3)](../man/chttpclient/chttpclient_do_pooled.3),
[chttpclient_do_pooled_streaming(3)](../man/chttpclient/chttpclient_do_pooled_streaming.3)

Convenience calls on the default client:
[chttp_do(3)](../man/chttpclient/chttp_do.3),
[chttp_get(3)](../man/chttpclient/chttp_get.3),
[chttp_post(3)](../man/chttpclient/chttp_post.3),
[chttp_put(3)](../man/chttpclient/chttp_put.3),
[chttp_delete(3)](../man/chttpclient/chttp_delete.3),
[chttp_patch(3)](../man/chttpclient/chttp_patch.3),
[chttp_run_query(3)](../man/chttpclient/chttp_run_query.3)

Responses:
[chttpclient_resp_header(3)](../man/chttpclient/chttpclient_resp_header.3),
[chttpclient_resp_header_count(3)](../man/chttpclient/chttpclient_resp_header_count.3),
[chttpclient_resp_header_at(3)](../man/chttpclient/chttpclient_resp_header_at.3),
[chttpclient_resp_free(3)](../man/chttpclient/chttpclient_resp_free.3)

The shared engine of Tiers 2 and 3:
[chttpcli_set_engine_logger(3)](../man/chttpclient/chttpcli_set_engine_logger.3),
[chttpcli_set_engine_mem_mgmt_procs(3)](../man/chttpclient/chttpcli_set_engine_mem_mgmt_procs.3),
[chttpcli_set_engine_num_reactor_threads(3)](../man/chttpclient/chttpcli_set_engine_num_reactor_threads.3)

Shared types and helpers (`chttp.h`):
[CHTTP_STATUS(3)](../man/chttp/CHTTP_STATUS.3),
[CHTTP_TLS_DEFAULT(3)](../man/chttp/CHTTP_TLS_DEFAULT.3),
[CHTTP_BODY(3)](../man/chttp/CHTTP_BODY.3),
[CHTTP_JSON_BODY(3)](../man/chttp/CHTTP_JSON_BODY.3),
[CHTTP_TEXT_BODY(3)](../man/chttp/CHTTP_TEXT_BODY.3),
[CHTTP_FORM_BODY(3)](../man/chttp/CHTTP_FORM_BODY.3),
[CHTTP_NO_BODY(3)](../man/chttp/CHTTP_NO_BODY.3),
[chttp_method_str(3)](../man/chttp/chttp_method_str.3),
[chttp_base64_encode(3)](../man/chttp/chttp_base64_encode.3),
[chttp_base64_encode_mp(3)](../man/chttp/chttp_base64_encode_mp.3),
[chttp_base64_decode(3)](../man/chttp/chttp_base64_decode.3),
[chttp_base64_decode_mp(3)](../man/chttp/chttp_base64_decode_mp.3),
[chttp_basic_auth(3)](../man/chttp/chttp_basic_auth.3),
[chttp_basic_auth_mp(3)](../man/chttp/chttp_basic_auth_mp.3)

Related guides: [HTTP server](chttpserver.md),
[Concurrency and fork()](concurrency.md), [Memory management](memory.md),
[Building](building.md), [JSON](cjson.md), [Design](design.md).
