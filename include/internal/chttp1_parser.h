/*
MIT License

Copyright (c) 2026 - A bunch of nerds

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once

#include <common.h> /* ccol_memmgmt_procs_t, for chttp1_stream_t.mp */
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h> /* C23 has bool, true and false as keywords */
#endif
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* ssize_t, for chttp1_stream_read()/_write() */

/**
 * @file chttp1_parser.h
 * @brief INTERNAL ONLY. A small HTTP/1.1 parser for requests and responses,
 *        written by hand. chttpclient.c uses it in response mode, and
 *        chttpserver.c uses it in request mode.
 *
 * This is not a public collections module: it has no macros and no opaque
 * handle type. No public header includes it (chttp.h, chttpclient.h and
 * chttpserver.h do not), and only src/chttpclient.c and src/chttpserver.c are
 * meant to #include this header.
 *
 * One parser covers both grammars. Response mode parses exactly the subset that
 * chttpclient.c needs, while request mode covers what chttpserver.c needs and
 * adds chttp1_parser_init_request(), settings->on_request_line,
 * CHTTP1_HEADERS_DIVERT_BODY, CHTTP1_HEADERS_ONLY and the request-specific rule
 * for the body framing from RFC 7230 SS3.3. Both modes share, instead of
 * duplicating, the core that handles the headers, the chunks, the trailers and
 * the size caps. The internal state machine of chttp1_parser_t makes this
 * natural, because both grammars share every state after the first line.
 *
 * chttp1_stream_t and its prepare, read, write, last_error and release
 * functions are a separate, small I/O helper with which a worker thread pulls
 * bytes without depending on a reactor. It makes real read(2), write(2) and
 * poll(2) system calls and reads and writes raw bytes on a file descriptor; see
 * its own section below. That helper depends on more than the C standard
 * library, because it needs poll(2) and the raw socket read and write.
 * chttp1_parser_t itself is different: the parser proper is pure computation,
 * with no I/O, and it allocates only when a response parser spills a long line;
 * see chttp1_parser_enable_line_spill().
 *
 * ### Design: a plain, self-contained value type
 *
 * A request parser, and a response parser that did not call
 * chttp1_parser_enable_line_spill(), own no resources, so it is safe to embed
 * one by value on the stack or as a struct member, and such a parser needs no
 * release. This works because the internal buffer that accumulates one line,
 * which the parser uses for the first line, the header lines and the chunk-size
 * lines, is a FIXED-size array of CHTTP1_MAX_LINE_LEN bytes (defined below)
 * that the struct holds directly. The fixed size is also what enforces the cap
 * on the length of a line: the parser rejects a line that does not terminate
 * before the buffer fills up and reports "line too long". The parser never
 * copies the bytes of a body or of chunk data into this buffer; it passes those
 * bytes straight from the read buffer of the caller to on_body, with no copy.
 *
 * A response parser can opt in to longer header and trailer lines with
 * chttp1_parser_enable_line_spill(). A line that fills the fixed buffer then
 * moves to one heap buffer of CHTTP1_MAX_SPILL_LINE_LEN bytes, allocated
 * through the memory procs of the caller, and the total header byte cap
 * bounds that line as well. Such a parser owns that buffer once it spills, and
 * the caller must call chttp1_parser_release() before it discards the parser
 * or initializes it again. Request mode never spills and never allocates.
 *
 * ### Header size protections
 *
 * A malicious or bad peer can send one endless header line, one byte at a time.
 * Three limits hold such a peer back: a limit on the length of one header line,
 * a limit on the number of headers, and a limit on their total size. Without
 * them, such a peer can force the memory to grow without a bound. This parser
 * applies three fixed limits that together bound this: CHTTP1_MAX_LINE_LEN,
 * which the definition below gives, and CHTTP1_MAX_HEADER_COUNT and
 * CHTTP1_MAX_TOTAL_HEADER_BYTES, which are private to chttp1_parser.c because
 * they do not change the layout of this struct. A chunked body ends with a
 * chunk of length 0, and the trailer headers that come after it count against
 * the same budget for the header count and for the total bytes as the ordinary
 * headers do. There is no separate allowance for trailers, so nothing can get
 * past the cap through them.
 *
 * ### Transfer-Encoding framing
 *
 * Repeated Transfer-Encoding header lines are one list of transfer codings that
 * commas separate, in order (RFC 7230 SS3.2.2), so every rule below applies to
 * that MERGED list, and not to one line. An empty list element names no coding
 * and is ignored (RFC 7230 SS7), whether it comes from a trailing comma or from
 * a whole line with no coding in it, so "chunked" followed by an empty
 * Transfer-Encoding line is the list "chunked".
 *
 * Both modes reject two things with CHTTP1_ERROR:
 *  - "chunked" that appears more than one time in the merged list, in every
 *    spelling and across every line. RFC 7230 SS3.3.1 forbids more than one
 *    application of chunked to one message body, and a recipient that de-chunks
 *    one time and a recipient that de-chunks two times also disagree about the
 *    end of the body.
 *  - A Content-Length beside any Transfer-Encoding, a pair that RFC 7230
 *    SS3.3.3 calls ambiguous.
 *
 * The two modes then differ, deliberately, over a merged list whose FINAL
 * coding is not "chunked". The parser rejects a REQUEST, because a request has
 * no EOF-delimited framing to fall back on, so its length is indeterminate. The
 * parser accepts a RESPONSE and reads it until the connection closes (RFC 7230
 * SS3.3.3), which is the framing that the specification defines for it. Such a
 * message is never eligible for keep-alive.
 *
 * A chunk-size line follows the RFC 9112 SS7.1 grammar in request mode:
 * whitespace after the size is accepted only before a ";" extension. In
 * response mode the parser also accepts whitespace between the size and the
 * CRLF with no extension after it ("5 \r\n"), as the Go net/http and curl
 * clients do.
 *
 * ### Thread safety
 *
 * A chttp1_parser_t instance is not thread-safe, because nothing here is meant
 * to be shared between threads. This matches the way chttpclient.c uses it:
 * there is one instance for each hop that is in flight, and one thread drives
 * that hop and touches the instance (in the async tier, one reactor callback
 * invocation touches it instead). This stays true across a CHTTP1_HEADERS_ONLY
 * divert in request mode: exactly one thread touches the parser at any moment,
 * and WHICH thread that is can change one time at the divert point, from a
 * reactor thread to a worker thread. There is no concurrent access, only a
 * handoff between two single owners, which the caller must sequence correctly;
 * see the doc comment of CHTTP1_HEADERS_ONLY. chttp1_stream_t instances obey
 * the same rule: the caller hands one instance to exactly one worker thread at
 * a time, and never shares it.
 */

/* ========================================================================== */
/*                         RESULT CODES                                      */
/* ========================================================================== */

/**
 * @brief The outcome of chttp1_parser_execute() or chttp1_parser_finish().
 *
 * These codes mirror the exact three-way split that chttpclient.c already
 * branches on. CHTTP1_OK means "keep reading, the message is not complete yet".
 * CHTTP1_PAUSED means that the message completed; see the hard contract on
 * chttp1_parser_execute below. CHTTP1_USER means that an application callback
 * (on_header, on_trailer, on_request_line, on_headers_complete, on_body or
 * on_message_complete) reported an error itself. CHTTP1_ERROR covers every
 * failure of the parse, such as a malformed status line, an invalid header, an
 * overflow of a chunk size, or an oversized header. This parser does not divide
 * CHTTP1_ERROR into one code for each cause, because chttpclient.c never needs
 * to tell one parse failure from another beyond "not OK, PAUSED or USER". A
 * reason string that a person can read is still available for the exact
 * rejection, in chttp1_parser_t.reason, for diagnostics and for tests.
 */
typedef enum {
  CHTTP1_OK = 0,
  CHTTP1_PAUSED,
  CHTTP1_USER,
  CHTTP1_ERROR,
  /**
   * This code appears in request mode only; see chttp1_parser_init_request().
   * The parser parsed and validated the headers in full, and
   * settings->on_headers_complete returned CHTTP1_HEADERS_DIVERT_BODY (see the
   * doc comment of that callback). That return value pauses the parse here,
   * before the parser consumes one byte of the body, so the parser does not
   * continue into the content of the body inside this same
   * chttp1_parser_execute() call. chttp1_parser_consumed() reports exactly how
   * many bytes of the buffer of THIS call were the header block; the parser
   * consumes nothing from that point to len. Those remaining bytes are probably
   * the start of the body, or the bytes of a pipelined message. They belong to
   * the caller, which hands them off in the way that it wants, for example as
   * the leftover argument of chttp1_stream_prepare() when it diverts the read
   * of the body to a worker thread.
   *
   * Unlike CHTTP1_PAUSED, which is a terminal outcome, this code is NOT one:
   * the parser is still in the middle of the message. parser->state already
   * sits at the state of the body framing that applies, which is content-length
   * or chunked. (If the request has no body at all, that state is instead
   * already complete, and the parser never returns this code; see the doc
   * comment of the callback.) More bytes through chttp1_parser_execute(), from
   * the thread that now owns the connection, continue exactly where this call
   * stopped, which is the expected use. More bytes after CHTTP1_PAUSED are a
   * violation of the contract, but more bytes here are not.
   */
  CHTTP1_HEADERS_ONLY
} chttp1_errno_t;

/**
 * @brief The grammar that chttp1_parser_t uses for its first line.
 *        chttp1_parser_init() sets CHTTP1_PARSE_RESPONSE, and
 *        chttp1_parser_init_request() sets CHTTP1_PARSE_REQUEST. The value
 *        is read-only after that.
 */
typedef enum {
  CHTTP1_PARSE_RESPONSE = 0,
  CHTTP1_PARSE_REQUEST
} chttp1_parser_type_t;

/**
 * @brief The constants that settings->on_headers_complete returns.
 *
 * CHTTP1_HEADERS_HAS_BODY and CHTTP1_HEADERS_NO_BODY apply in both modes of the
 * parse. Their values are 0 and 1, so a callback that returns a bare literal 0
 * or 1 behaves exactly like one that returns the named constant.
 * CHTTP1_HEADERS_DIVERT_BODY is valid ONLY in request mode; see the doc comment
 * of chttp1_settings_t.on_headers_complete for its full contract. A callback
 * that returns it in response mode gives an invalid hint, and the parser
 * answers with CHTTP1_USER, because chttpclient.c has no concept of a diversion
 * to a worker thread to hand the connection to.
 */
#define CHTTP1_HEADERS_HAS_BODY 0
#define CHTTP1_HEADERS_NO_BODY 1
#define CHTTP1_HEADERS_DIVERT_BODY 2

/* ========================================================================== */
/*                         INTERNAL STATE (opaque to chttpclient.c)          */
/* ========================================================================== */

/* This enum is not part of the contract that this module gives chttpclient.c.
 * It is declared here for one reason only: chttp1_parser_t must be a complete
 * type that a caller can embed on the stack. */
typedef enum {
  CHTTP1_ST_FIRST_LINE = 0, /* the status line in response mode, or the
                             * request line in request mode. parser->type
                             * decides which grammar applies. Two separate
                             * states are not needed, because both modes
                             * fully share every state after this one. */
  CHTTP1_ST_HEADERS,
  CHTTP1_ST_BODY_CONTENT_LENGTH,
  CHTTP1_ST_BODY_CHUNK_SIZE,
  CHTTP1_ST_BODY_CHUNK_DATA,
  CHTTP1_ST_BODY_CHUNK_CRLF,
  CHTTP1_ST_BODY_CHUNK_TRAILERS,
  CHTTP1_ST_BODY_EOF,
  CHTTP1_ST_MESSAGE_DONE,
  CHTTP1_ST_DEAD /* a hard parse error happened. execute() does nothing */
} chttp1__state_t;

/* The finish state has three values: a clean boundary, a completion that an EOF
 * delimits, and a message that an EOF cut in the middle. This three-way split
 * is what makes chttp1_parser_finish() correct without the call site needing to
 * know which mode of the body framing was in force. */
typedef enum {
  CHTTP1_FINISH_SAFE = 0,     /* a clean message boundary. Nothing pends */
  CHTTP1_FINISH_SAFE_WITH_CB, /* a body that an EOF delimits. The EOF itself
                                 completes the message */
  CHTTP1_FINISH_UNSAFE        /* an EOF in the middle of a message. The
                                 message is truncated and invalid */
} chttp1__finish_state_t;

/**
 * @brief The fixed size of the internal buffer that accumulates one line. That
 *        buffer is chttp1_parser_t._line_buf below, and it holds the status
 *        line, a header line or a chunk-size line.
 *
 * This one constant has two jobs: it is the size of the buffer, and it is also
 * the threshold that rejects a line as too long, except for the header and
 * trailer lines of a response parser that spills them to the heap (see
 * chttp1_parser_enable_line_spill() and the doc comment at the top of this
 * file). The parser rejects a line that does not terminate before it
 * accumulates this many bytes, so there is no separate idea of a "cap" to keep
 * equal to the real size of the buffer. The constant must live here, and not in
 * chttp1_parser.c, because it decides the layout of chttp1_parser_t, which must
 * be a complete type for chttpclient.c to embed it by value. The value 8192
 * matches the size of the read buffer that chttpclient.c holds as a literal at
 * both of its socket-read call sites.
 *
 * The other two limits, the maximum header count and the maximum cumulative
 * header bytes, do not change the layout of this struct: they are plain running
 * counts, which the parser compares against constants that are private to
 * chttp1_parser.c.
 */
#define CHTTP1_MAX_LINE_LEN 8192

/**
 * @brief The capacity of the heap buffer that holds a header or trailer
 *        line of a response parser that outgrew CHTTP1_MAX_LINE_LEN; see
 *        chttp1_parser_enable_line_spill(). The total header byte cap bounds
 *        the line as well.
 */
#define CHTTP1_MAX_SPILL_LINE_LEN (64 * 1024)

typedef struct chttp1_parser chttp1_parser_t;

/**
 * @brief The callback settings: on_header, on_trailer, on_request_line,
 *        on_headers_complete, on_body and on_message_complete. on_request_line
 *        applies in request mode only. The parser always assembles a whole
 *        header line internally before it fires on_header, so the caller needs
 *        no protocol of its own to reassemble fragments.
 *
 * The parser checks every callback for NULL before it calls that callback, so a
 * caller wires only the callbacks that it needs. chttpclient.c and
 * chttpserver.c both leave on_trailer NULL, because neither of them gives a
 * trailer API of its own.
 */
typedef struct {
  /**
   * The parser fires this callback one time for each complete header line of
   * the header block of the message, which holds the lines between the first
   * line and the blank line that ends the block. The trailer fields of a
   * chunked body NEVER reach this callback; the parser delivers them to
   * on_trailer instead. A caller can therefore keep one single collection of
   * headers, and a trailer field with the name of a real header can never
   * silently replace that header.
   *
   * name and value point into the internal line buffer of this parser and are
   * valid ONLY for the duration of this call, so a callback that must keep them
   * copies them before it returns. name is exactly as it arrived on the wire,
   * because this parser does not change it to lower case. The parser has
   * already trimmed the optional white space (SP and HTAB) from the start and
   * the end of value.
   *
   * Return 0 after a success. Return a value that is not zero to stop the parse
   * with CHTTP1_USER. The caller of chttp1_parser_execute sees that value only
   * indirectly, through CHTTP1_USER; nothing else gives the raw return value to
   * that caller.
   */
  int (*on_header)(chttp1_parser_t *p, const char *name, size_t name_len,
                   const char *value, size_t value_len);

  /**
   * The parser fires this callback one time for each complete trailer field
   * line (RFC 7230 SS4.1.2). A chunked body ends with a chunk of length 0, and
   * those lines come after that chunk and before the final blank line. The
   * parser never fires this callback for the header block of a message, and
   * never fires it at all for a message with no chunked body.
   *
   * name and value obey the same contract as in on_header for the lifetime, the
   * spelling and the trim of the optional white space. The parser validates a
   * trailer line with exactly the same grammar (the name is a token, and the
   * value holds no control characters) and charges the line against exactly the
   * same budget for the header count and for the total header bytes as a header
   * line. There is no separate allowance for trailers, so nothing can get past
   * the caps through them.
   *
   * A trailer field never changes the view that this parser has of the message:
   * the parser interprets Content-Length, Transfer-Encoding, Connection and
   * Expect inside the header block only. A trailer can therefore not change the
   * framing that is already in force, nor what chttp1_should_keep_alive(),
   * chttp1_has_content_length(), chttp1_declared_content_length() and
   * chttp1_expects_continue() report. A trailer field that carries one of those
   * names arrives here as an ordinary, inert trailer.
   *
   * chttp1_settings_init() leaves this callback NULL. With it NULL, the parser
   * validates every trailer field and then discards it, which is what a caller
   * with no trailer API of its own wants: a trailer then has no way at all to
   * reach the headers collection of that caller.
   *
   * Return 0 after a success. Return a value that is not zero to stop the parse
   * with CHTTP1_USER.
   */
  int (*on_trailer)(chttp1_parser_t *p, const char *name, size_t name_len,
                    const char *value, size_t value_len);

  /**
   * This callback applies in request mode only, which
   * chttp1_parser_init_request() selects; the parser never fires it in response
   * mode. It fires the callback one time, after it parses and validates the
   * request line in full and before the first header line, when p->http_major
   * and p->http_minor already hold their values.
   *
   * method and target point into the internal line buffer of this parser and
   * are valid ONLY for the duration of this call, exactly like name and value
   * in on_header, so a callback that must keep them copies them before it
   * returns. The parser validates neither of them beyond the basic grammar:
   * method is a token of tchar bytes only, and target is a run of one or more
   * bytes that holds no control character, no DEL and no white space at all
   * (white space covers both SP and HTAB here; see RFC 7230 SS3.1.1 and SS5.3).
   * This parser does not tell an origin-form target apart from an
   * absolute-form, an authority-form or an asterisk-form target, and it does
   * not reject a method name that it does not know. Both of those belong to the
   * routing of the caller: an unsupported method, for example, becomes an
   * ordinary 405, and not a parse error.
   *
   * Return 0 after a success. Return a value that is not zero to stop the parse
   * with CHTTP1_USER.
   */
  int (*on_request_line)(chttp1_parser_t *p, const char *method,
                         size_t method_len, const char *target,
                         size_t target_len);

  /**
   * The parser fires this callback one time, after the blank line that ends the
   * header block. In response mode, p->status_code already holds its value.
   *
   * Return CHTTP1_HEADERS_HAS_BODY (0) if this message has a body, which is the
   * normal case. Return CHTTP1_HEADERS_NO_BODY (1) if the caller already knows
   * from another source that no body follows, for example for a request whose
   * method was HEAD; this answer holds even when a Content-Length header is
   * present. Return CHTTP1_HEADERS_DIVERT_BODY (2) to pause the parse here
   * instead of continuing into the content of the body inside this same
   * chttp1_parser_execute() call. That value applies in request mode only; see
   * the CHTTP1_HEADERS_ONLY doc comment of that function for the full contract.
   * With this value a caller can route the request from the headers alone,
   * before it decides whether to read the body and how to read it, for example
   * to hand off the read of the body to a worker thread. Return any OTHER
   * value, a negative value included, to stop the parse with CHTTP1_USER.
   *
   * This split is deliberate. A real on_headers_complete implementation needs a
   * genuine path to report an error here; one example on the response side is a
   * failure to allocate a copy of a Location header during the detection of a
   * redirect. The parser therefore always treats a value outside the accepted
   * set (CHTTP1_HEADERS_HAS_BODY, CHTTP1_HEADERS_NO_BODY, and
   * CHTTP1_HEADERS_DIVERT_BODY in request mode) as a reported error, and never
   * turns such a value silently into "no body".
   *
   * The parser downgrades CHTTP1_HEADERS_DIVERT_BODY to the ordinary has-body
   * behavior whenever there is nothing meaningful to divert: it then treats the
   * value as CHTTP1_HEADERS_HAS_BODY and continues to parse the content of the
   * body in this same call. Two requests get this downgrade. A request with
   * neither a Content-Length nor a chunked Transfer-Encoding has no body at
   * all, whatever this callback returns (see the doc comment of
   * CHTTP1_HEADERS_ONLY), and a request with an explicit "Content-Length: 0"
   * completes immediately for the same reason; a pause that diverts an empty
   * body to a worker thread has no value. The parser always truly diverts a
   * chunked body, even one whose first chunk is the empty chunk that terminates
   * the body, because it can find that out only from the first chunk-size line,
   * which it does not read before this callback runs.
   */
  int (*on_headers_complete)(chttp1_parser_t *p);

  /**
   * The parser fires this callback zero times or more, with the bytes of the
   * body as it parses them out of the framing, which a Content-Length delimits
   * or which is chunked. at points directly into the buffer that the caller
   * passed to chttp1_parser_execute, NOT into the line buffer of this parser,
   * because the parser never copies the bytes of a body internally. at is valid
   * only for the duration of this call.
   *
   * Return 0 after a success. Return a value that is not zero to stop the parse
   * with CHTTP1_USER.
   */
  int (*on_body)(chttp1_parser_t *p, const char *at, size_t len);

  /**
   * The parser fires this callback exactly one time, when it has parsed the
   * whole message: the headers plus the body framing that applies. This parser
   * knows on its own that it reached a message boundary, because it drives its
   * own explicit state machine instead of learning that from the return value
   * of a callback, so this callback follows the same plain convention as the
   * other three. Return 0 after a success. Return a value that is not zero to
   * stop the parse with CHTTP1_USER. chttp1_parser_execute and
   * chttp1_parser_finish report the pause itself with their own CHTTP1_PAUSED
   * return value, whatever this callback returns beyond the difference between
   * zero and not zero.
   */
  int (*on_message_complete)(chttp1_parser_t *p);
} chttp1_settings_t;

/**
 * @brief The parser itself. It is self-contained, and it owns a resource
 *        only after a response parser with line spilling turned on spilled a
 *        line; see chttp1_parser_enable_line_spill() and
 *        chttp1_parser_release(). It is safe to embed it by value on the
 *        stack, and as a struct member.
 */
struct chttp1_parser {
  /** This field is opaque to the parser. The caller sets it one time, after
   * chttp1_parser_init, and reads it back in every callback to get its own
   * context. */
  void *data;

  /** The parser sets this field one time, in on_headers_complete, from the
   * status line that it parsed. It applies in response mode only, and it is
   * always 0 in request mode. */
  int status_code;

  /** chttp1_parser_init() and chttp1_parser_init_request() set this field one
   * time, and it is read-only after that. It decides whether
   * CHTTP1_ST_FIRST_LINE parses a status line or a request line, and it also
   * gates three behaviors that belong to request mode alone: the fire of
   * on_request_line, the acceptance of CHTTP1_HEADERS_DIVERT_BODY from
   * on_headers_complete, and the rule of RFC 7230 SS3.3 that says "no framing
   * means no body, and not an EOF-delimited body". */
  chttp1_parser_type_t type;

  /** A field for diagnostics only. It holds a static string that describes the
   * last rejection, for example "Duplicate Content-Length", or NULL. The error
   * reporting of chttpclient.c is built on ccol_retval_t and does not read this
   * field, because every CHTTP1_ERROR becomes the same
   * ccol_http_transfer_aborted, whatever the reason. The field is here for
   * logs, and for the test suite of this module, which asserts on the exact
   * reason of a rejection. */
  const char *reason;

  /* Everything below is internal state, which chttpclient.c neither reads nor
   * writes directly. */
  chttp1__state_t state;
  chttp1__finish_state_t finish_state;
  uint8_t http_major;
  uint8_t http_minor;
  uint16_t flags;
  uint64_t content_length; /* the bytes that remain in
                            * CHTTP1_ST_BODY_CONTENT_LENGTH, or the bytes
                            * that remain in the CURRENT chunk in
                            * CHTTP1_ST_BODY_CHUNK_DATA. One field serves
                            * both states. */
  uint64_t chunk_excess;   /* the bytes of chunk framing beyond what the data
                            * of the chunks so far earns; see
                            * parse_chunk_size_line() in chttp1_parser.c */

  size_t header_count;
  size_t total_header_bytes;
  /* Request mode only. This field counts the blank lines that the parser
   * accepts before the request line itself (RFC 7230 SS3.5).
   * CHTTP1_MAX_LEADING_BLANK_LINES, a constant that is private to
   * chttp1_parser.c, bounds the count. */
  size_t leading_blank_lines;

  /* Request mode only. This field counts the Host header lines of the
   * header block of the current request. RFC 7230 SS5.4 makes the count
   * itself a validity rule of the message, and not a concern of routing: a
   * server must answer 400 for any request that carries more than one Host
   * line, and for an HTTP/1.1 request that carries none. Two Host lines let
   * a front end and a back end disagree about which authority the request
   * names, which is what request smuggling needs. The count saturates, since
   * no reader asks for more than "0, 1 or many". The parser applies the rule
   * at the blank line that ends the header block, because only there is the
   * count final. Response mode never touches this field. */
  uint8_t host_count;

  /* Response mode only. True once chttp1_parser_enable_line_spill() allowed
   * a header or trailer line to outgrow line_buf. line_spill_mp are the
   * memory procs of that heap buffer, and line_spill is the buffer itself
   * once a line needed it, or NULL. */
  bool line_spill_enabled;

  /** An optional override, for this one parser, of the built-in caps
   * CHTTP1_MAX_HEADER_COUNT and CHTTP1_MAX_TOTAL_HEADER_BYTES, which are both
   * private to chttp1_parser.c. A value of 0 means "use the built-in default",
   * and chttp1_parser_init() and chttp1_parser_init_request() leave both fields
   * at 0. Set either field to a value above 0, at any time before you feed the
   * first byte, to override that cap for this parser instance. These fields
   * exist for the configurable chttpsvr_config_t.max_header_bytes of
   * chttpserver. chttpclient has no public control of this kind and never sets
   * these fields, so it gets the built-in caps.
   */
  size_t max_header_count_override;
  size_t max_total_header_bytes_override;

  /** An optional cap, for this one parser, on the declared size of one chunk.
   * It applies to bodies with a chunked Transfer-Encoding only and has no
   * effect on a body that a Content-Length frames. The default is 0, which
   * means no cap, and the parser then accepts a chunk-size line for every value
   * that fits in a uint64_t. With a value above 0, the parser rejects a
   * chunk-size line whose decoded value is above the cap with CHTTP1_ERROR as
   * soon as it parses the line (see chttp1_chunk_size_limit_exceeded()), before
   * it waits for one byte of the data of that chunk. This field exists for the
   * max_body_size of chttpserver. Without the cap, a peer can declare one
   * absurdly large chunk (every value up to UINT64_MAX is a chunk-size token
   * that the syntax allows) and then never send it, and such a peer holds a
   * worker thread until a read timeout fires, because the parser checks
   * max_body_size itself only against the bytes that arrive. See the comment of
   * chttp1_declared_content_length for the same gap in a body that a
   * Content-Length frames. chttpclient.c has no public control of this kind and
   * never sets this field. */
  uint64_t max_chunk_size_override;

  /* A fixed-size buffer that accumulates the status line, each header line,
   * each trailer line and each chunk-size line. The parser never uses it for
   * the bytes of a body or of chunk data, which go straight from the buffer of
   * the caller to on_body. A partial line that spans two chttp1_parser_execute
   * calls accumulates here. See the comment of CHTTP1_MAX_LINE_LEN for the
   * reason that this buffer has a fixed size and does not grow. A response
   * parser that spilled a line uses line_spill instead, for that line and every
   * later one of the message. */
  char line_buf[CHTTP1_MAX_LINE_LEN];
  size_t line_len;
  char *line_spill;
  ccol_memmgmt_procs_t *line_spill_mp;

  size_t consumed; /* the value behind chttp1_parser_consumed(). It has a
                    * meaning only immediately after chttp1_parser_execute
                    * returns CHTTP1_PAUSED. See the doc comment of that
                    * function. */

  const chttp1_settings_t *settings;
};

/* ========================================================================== */
/*                         GRAMMAR HELPERS                                    */
/* ========================================================================== */

/**
 * @brief The tchar rule of RFC 7230 SS3.2.6. It gives true if the NAME of a
 *        header field can legally hold the byte c. The same rule applies to the
 *        token of an HTTP method.
 *
 * This header exposes the function for chttp_request_set_header() of
 * chttpclient.c and for chttpsvr_resp_set_header() of chttpserver.c, which both
 * validate a header NAME from the caller as a real token before they accept it.
 * This parser applies the same check to the bytes that arrive on the wire, to
 * the method of a request line and to a header name.
 */
bool chttp1_is_tchar(unsigned char c);

/* ========================================================================== */
/*                         LIFECYCLE                                          */
/* ========================================================================== */

/** @brief Sets every byte of a chttp1_settings_t to zero, which leaves every
 *         callback NULL. */
void chttp1_settings_init(chttp1_settings_t *settings);

/**
 * @brief Initializes parser for a new response message. The function returns
 *        nothing and cannot fail, because it allocates nothing.
 *
 * settings must live longer than parser.
 *
 * There is no chttp1_parser_reset(): chttpclient.c never uses one parser
 * instance for two hops, because every hop builds a new instance, in the sync
 * tier and in the async tier, so this parser has no contract for reuse in place
 * either. Build a new instance for each message, or run chttp1_parser_init on
 * the instance again. The init overwrites every field, so a parser that enabled
 * line spilling must go through chttp1_parser_release() first, or its heap line
 * buffer leaks.
 */
void chttp1_parser_init(chttp1_parser_t *parser,
                        const chttp1_settings_t *settings);

/**
 * @brief Initializes parser for a new REQUEST message. parser->type becomes
 *        CHTTP1_PARSE_REQUEST. The function returns nothing and cannot fail,
 *        and its contract is otherwise the same as the contract of
 *        chttp1_parser_init().
 *
 * The parser then parses "method SP request-target SP HTTP-version CRLF"
 * instead of a status line and fires settings->on_request_line one time, before
 * the first header. It applies the rule for the body framing of a request from
 * RFC 7230 SS3.3: a message with neither a Content-Length nor a chunked coding
 * has no body at all, and never gets the EOF-delimited framing that belongs to
 * a response alone. It also accepts CHTTP1_HEADERS_DIVERT_BODY from
 * on_headers_complete.
 */
void chttp1_parser_init_request(chttp1_parser_t *parser,
                                const chttp1_settings_t *settings);

/**
 * @brief Lets a RESPONSE parser accept a header or trailer line longer than
 *        CHTTP1_MAX_LINE_LEN, up to CHTTP1_MAX_SPILL_LINE_LEN bytes and
 *        within the total header byte cap.
 *
 * Call it after chttp1_parser_init() and before the first byte. A line that
 * fills the fixed buffer moves to one heap buffer, which the parser
 * allocates through mp (NULL selects the default allocator) and keeps for
 * the rest of the message. The status line and the chunk-size lines stay
 * bounded by CHTTP1_MAX_LINE_LEN until a header line spills. A failed
 * allocation reports the line as too long.
 *
 * Once this call returned true, the parser may own that buffer, and the
 * caller must call chttp1_parser_release() on every path that discards the
 * parser or initializes it again, whatever the outcome of the parse.
 *
 * @return true when spilling is now allowed; false for a request parser,
 *         which never allocates, and for a NULL parser.
 */
bool chttp1_parser_enable_line_spill(chttp1_parser_t *parser,
                                     ccol_memmgmt_procs_t *mp);

/**
 * @brief Frees the heap line buffer of a parser that spilled a line, if it
 *        has one. It is safe on any initialized parser, and safe to call
 *        more than once. The parser must not be fed after this call; to
 *        reuse the struct, initialize it again.
 */
void chttp1_parser_release(chttp1_parser_t *parser);

/* ========================================================================== */
/*                         PARSING                                           */
/* ========================================================================== */

/**
 * @brief Feeds len bytes of data to the parser.
 *
 * Three contracts apply. The first two carry load for the read loop of
 * chttpclient.c, which calls this function one time for each socket read.
 *
 * 1. In the CHTTP1_OK case this function ALWAYS consumes the whole input
 *    buffer. It buffers a partial line internally across calls, and it
 *    continues a partial count of body bytes or of chunk bytes across calls
 *    as it needs to, so it never returns CHTTP1_OK with bytes left over. Only
 *    CHTTP1_PAUSED leaves a well-defined "consumed" position below len; see
 *    chttp1_parser_consumed().
 *
 * 2. Do not feed more bytes to this parser instance after this function
 *    returns CHTTP1_PAUSED. The bytes from chttp1_parser_consumed() to len,
 *    in the buffer that caused the pause, are not consumed. In response
 *    mode, which is the only way chttpclient.c uses this parser, the caller
 *    discards them: chttpclient.c never feeds this parser instance again
 *    after CHTTP1_PAUSED, and it has no concept of pipelining at all. In
 *    request mode, chttpserver.c DOES treat those bytes as real, meaningful
 *    bytes of a pipelined request instead of discarding them; see
 *    chttp1_stream_push_back_leftover(), which exists to thread them into
 *    the parse of the next request. This contract covers response mode
 *    only.
 *
 * 3. Contract 2 above does NOT cover CHTTP1_HEADERS_ONLY, which appears in
 *    request mode only. That code is not a terminal outcome, so more bytes
 *    after it, through another chttp1_parser_execute call from the thread
 *    that now owns the connection, are expected and are not a violation.
 *    See the doc comment of CHTTP1_HEADERS_ONLY.
 *
 * @return CHTTP1_OK          The parser consumed the whole buffer. The
 *                            message is not complete yet, so call the
 *                            function again with more data.
 *         CHTTP1_PAUSED      The message is complete, and
 *                            on_message_complete fired. See
 *                            chttp1_parser_consumed().
 *         CHTTP1_HEADERS_ONLY Request mode only. See its own doc comment.
 *         CHTTP1_USER        An application callback reported an error.
 *         CHTTP1_ERROR       The input is malformed. See parser->reason.
 */
chttp1_errno_t chttp1_parser_execute(chttp1_parser_t *parser, const char *data,
                                     size_t len);

/**
 * @brief Call this function when the connection below reports EOF, which is a
 *        read() or a recv() that returns 0. The function tells you whether that
 *        EOF is a valid end for the message in progress.
 *
 * Some messages, such as a response in the style of HTTP/1.0 or a response with
 * an explicit Connection: close, have no Content-Length and no chunked
 * Transfer-Encoding. Such a message is complete only when the connection
 * closes, because there is no other signal for it. In that case, and only in
 * that case, this function fires on_message_complete and returns CHTTP1_PAUSED.
 * CHTTP1_PAUSED from THIS function means "yes, the EOF was a valid, clean end
 * to the message", not "keep going"; there is nothing after it to consume, so
 * there is no equivalent of chttp1_parser_consumed() to read after it.
 *
 * In every other state this function returns CHTTP1_OK if the message was
 * already complete, and CHTTP1_ERROR if the EOF arrived in the middle of the
 * message, which makes the response truncated and invalid. There are five other
 * states: before the status line, inside the headers, inside a body that a
 * Content-Length frames, inside a chunk, and at a clean CHTTP1_ST_MESSAGE_DONE
 * boundary with nothing more expected. One outcome remains: the function
 * returns CHTTP1_USER when the on_message_complete that it fires itself returns
 * a value that is not zero.
 *
 * @return CHTTP1_OK      The parser is already at a clean boundary. The EOF
 *                        is fine, and no callback is needed.
 *         CHTTP1_PAUSED  A body that an EOF terminates completed, through
 *                        on_message_complete. See above.
 *         CHTTP1_USER    That on_message_complete call reported an error.
 *                        The parser is left dead, exactly as it is when
 *                        chttp1_parser_execute() reports an error from the
 *                        same callback, so a caller that treats every value
 *                        other than CHTTP1_PAUSED as "this EOF did not
 *                        complete a message" needs no separate arm for it.
 *         CHTTP1_ERROR   The EOF arrived in the middle of the message, which
 *                        is truncated and invalid.
 */
chttp1_errno_t chttp1_parser_finish(chttp1_parser_t *parser);

/**
 * @brief Returns the number of bytes that the message that completed took from
 *        the buffer of the last chttp1_parser_execute call.
 *
 * This count has a meaning only immediately after chttp1_parser_execute returns
 * CHTTP1_PAUSED. The function reports a plain count of bytes instead of a raw
 * pointer into the buffer of the caller, so the caller never subtracts one
 * pointer from another itself. The bytes from this count to the end of that
 * same buffer are trailing data, which the parser did not consume and the
 * caller never asked for. See contract 2 of chttp1_parser_execute.
 */
size_t chttp1_parser_consumed(const chttp1_parser_t *parser);

/**
 * @brief Reports whether parser reached a clean message boundary with the whole
 *        message parsed. chttp1_parser_execute() and chttp1_parser_finish()
 *        leave the parser in that state after they return CHTTP1_PAUSED.
 *
 * This is a thin, stable accessor over parser->state. A caller that drives one
 * request across a CHTTP1_HEADERS_ONLY divert continues chttp1_parser_execute()
 * from a different thread, maybe several calls later, and this accessor gives
 * that caller a documented way to ask "is this request now fully parsed"
 * without reading parser->state directly. The values of chttp1__state_t are not
 * part of the documented contract of this module, and they can change.
 */
bool chttp1_parser_message_complete(const chttp1_parser_t *parser);

/**
 * @brief Reports whether the caller can use the connection that this message
 *        arrived on for a later request, as RFC 7230 SS6.3 says.
 *
 * HTTP/1.1 keeps the connection alive by default, while HTTP/1.0 and the
 * earlier versions do NOT keep it alive unless a Connection: keep-alive token
 * arrives. A Connection: close token ends the connection for every version,
 * even beside a keep-alive token on the same line or on another Connection
 * line. An HTTP/1.0 message that carries Transfer-Encoding never keeps the
 * connection alive either, because RFC 9112 SS6.1 treats its framing as faulty;
 * request mode refuses such a request outright, and a response is processed and
 * the connection then closed. One more rule covers every version: a body that
 * only the close of the connection can delimit (one with no Content-Length and
 * no chunked Transfer-Encoding) is never eligible for keep-alive, whatever the
 * value of a Connection header, because the end of that body already needed the
 * end of the connection.
 *
 * This report has a meaning only after the message completes, that is, after
 * chttp1_parser_execute returns CHTTP1_PAUSED, or after chttp1_parser_finish
 * returns CHTTP1_PAUSED.
 */
bool chttp1_should_keep_alive(const chttp1_parser_t *parser);

/**
 * @brief Makes chttp1_should_keep_alive() report false for the message that
 *        this parser holds, exactly as a Connection: close token does.
 *
 * A caller that lost bytes it already took off the connection, such as the
 * start of a pipelined next message that it could not keep, calls this so
 * that the connection closes after the current message instead of waiting
 * for data that the peer already sent. The next chttp1_parser_init() clears
 * it with every other flag. It does nothing for a NULL parser.
 */
void chttp1_parser_force_close(chttp1_parser_t *parser);

/**
 * @brief Reports whether the headers that the parser read hold "Expect:
 *        100-continue", the only expectation value that RFC 7231 SS5.1.1
 *        defines. This function does NOT detect any other Expect value, nor
 *        that value beside other tokens; the caller rejects such a value
 *        itself, for example with a 417 Expectation Failed. An Expect value
 *        that the caller does not know needs that explicit handling in any
 *        case, whatever this parser does.
 *
 * The report is false for an HTTP/1.0 message whatever it carries: RFC 9110
 * SS10.1.1 has a server ignore 100-continue from an HTTP/1.0 client, which
 * sends its body without waiting for an interim response.
 *
 * This report has a meaning after settings->on_headers_complete fires, which is
 * when the parser holds the whole set of headers. This parser does no I/O of
 * its own, so it does NOT send an interim "HTTP/1.1 100 Continue\r\n\r\n"
 * response by itself. A caller that wants to honor the expectation writes that
 * line to the raw connection itself, usually inside its own on_headers_complete
 * callback or immediately after it, and before it feeds more body bytes to
 * chttp1_parser_execute. A caller that wants to reject the request writes a
 * final status response instead and never reads the body at all.
 *
 * This function is the detection half of Expect: 100-continue for request mode,
 * which is the server side. A parser instance in response mode carries no
 * counterpart that spans the interim response and the final one: it reports a
 * "100 Continue" as the ordinary complete message with no body that every other
 * 1xx status produces. The client then drives the second message on the same
 * connection through a NEW chttp1_parser_t instance, discarding each interim
 * response as that response completes. This keeps one parser instance bound to
 * exactly one message, which is what lets the same value type serve both modes.
 * A caller that sends Expect: 100-continue therefore sequences the interim
 * response and the final one itself, and chttpclient.c is where that code
 * lives.
 */
bool chttp1_expects_continue(const chttp1_parser_t *parser);

/**
 * @brief Reports whether the merged Transfer-Encoding list of the header
 *        block names a coding other than "chunked".
 *
 * This parser decodes chunked framing and no other coding. For a request, a
 * list such as "gzip, chunked" passes the framing rules, and the body that
 * on_body delivers is still gzip-encoded. A server that does not decode the
 * coding itself answers 501 Not Implemented (RFC 9112 SS6.1). A list that
 * does not end in "chunked" never reaches on_headers_complete in request
 * mode, because its length cannot be determined; the parser refuses it
 * first. The report has a meaning once on_headers_complete fires.
 */
bool chttp1_has_other_transfer_coding(const chttp1_parser_t *parser);

/**
 * @brief Reports whether the headers that the parser read set up the framing of
 *        a Content-Length, as opposed to a chunked Transfer-Encoding or no body
 *        at all.
 *
 * This report has a meaning after settings->on_headers_complete fires, and by
 * construction it excludes chunked framing: this parser rejects a message that
 * declares both a Content-Length and a chunked Transfer-Encoding before the
 * headers can complete (RFC 7230 SS3.3.3), so this report and chunked framing
 * can never both be true for one message. The parser also rejects, in both
 * modes, a Content-Length beside a Transfer-Encoding whose final coding is NOT
 * chunked, so a message that reaches headers-complete with the framing of a
 * Content-Length carries no Transfer-Encoding at all. This function lets a
 * caller inspect the declared size of the body, which
 * chttp1_declared_content_length() gives, before it decides whether to divert
 * the request to a worker thread at all, without reading parser->flags
 * directly, a field that is private to chttp1_parser.c.
 */
bool chttp1_has_content_length(const chttp1_parser_t *parser);

/**
 * @brief The declared value of the Content-Length, after
 *        chttp1_has_content_length() becomes true.
 *
 * This value has a meaning only when chttp1_has_content_length() returns true,
 * because the parser uses the field below it for a second purpose: after the
 * parse of a chunked body starts, that field counts the bytes that remain in
 * the CURRENT chunk. A call for a chunked message therefore gives a value with
 * a different meaning instead of the total that the message declared, and so
 * does a call after the parse of a body that a Content-Length frames starts to
 * consume bytes. The function returns 0 for a NULL parser.
 *
 * One caller of this function is chttpserver.c, which uses it to reject a
 * request whose declared Content-Length is already above its own configured
 * limit for the size of a body immediately at headers-complete time, instead of
 * reacting byte by byte as it reads the body (see the doc comment of
 * chttp1_settings_t.on_body). Without this function, a peer that declares an
 * oversized Content-Length and then never sends the body holds a reader until a
 * read timeout fires, because a limit that counts bytes catches nothing if the
 * bytes never arrive.
 */
uint64_t chttp1_declared_content_length(const chttp1_parser_t *parser);

/**
 * @brief Reports whether the last CHTTP1_ERROR was a chunk-size line above
 *        max_chunk_size_override.
 *
 * This report has a meaning only immediately after chttp1_parser_execute() or
 * chttp1_parser_finish() returns CHTTP1_ERROR, and it is false in every other
 * case, as well as for every other reason of a rejection that CHTTP1_ERROR
 * covers, such as a malformed chunk-size token or a line that is too long. For
 * a caller that sets max_chunk_size_override, this function tells "this one
 * chunk declared more than the configured limit" apart from every other failure
 * of the parse, so the caller can map that case to the same "body too large"
 * outcome that a violation of max_body_size already produces (for
 * chttpserver.c, a 413 and ccol_msg_too_large) instead of reporting a generic
 * aborted transfer.
 */
bool chttp1_chunk_size_limit_exceeded(const chttp1_parser_t *parser);

/**
 * @brief Gives the number of body bytes that the framing of the current
 *        message still guarantees to come, or 0 when the framing guarantees
 *        none right now.
 *
 * For a body that a Content-Length frames, the answer is the part of the
 * declared length that the parser has not consumed yet. For a chunked body,
 * the answer is what the current chunk still needs while the parser is
 * inside the data of that chunk, and 0 on a chunk-size line, on the CRLF
 * after a chunk and in the trailers, where the next byte can be the last
 * one. It is 0 in every other state.
 *
 * A caller that waits for readiness can ask the kernel to wake it only once
 * this many bytes are queued (SO_RCVLOWAT), because a peer that follows its
 * own framing sends at least this many before it can expect an answer.
 */
uint64_t chttp1_body_bytes_still_expected(const chttp1_parser_t *parser);

/**
 * @brief Reports whether the peer can still have bytes of the current message
 *        to send that the parser has not consumed.
 *
 * It is false once the message is complete, and false for a message whose
 * header block ended with no body framing (no Content-Length above 0 and no
 * chunked coding), even when the caller refused it at on_headers_complete.
 * It is true for a message whose body is unfinished, whether the parse is
 * paused, diverted or ended by an error inside the body, and true before the
 * end of the header block or after an error inside it, where the framing is
 * unknown. It returns false for a NULL parser.
 *
 * A server that closes a connection while the peer still sends makes the
 * kernel answer with a reset, which can destroy a response that the peer
 * has not read yet. chttpserver.c asks this question before such a close,
 * and closes with a lingering close when the answer is true.
 */
bool chttp1_request_bytes_may_remain(const chttp1_parser_t *parser);

/* ========================================================================== */
/*                    WORKER-PULL BODY/RESPONSE STREAMING                     */
/* ========================================================================== */

/**
 * @brief A small helper that does not depend on a reactor, with which a worker
 *        thread reads raw bytes off a socket and writes them. Each call has an
 *        absolute timeout. The helper also supports "carry-over" bytes, which
 *        are bytes that a reactor thread already read before it handed the
 *        connection off.
 *
 * chttpserver.c uses this helper internally, where a worker thread drives the
 * read of the body. The helper works on a raw file descriptor and depends on no
 * reactor and no ccol_event_loop at all. Unlike a request parser, which
 * allocates nothing, chttp1_stream_t can allocate a heap copy of the carry-over
 * bytes that the caller passes to chttp1_stream_prepare(). Its lifetime does
 * not need that same constraint, because there is one instance for each
 * diverted request and the code does not embed it everywhere.
 *
 * This is the typical use. A reactor thread parses the headers with
 * chttp1_parser_execute() until that function returns CHTTP1_HEADERS_ONLY, and
 * then hands the fd to a worker thread, together with the buffer position that
 * chttp1_parser_consumed() gives; the bytes from that position to the end of
 * that buffer are the carry-over. The worker calls chttp1_stream_prepare() one
 * time with that carry-over and then calls chttp1_stream_read() again and
 * again; that function drains the carry-over first and then reads the raw fd.
 * The worker feeds each chunk to chttp1_parser_execute(), which keeps the SAME
 * parser instance moving through the body, the chunks and the trailers, until
 * the parser returns CHTTP1_PAUSED. chttp1_stream_write() is the symmetric
 * primitive for the phase where the worker owns the write of the response.
 */
typedef struct chttp1_stream {
  int fd;

  /* An opaque ctls_conn_t*, or NULL for a connection in plain text. The type is
   * void* instead of ctls_conn_t*, so that this header does not need to
   * #include ctls.h; chttp1_parser.c casts the pointer internally.
   * chttp1_stream_prepare_tls() sets this field, and chttp1_stream_prepare()
   * does not; see the doc comment of that function. With this field set,
   * chttp1_stream_read() and chttp1_stream_write() call ctls_conn_read() and
   * ctls_conn_write() instead of the raw read(2) and write(2). The code still
   * uses fd for the poll(2) waits on readiness, because ctls_conn_t
   * deliberately does not depend on a reactor and so has no poll primitive of
   * its own. */
  void *tls;

  /* The carry-over bytes: a heap copy of the bytes that the caller gave to
   * chttp1_stream_prepare, which the code drains with carry_pos before a real
   * read(2) touches fd. The pointer is NULL and the length is 0 if the caller
   * gave chttp1_stream_prepare no leftover bytes. On a TLS connection these are
   * application bytes that ctls_conn_read() already decrypted during the loop
   * in which the reactor thread parsed the headers, not raw bytes from the
   * wire. Their meaning is the same as in the case of plain text; they are only
   * already past the TLS layer. */
  char *carry;
  size_t carry_len;
  size_t carry_pos;

  /* The allocator that every buffer of this stream comes from and goes back to.
   * chttp1_stream_prepare() and chttp1_stream_prepare_tls() set it, and NULL
   * selects the default malloc and free through _ccol_mem_alloc(). The
   * carry-over buffer and the buffer that chttp1_stream_take_leftover() hands
   * out both come from it.
   *
   * Without this field the stream would call malloc() and free() directly, and
   * an embedder that installs its own allocator with
   * chttpsvr_set_engine_mem_mgmt_procs() would then find the body bytes of
   * every diverted request outside that allocator, with nothing to say so.
   * Every other buffer that a request owns already comes from the allocator of
   * its server. */
  ccol_memmgmt_procs_t *mp;

  int last_errno; /* the errno of the last read or write that failed, or 0 */
  bool timed_out; /* true if the last read or write failed because its
                   * deadline passed, which is a poll(2) that returns 0.
                   * Such a failure is not a real error of the I/O. For that
                   * same call, last_errno then has no meaning. */
  bool prepared;
  bool released;
  /* When true, chttp1_stream_write() never waits: it behaves as if every
   * call passed a timeout_ms of 0, and it reports a socket that cannot take
   * more bytes right now as a timeout (chttp1_stream_timed_out() gives
   * true). A caller that must not block a thread on a slow reader sets it,
   * and then waits for writability through its own reactor instead. The
   * prepare functions clear it. It has no effect on chttp1_stream_read(),
   * whose non-blocking form is an explicit timeout_ms of 0. */
  bool write_nonblocking;
  /* When true, the caller promises that fd is non-blocking, and every read
   * and write tries the I/O first and polls only after EWOULDBLOCK or
   * EAGAIN. A read or a write that would have succeeded therefore costs no
   * poll(2). The prepare functions clear it: a read on a plaintext stream
   * over a blocking fd must poll first, or the attempt itself blocks and
   * ignores timeout_ms. A plaintext write never blocks in the attempt
   * (MSG_DONTWAIT) whichever way this is set. */
  bool fd_nonblocking;
} chttp1_stream_t;

/**
 * @brief Prepares stream for the reads and the writes that a worker thread
 *        pulls from fd.
 *
 * leftover and leftover_len describe bytes that somebody else, usually a
 * reactor thread that parsed the headers, already read off fd. In the logical
 * order those bytes come BEFORE every further byte that a read of fd gives, so
 * the stream must deliver them first. This function copies them internally,
 * into a heap buffer that stream owns, so the caller can reuse or free its own
 * buffer immediately after this call returns. Pass a leftover_len of 0 if there
 * are no such bytes, and leftover may then be NULL.
 *
 * fd must be a connected stream socket. A plaintext write goes out with send(2)
 * or sendmsg(2) and MSG_NOSIGNAL, so a write to a peer that has closed or reset
 * fails with EPIPE or ECONNRESET and never raises SIGPIPE, whatever the
 * disposition of that signal in the process. A descriptor that is not a socket
 * fails every write with ENOTSOCK.
 *
 * Call this function synchronously, before another thread starts to touch fd at
 * the same time: in other words, call it BEFORE you hand fd to the mechanism
 * that wakes the worker thread, not after.
 *
 * @param mp  The allocator for every buffer that this stream owns, or NULL for
 *            the default malloc and free. The stream keeps the pointer, so it
 *            must stay valid until chttp1_stream_release(). The same allocator
 *            frees the buffer that chttp1_stream_take_leftover() hands out, and
 *            the CALLER of that function must use it too.
 * @return true after a success, and a leftover_len of 0 is a success. Gives
 *         false only after an allocation failure, which needs a leftover_len
 *         above 0 and a copy that failed. After a failure the stream holds the
 *         equivalent of all zero bytes, which is safe: a call to
 *         chttp1_stream_release() on it is still safe, although it is not
 *         needed and does no harm.
 */
bool chttp1_stream_prepare(chttp1_stream_t *stream, int fd,
                           const char *leftover, size_t leftover_len,
                           ccol_memmgmt_procs_t *mp);

/**
 * @brief Prepares stream for the reads and the writes that a worker thread
 *        pulls from a TLS connection.
 *
 * This function is the same as chttp1_stream_prepare(), with the same contract
 * for leftover and leftover_len; those bytes are application bytes that
 * ctls_conn_read() already decrypted in the loop where the reactor thread
 * parsed the headers, not raw bytes from the wire. One thing differs:
 * chttp1_stream_read() and chttp1_stream_write() then call ctls_conn_read() and
 * ctls_conn_write() on tls_conn, instead of the raw read(2) and send(2) on fd.
 * The stream still needs fd, but uses it only for the poll(2) waits on
 * readiness, because ctls_conn_t deliberately does not depend on a reactor and
 * has no poll primitive of its own.
 *
 * @param tls_conn  The ctls_conn_t, from ctls.h, that drives the encrypted I/O
 *                  of this connection. Its handshake must already be complete,
 *                  which means that ctls_conn_handshake_step() returned
 *                  CTLS_HANDSHAKE_DONE. This function does not take ownership
 *                  of it: the caller still destroys it with
 *                  ctls_conn_destroy(), after chttp1_stream_release().
 * @return The same as chttp1_stream_prepare().
 */
bool chttp1_stream_prepare_tls(chttp1_stream_t *stream, int fd, void *tls_conn,
                               const char *leftover, size_t leftover_len,
                               ccol_memmgmt_procs_t *mp);

/**
 * @brief Reads buflen bytes into buf, or fewer.
 *
 * The function drains the carry-over bytes that remain first, and while
 * carry-over remains it never touches fd at all. After the carry-over is empty,
 * it tries one read BEFORE it looks at poll(2) or at timeout_ms, through
 * ctls_conn_read() if chttp1_stream_prepare_tls() prepared the stream and
 * through a raw read(2) if not. The order matters for a TLS stream, because
 * OpenSSL can already hold decrypted application bytes in its own internal
 * buffer, fully independent of whether the raw fd still has anything for the
 * kernel to give, so checking the readiness of fd first would hide those bytes.
 * Only after an attempt reports EWOULDBLOCK or EAGAIN does this function block
 * in poll(2), waiting for fd to become readable for the milliseconds that
 * remain of timeout_ms, and then try again. For a TLS stream, one event of
 * readiness does not guarantee that application bytes come back immediately: a
 * partial TLS record can need another read, and so can a renegotiation message
 * or a key-update message that OpenSSL consumes internally. In that case
 * ctls_conn_read() reports EWOULDBLOCK or EAGAIN, and this function polls and
 * tries again without telling the caller, still bounded by the same total
 * budget of timeout_ms instead of a new budget for each retry.
 *
 * @param timeout_ms  A negative value blocks forever, with no timeout, which
 *                    matches the -1 convention of poll(2). A value of 0 returns
 *                    immediately if fd is not readable at that moment. A value
 *                    above 0 is the maximum number of milliseconds to wait,
 *                    over every internal retry together. The function does not
 *                    look at this value at all while carry-over bytes remain,
 *                    because it always returns those bytes immediately.
 * @return The number of bytes that it read, which is above 0; the carry-over
 *         alone can give all of them. Gives 0 on a clean EOF, which is a peer
 *         that closed its write side, or a clean TLS close_notify. Gives -1 on
 *         an error and on a timeout; use chttp1_stream_timed_out() and
 *         chttp1_stream_last_error() to tell those two apart.
 */
ssize_t chttp1_stream_read(chttp1_stream_t *stream, char *buf, size_t buflen,
                           int timeout_ms);

/**
 * @brief Gives true while stream still holds carry-over bytes, which the next
 *        chttp1_stream_read() returns without touching the file descriptor.
 */
static inline bool chttp1_stream_has_carry(const chttp1_stream_t *stream) {
  return stream->carry_pos < stream->carry_len;
}

/**
 * @brief Writes len bytes from buf to the fd of stream, or fewer. For a TLS
 *        stream it encrypts them and writes them with ctls_conn_write(). If the
 *        fd is not writable immediately, the function blocks in poll(2) for
 *        timeout_ms milliseconds at most, and for a TLS stream it retries
 *        inside that same budget, in the way that chttp1_stream_read()
 *        documents. The convention for timeout_ms is the same as in
 *        chttp1_stream_read().
 *
 * The phase in which a worker owns the write of the response needs this
 * function, because it bounds how long a peer that reads slowly can hold a
 * worker thread during the send of a response.
 *
 * @return The number of bytes that it wrote, which can be less than len, as
 *         with write(2) itself, so a caller that must write the whole of a
 *         larger buffer loops. Gives -1 on an error and on a timeout.
 */
ssize_t chttp1_stream_write(chttp1_stream_t *stream, const char *buf,
                            size_t len, int timeout_ms);

/**
 * @brief The largest total that chttp1_stream_writev2() copies into one
 *        buffer, to send a head and a body as one TLS record. It is the
 *        largest plaintext that one TLS record carries.
 */
#define CHTTP1_TLS_COALESCE_MAX 16384

/**
 * @brief Writes the bytes of a, and then the bytes of b, as one write where
 *        the transport allows it. Either part may be empty.
 *
 * It always tries the write first, and waits in poll(2) for at most
 * timeout_ms only after the socket reported that it is full, so fd must be
 * non-blocking. A plaintext stream sends both parts with one sendmsg(2),
 * which is the same gather write as writev(2), with MSG_NOSIGNAL. A
 * TLS stream has no gather write: when the two parts together fit in
 * CHTTP1_TLS_COALESCE_MAX bytes it copies them into one buffer and sends
 * them as one record; otherwise it sends a with one write and then tries b
 * at once, with no wait in between. write_nonblocking applies as for
 * chttp1_stream_write().
 *
 * A TLS write that reported "would block" must be retried with the same
 * bytes at the same position of the stream, which the next call with the
 * unsent rest of a and b always does; the connection allows the retry from a
 * different buffer.
 *
 * @return The number of bytes written, counted across a and then b; it can
 *         be less than alen + blen. Gives -1 on an error and on a timeout, as
 *         chttp1_stream_write() does.
 */
ssize_t chttp1_stream_writev2(chttp1_stream_t *stream, const char *a,
                              size_t alen, const char *b, size_t blen,
                              int timeout_ms);

/**
 * @brief Gives true if the last chttp1_stream_read or chttp1_stream_write
 *        call returned -1 because its deadline passed, which is a poll(2)
 *        that returns 0. Gives false if that call failed with a real error
 *        of the I/O.
 */
bool chttp1_stream_timed_out(const chttp1_stream_t *stream);

/**
 * @brief Returns the errno of the last chttp1_stream_read or
 *        chttp1_stream_write call that failed. Gives 0 if the last call
 *        succeeded, if it timed out, and if the carry-over alone satisfied
 *        it. For a timeout, use chttp1_stream_timed_out() instead.
 */
int chttp1_stream_last_error(const chttp1_stream_t *stream);

/**
 * @brief Pushes buf and len back onto the carry-over of stream, ahead of any
 *        bytes that already sit there unconsumed.
 *
 * This function is for a caller that drives chttp1_parser_execute() over the
 * output of chttp1_stream_read(). execute() can report a message boundary
 * (CHTTP1_HEADERS_ONLY or CHTTP1_PAUSED) with a chttp1_parser_consumed() below
 * the number of bytes that the caller just gave it. The trailing bytes are
 * already off the wire, and they belong to whatever comes next on this same
 * connection, most often the next request of a pipeline, so the caller must not
 * drop them. After a push back here, one later call to
 * chttp1_stream_take_leftover(), made after the read of this message is
 * completely done, reclaims them in a uniform way, whatever the source of those
 * bytes was: a real read(2) or ctls_conn_read() call, or part of the carry-over
 * that chttp1_stream_prepare() gave the stream which nothing ever needed (see
 * the doc comment of that function for the second case).
 *
 * This function copies buf, so the caller can reuse or free its own buffer
 * immediately after this call returns.
 *
 * @return true after a success, and a len of 0 is a safe success that does
 *         nothing. Gives false only after an allocation failure, after which
 *         the carry-over that stream holds, if there is any, stays completely
 *         as it was, and this mechanism cannot recover buf and len. This is a
 *         rare degradation that nothing can avoid when there is not enough
 *         memory, not a crash and not a corruption.
 */
bool chttp1_stream_push_back_leftover(chttp1_stream_t *stream, const char *buf,
                                      size_t len);

/**
 * @brief Reclaims what is left of the carry-over of stream, and gives the
 *        ownership of those bytes to the caller. Those bytes are the bytes that
 *        chttp1_stream_read never consumed. They can still be exactly the bytes
 *        that chttp1_stream_prepare() got, or
 *        chttp1_stream_push_back_leftover() can have replaced them or added to
 *        them.
 *
 * Call this function exactly one time, from the caller that drove the read of
 * this stream, immediately before chttp1_stream_release(); without this call,
 * that function silently frees those bytes and nothing can get them back. A
 * caller that skips this call loses the next request of a pipeline: the bytes
 * of that request are already permanently out of the receive buffer of the
 * socket in the kernel, so they disappear the instant the read of the current
 * request finishes, and the client waits for a response that never comes. After
 * this call the carry-over of stream is empty, exactly as if
 * chttp1_stream_prepare() got none at all.
 *
 * @param len_out  The function sets this to the number of bytes that it
 *                 returns. With a NULL return, it sets it to 0 when there was
 *                 nothing to reclaim, and to the number of bytes that could not
 *                 be reclaimed after an allocation failure. A caller that keeps
 *                 the connection alive must close it instead when it sees NULL
 *                 with a length above 0, because the next request on it is
 *                 lost.
 * @return A heap buffer that the caller owns. It comes from the allocator of
 *         the stream, which is the mp that chttp1_stream_prepare() or
 *         chttp1_stream_prepare_tls() got, and the caller must release it with
 *         _ccol_mem_free() and that same mp; a plain free() is wrong whenever
 *         mp is a custom allocator. _ccol_mem_free() accepts NULL, so the
 *         caller can release the result even when *len_out is 0. Gives NULL if
 *         there was nothing left to reclaim, and after an allocation failure;
 *         *len_out tells the two apart. After such a failure this call cannot
 *         recover the bytes, but the state of stream stays valid, and
 *         chttp1_stream_release() still cleans it up in the normal way.
 */
char *chttp1_stream_take_leftover(chttp1_stream_t *stream, size_t *len_out);

/**
 * @brief Frees the resources of stream, which are its carry-over buffer, if it
 *        has one. This function does NOT close fd, because
 *        chttp1_stream_prepare() never gives the ownership of the file
 *        descriptor itself to this struct.
 *
 * Call this function exactly one time, after the read is completely done: at an
 * EOF, after an error, or when a handler stopped its reads and writes early. A
 * call on a stream that holds all zero bytes, which nothing prepared, is safe,
 * and a second call is also safe and does nothing.
 */
void chttp1_stream_release(chttp1_stream_t *stream);
