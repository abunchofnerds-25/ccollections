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
#include <stdbool.h> /* bool, true and false are keywords in C23 */
#endif
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* ssize_t, for chttp1_stream_read()/_write() */

/**
 * @file chttp1_parser.h
 * @brief INTERNAL ONLY. A small hand-written HTTP/1.1 parser for requests
 *        and responses. chttpclient.c uses it in response mode and
 *        chttpserver.c uses it in request mode.
 *
 * It isn't a public collections module: no macros and no opaque handle type.
 * No public header includes it (neither chttp.h, chttpclient.h nor
 * chttpserver.h), and only src/chttpclient.c and src/chttpserver.c are meant
 * to #include it.
 *
 * One parser covers both grammars. Response mode parses exactly the subset
 * chttpclient.c needs, and request mode covers what chttpserver.c needs,
 * adding chttp1_parser_init_request(), settings->on_request_line,
 * CHTTP1_HEADERS_DIVERT_BODY, CHTTP1_HEADERS_ONLY and the request-specific
 * body framing rule of RFC 7230 SS3.3. The two modes share the core that
 * handles headers, chunks, trailers and size caps instead of duplicating it,
 * and the internal state machine of chttp1_parser_t makes that natural since
 * both grammars share every state after the first line.
 *
 * chttp1_stream_t and its prepare, read, write, last_error and release
 * functions are a separate little I/O helper that lets a worker thread pull
 * bytes without depending on a reactor. It makes real read(2), write(2) and
 * poll(2) system calls and moves raw bytes on a file descriptor; see its own
 * section below. That helper needs more than the C standard library, because
 * of poll(2) and the raw socket read and write. chttp1_parser_t itself is
 * different: the parser proper is pure computation without I/O, and it only
 * allocates when a response parser spills a long line; see
 * chttp1_parser_enable_line_spill().
 *
 * ### Design: a plain, self-contained value type
 *
 * A request parser, and a response parser that never called
 * chttp1_parser_enable_line_spill(), own no resources, so they're safe to
 * embed by value on the stack or in a struct, and they need no release. That
 * works because the internal buffer that accumulates one line (the first
 * line, the header lines and the chunk-size lines) is a FIXED-size array of
 * CHTTP1_MAX_LINE_LEN bytes (defined below) held directly in the struct. The
 * fixed size is also what enforces the line length cap: a line that doesn't
 * end before the buffer fills up is rejected as "line too long". Body bytes
 * and chunk data are never copied into this buffer; they go straight from
 * the caller's read buffer to on_body without a copy. An init clears the
 * parser's state but never this buffer, so a per-message reset costs a few
 * cache lines and not 8 KiB; the comment on chttp1_parser_t.line_buf states
 * the invariant that makes that safe.
 *
 * A response parser can opt in to longer header and trailer lines with
 * chttp1_parser_enable_line_spill(). A line that fills the fixed buffer then
 * moves to one heap buffer of CHTTP1_MAX_SPILL_LINE_LEN bytes, allocated
 * through the caller's memory procs, and the total header byte cap bounds
 * that line too. Once it spills, such a parser owns that buffer, and the
 * caller must call chttp1_parser_release() before discarding the parser or
 * initializing it again. Request mode never spills and never allocates.
 *
 * ### Header size protections
 *
 * A malicious or broken peer can send one endless header line, one byte at a
 * time. Three limits hold such a peer back: on the length of one header
 * line, on the number of headers and on their total size, and without them
 * the peer could make memory grow without bound. The parser applies three
 * fixed limits that together bound this: CHTTP1_MAX_LINE_LEN, defined below,
 * and CHTTP1_MAX_HEADER_COUNT and CHTTP1_MAX_TOTAL_HEADER_BYTES, which are
 * private to chttp1_parser.c because they don't affect this struct's layout.
 * A chunked body ends with a zero-length chunk, and the trailer headers after
 * it count against the same header count and total byte budget as the
 * ordinary headers. Trailers get no separate allowance, so nothing can slip
 * past the cap through them.
 *
 * ### Transfer-Encoding framing
 *
 * Repeated Transfer-Encoding header lines form one comma-separated list of
 * transfer codings, in order (RFC 7230 SS3.2.2), so every rule below applies
 * to that MERGED list and not to a single line. An empty list element names
 * no coding and is ignored (RFC 7230 SS7), whether it comes from a trailing
 * comma or from a whole line with no coding in it, so "chunked" followed by
 * an empty Transfer-Encoding line is the list "chunked".
 *
 * Both modes reject two things with CHTTP1_ERROR:
 *  - "chunked" appearing more than once in the merged list, in any spelling
 *    and across any lines. RFC 7230 SS3.3.1 forbids applying chunked more
 *    than once to one message body, and a recipient that de-chunks once and
 *    one that de-chunks twice would also disagree about where the body ends.
 *  - A Content-Length beside any Transfer-Encoding, a pair RFC 7230 SS3.3.3
 *    calls ambiguous.
 *
 * Where the two modes differ, on purpose, is a merged list whose FINAL
 * coding isn't "chunked". A REQUEST is rejected, because a request has no
 * EOF-delimited framing to fall back on, so its length is indeterminate. A
 * RESPONSE is accepted and read until the connection closes (RFC 7230
 * SS3.3.3), which is the framing the specification defines for it. Such a
 * message never qualifies for keep-alive.
 *
 * In request mode a chunk-size line follows the RFC 9112 SS7.1 grammar:
 * whitespace after the size is only accepted before a ";" extension. In
 * response mode whitespace between the size and the CRLF without any
 * extension ("5 \r\n") is accepted too, as the Go net/http and curl clients
 * do.
 *
 * ### Thread safety
 *
 * A chttp1_parser_t isn't thread-safe, since nothing here is meant to be
 * shared between threads. That matches how chttpclient.c uses it: one
 * instance per hop in flight, and one thread drives that hop and touches the
 * instance (in the async tier, one reactor callback invocation touches it
 * instead). It stays true across a CHTTP1_HEADERS_ONLY divert in request
 * mode: exactly one thread touches the parser at any moment, and WHICH thread
 * that is may change once, at the divert point, from a reactor thread to a
 * worker thread. There's no concurrent access, only a handoff between two
 * single owners that the caller has to sequence correctly; see the doc
 * comment of CHTTP1_HEADERS_ONLY. chttp1_stream_t follows the same rule: the
 * caller hands one instance to exactly one worker thread at a time and never
 * shares it.
 */

/* ========================================================================== */
/*                         RESULT CODES                                      */
/* ========================================================================== */

/**
 * @brief The outcome of chttp1_parser_execute() or chttp1_parser_finish().
 *
 * These codes mirror the exact three-way split chttpclient.c branches on.
 * CHTTP1_OK means "keep reading, the message isn't complete yet".
 * CHTTP1_PAUSED means the message completed; see the hard contract on
 * chttp1_parser_execute below. CHTTP1_USER means an application callback
 * (on_header, on_trailer, on_request_line, on_headers_complete, on_body or
 * on_message_complete) reported an error itself. CHTTP1_ERROR covers every
 * parse failure, like a malformed status line, an invalid header, a chunk
 * size overflow or an oversized header. CHTTP1_ERROR isn't split into one
 * code per cause, because chttpclient.c never needs to tell one parse
 * failure from another beyond "not OK, PAUSED or USER". A human-readable
 * reason for the exact rejection is still there in chttp1_parser_t.reason,
 * for diagnostics and tests.
 */
typedef enum {
  CHTTP1_OK = 0,
  CHTTP1_PAUSED,
  CHTTP1_USER,
  CHTTP1_ERROR,
  /**
   * Request mode only; see chttp1_parser_init_request(). The headers were
   * parsed and validated in full, and settings->on_headers_complete returned
   * CHTTP1_HEADERS_DIVERT_BODY (see that callback's doc comment). That
   * return value pauses the parse right here, before a single body byte is
   * consumed, so the parser doesn't go on into the body inside this same
   * chttp1_parser_execute() call. chttp1_parser_consumed() reports exactly
   * how many bytes of THIS call's buffer were the header block, and nothing
   * from that point to len is consumed. Those remaining bytes are probably
   * the start of the body, or bytes of a pipelined message. They belong to
   * the caller, which hands them off however it likes, for example as the
   * leftover argument of chttp1_stream_prepare() when it diverts reading the
   * body to a worker thread.
   *
   * Unlike CHTTP1_PAUSED, which is terminal, this code is NOT: the parser is
   * still in the middle of the message. parser->state already sits at the
   * applicable body framing state, content-length or chunked. (If the
   * request has no body at all, that state is already complete instead and
   * this code is never returned; see the callback's doc comment.) Feeding
   * more bytes through chttp1_parser_execute() from the thread that now owns
   * the connection continues exactly where this call stopped, which is the
   * intended use. More bytes after CHTTP1_PAUSED break the contract; more
   * bytes here don't.
   */
  CHTTP1_HEADERS_ONLY
} chttp1_errno_t;

/**
 * @brief The grammar chttp1_parser_t uses for its first line.
 *        chttp1_parser_init() sets CHTTP1_PARSE_RESPONSE and
 *        chttp1_parser_init_request() sets CHTTP1_PARSE_REQUEST. The value
 *        is read-only after that.
 */
typedef enum {
  CHTTP1_PARSE_RESPONSE = 0,
  CHTTP1_PARSE_REQUEST
} chttp1_parser_type_t;

/**
 * @brief The constants settings->on_headers_complete returns.
 *
 * CHTTP1_HEADERS_HAS_BODY and CHTTP1_HEADERS_NO_BODY apply in both parse
 * modes. Their values are 0 and 1, so a callback returning a bare literal 0
 * or 1 behaves exactly like one returning the named constant.
 * CHTTP1_HEADERS_DIVERT_BODY is valid ONLY in request mode; see the doc
 * comment of chttp1_settings_t.on_headers_complete for its full contract.
 * Returned in response mode it's an invalid hint and the parser answers with
 * CHTTP1_USER, because chttpclient.c has no notion of diverting the
 * connection to a worker thread.
 */
#define CHTTP1_HEADERS_HAS_BODY 0
#define CHTTP1_HEADERS_NO_BODY 1
#define CHTTP1_HEADERS_DIVERT_BODY 2

/* ========================================================================== */
/*                         INTERNAL STATE (opaque to chttpclient.c)          */
/* ========================================================================== */

/* This enum isn't part of the contract this module gives chttpclient.c. It's
 * declared here for one reason only: chttp1_parser_t must be a complete
 * type that a caller can embed on the stack. */
typedef enum {
  CHTTP1_ST_FIRST_LINE = 0, /* the status line in response mode, or the
                             * request line in request mode; parser->type
                             * decides the grammar. No separate states are
                             * needed, since both modes share every state
                             * after this one. */
  CHTTP1_ST_HEADERS,
  CHTTP1_ST_BODY_CONTENT_LENGTH,
  CHTTP1_ST_BODY_CHUNK_SIZE,
  CHTTP1_ST_BODY_CHUNK_DATA,
  CHTTP1_ST_BODY_CHUNK_CRLF,
  CHTTP1_ST_BODY_CHUNK_TRAILERS,
  CHTTP1_ST_BODY_EOF,
  CHTTP1_ST_MESSAGE_DONE,
  CHTTP1_ST_DEAD /* hard parse error; execute() does nothing */
} chttp1__state_t;

/* The finish state has three values: a clean boundary, a completion that an
 * EOF delimits, and a message an EOF cut in the middle. That three-way split
 * is what makes chttp1_parser_finish() correct without the call site having
 * to know which body framing was in force. */
typedef enum {
  CHTTP1_FINISH_SAFE = 0,     /* clean message boundary, nothing pending */
  CHTTP1_FINISH_SAFE_WITH_CB, /* EOF-delimited body; the EOF itself
                                 completes the message */
  CHTTP1_FINISH_UNSAFE        /* EOF in the middle of a message; the
                                 message is truncated and invalid */
} chttp1__finish_state_t;

/**
 * @brief The fixed size of the internal buffer that accumulates one line.
 *        That buffer is chttp1_parser_t._line_buf below, and it holds the
 *        status line, a header line or a chunk-size line.
 *
 * This one constant does two jobs: it's the size of the buffer and also the
 * threshold for rejecting a line as too long, except for the header and
 * trailer lines of a response parser that spills them to the heap (see
 * chttp1_parser_enable_line_spill() and the doc comment at the top of this
 * file). A line that doesn't end before this many bytes have accumulated is
 * rejected, so there's no separate "cap" to keep equal to the buffer's real
 * size. The constant lives here and not in chttp1_parser.c because it
 * decides the layout of chttp1_parser_t, which must be a complete type for
 * chttpclient.c to embed by value. 8192 matches the read buffer size that
 * chttpclient.c uses as a literal at both of its socket-read call sites.
 *
 * The other two limits, the maximum header count and the maximum cumulative
 * header bytes, don't affect this struct's layout: they're plain running
 * counts compared against constants private to chttp1_parser.c.
 */
#define CHTTP1_MAX_LINE_LEN 8192

/**
 * @brief Capacity of the heap buffer that holds a response parser's header
 *        or trailer line once it outgrows CHTTP1_MAX_LINE_LEN; see
 *        chttp1_parser_enable_line_spill(). The total header byte cap
 *        bounds the line as well.
 */
#define CHTTP1_MAX_SPILL_LINE_LEN (64 * 1024)

typedef struct chttp1_parser chttp1_parser_t;

/**
 * @brief The callback settings: on_header, on_trailer, on_request_line,
 *        on_headers_complete, on_body and on_message_complete.
 *        on_request_line only applies in request mode. A whole header line
 *        is always assembled internally before on_header fires, so the
 *        caller needs no fragment reassembly of its own.
 *
 * Every callback is checked for NULL before it's called, so a caller only
 * wires the ones it needs. chttpclient.c and chttpserver.c both leave
 * on_trailer NULL, since neither offers a trailer API of its own.
 */
typedef struct {
  /**
   * Fires once for each complete header line of the message's header block,
   * meaning the lines between the first line and the blank line that ends
   * the block. The trailer fields of a chunked body NEVER reach this
   * callback; they go to on_trailer instead. So a caller can keep one single
   * header collection, and a trailer field named like a real header can
   * never silently replace that header.
   *
   * name and value point into this parser's internal line buffer and are
   * valid ONLY during this call, so a callback that needs to keep them copies
   * them before returning. name is exactly as it came off the wire, since
   * the parser doesn't lower-case it. Optional white space (SP and HTAB) has
   * already been trimmed from both ends of value.
   *
   * Return 0 on success. Return non-zero to stop the parse with CHTTP1_USER.
   * The caller of chttp1_parser_execute only sees that value indirectly,
   * through CHTTP1_USER; nothing passes the raw return value on to it.
   */
  int (*on_header)(chttp1_parser_t *p, const char *name, size_t name_len,
                   const char *value, size_t value_len);

  /**
   * Fires once for each complete trailer field line (RFC 7230 SS4.1.2). A
   * chunked body ends with a zero-length chunk, and these lines come after
   * that chunk and before the final blank line. It never fires for a
   * message's header block, and never at all for a message without a chunked
   * body.
   *
   * name and value follow the same contract as in on_header for lifetime,
   * spelling and optional white space trimming. A trailer line is validated
   * with exactly the same grammar (the name is a token, the value holds no
   * control characters) and charged against exactly the same header count
   * and total header byte budget as a header line. Trailers get no separate
   * allowance, so nothing can slip past the caps through them.
   *
   * A trailer field never changes this parser's view of the message: only
   * the header block is interpreted for Content-Length, Transfer-Encoding,
   * Connection and Expect. So a trailer can't change the framing already in
   * force, nor what chttp1_should_keep_alive(), chttp1_has_content_length(),
   * chttp1_declared_content_length() and chttp1_expects_continue() report. A
   * trailer field with one of those names just arrives here as an ordinary,
   * inert trailer.
   *
   * chttp1_settings_init() leaves this callback NULL. When it's NULL, every
   * trailer field is validated and then thrown away, which is what a caller
   * without a trailer API wants: a trailer then has no way at all to get
   * into that caller's header collection.
   *
   * Return 0 on success. Return non-zero to stop the parse with CHTTP1_USER.
   */
  int (*on_trailer)(chttp1_parser_t *p, const char *name, size_t name_len,
                    const char *value, size_t value_len);

  /**
   * Request mode only, which chttp1_parser_init_request() selects; it never
   * fires in response mode. It fires once, after the request line has been
   * parsed and validated in full and before the first header line, when
   * p->http_major and p->http_minor already hold their values.
   *
   * method and target point into this parser's internal line buffer and are
   * valid ONLY during this call, exactly like name and value in on_header,
   * so a callback that needs to keep them copies them before returning.
   * Neither is validated beyond the basic grammar: method is a token of
   * tchar bytes only, and target is a run of one or more bytes without any
   * control character, DEL or white space (here white space means both SP
   * and HTAB; see RFC 7230 SS3.1.1 and SS5.3). The parser doesn't tell an
   * origin-form target from an absolute-form, authority-form or
   * asterisk-form one, and it doesn't reject a method name it doesn't know.
   * Both are the caller's routing business; an unsupported method, for
   * example, becomes an ordinary 405 and not a parse error.
   *
   * Return 0 on success. Return non-zero to stop the parse with CHTTP1_USER.
   */
  int (*on_request_line)(chttp1_parser_t *p, const char *method,
                         size_t method_len, const char *target,
                         size_t target_len);

  /**
   * Fires once, after the blank line that ends the header block. In response
   * mode p->status_code already holds its value.
   *
   * Return CHTTP1_HEADERS_HAS_BODY (0) if this message has a body, which is
   * the normal case. Return CHTTP1_HEADERS_NO_BODY (1) if the caller already
   * knows from elsewhere that no body follows, for example for a request
   * whose method was HEAD; this holds even when a Content-Length header is
   * present. Return CHTTP1_HEADERS_DIVERT_BODY (2) to pause the parse here
   * instead of going on into the body inside this same
   * chttp1_parser_execute() call. That value is for request mode only; the
   * CHTTP1_HEADERS_ONLY doc comment of that function gives the contract.
   * It lets a caller route the request from the headers alone, before
   * deciding whether and how to read the body, for example to hand reading
   * the body off to a worker thread. Return any OTHER value, a negative one
   * included, to stop the parse with CHTTP1_USER.
   *
   * The split is on purpose. A real on_headers_complete needs a genuine way
   * to report an error here; on the response side, for example, a failure to
   * allocate a copy of a Location header while detecting a redirect. So a
   * value outside the accepted set (CHTTP1_HEADERS_HAS_BODY,
   * CHTTP1_HEADERS_NO_BODY, and CHTTP1_HEADERS_DIVERT_BODY in request mode)
   * is always treated as a reported error and never silently turned into
   * "no body".
   *
   * CHTTP1_HEADERS_DIVERT_BODY is downgraded to the ordinary has-body
   * behavior whenever there's nothing meaningful to divert: it's then treated
   * as CHTTP1_HEADERS_HAS_BODY and the body is parsed in this same call. Two
   * kinds of request get the downgrade. A request with neither a
   * Content-Length nor a chunked Transfer-Encoding has no body at all,
   * whatever this callback returns (see the doc comment of
   * CHTTP1_HEADERS_ONLY), and a request with an explicit "Content-Length: 0"
   * completes at once for the same reason; pausing to divert an empty
   * body to a worker thread would be pointless. A chunked body is always
   * really diverted, even one whose first chunk is the empty terminating
   * chunk, because that can only be known from the first chunk-size line,
   * which hasn't been read when this callback runs.
   */
  int (*on_headers_complete)(chttp1_parser_t *p);

  /**
   * Fires zero or more times with the body bytes as they're parsed out of
   * the framing, whether a Content-Length delimits it or it's chunked. at
   * points straight into the buffer the caller passed to
   * chttp1_parser_execute, NOT into this parser's line buffer, since body
   * bytes are never copied internally. at is only valid during this call.
   *
   * Return 0 on success. Return non-zero to stop the parse with CHTTP1_USER.
   */
  int (*on_body)(chttp1_parser_t *p, const char *at, size_t len);

  /**
   * Fires exactly once, when the whole message has been parsed: the headers
   * plus whatever body framing applies. The parser knows without help that it
   * has reached a message boundary, because it drives its own explicit state
   * machine instead of learning it from a callback's return value, so this
   * callback follows the same plain convention as the others. Return 0 on
   * success. Return non-zero to stop the parse with CHTTP1_USER.
   * chttp1_parser_execute and chttp1_parser_finish report the pause itself
   * with their own CHTTP1_PAUSED return value, whatever this callback returns
   * beyond zero versus non-zero.
   */
  int (*on_message_complete)(chttp1_parser_t *p);
} chttp1_settings_t;

/**
 * @brief The parser itself. It is self-contained, and it only owns a
 *        resource once a response parser with line spilling enabled has
 *        spilled a line; see chttp1_parser_enable_line_spill() and
 *        chttp1_parser_release(). It's safe to embed by value on the stack
 *        and as a struct member.
 */
struct chttp1_parser {
  /** Opaque to the parser. The caller sets it once after chttp1_parser_init
   * and reads it back in every callback to get its own context. */
  void *data;

  /** Set once in on_headers_complete from the parsed status line. Response
   * mode only; always 0 in request mode. */
  int status_code;

  /** Set once by chttp1_parser_init() or chttp1_parser_init_request() and
   * read-only after that. It decides whether CHTTP1_ST_FIRST_LINE parses a
   * status line or a request line, and it gates three request-mode-only
   * behaviors: firing on_request_line, accepting CHTTP1_HEADERS_DIVERT_BODY
   * from on_headers_complete, and the RFC 7230 SS3.3 rule that "no framing
   * means no body, not an EOF-delimited body". */
  chttp1_parser_type_t type;

  /** For diagnostics only. A static string describing the last rejection,
   * for example "Duplicate Content-Length", or NULL. chttpclient.c's error
   * reporting is built on ccol_retval_t and doesn't read it, because every
   * CHTTP1_ERROR becomes the same ccol_http_transfer_aborted whatever the
   * reason. It's here for logs and for this module's test suite, which
   * asserts on the exact reason of a rejection. */
  const char *reason;

  /* Everything below is internal state that chttpclient.c neither reads nor
   * writes directly. */
  chttp1__state_t state;
  chttp1__finish_state_t finish_state;
  uint8_t http_major;
  uint8_t http_minor;
  uint16_t flags;
  uint64_t content_length; /* bytes left in
                            * CHTTP1_ST_BODY_CONTENT_LENGTH, or bytes left in
                            * the CURRENT chunk in CHTTP1_ST_BODY_CHUNK_DATA;
                            * one field serves both states. */
  uint64_t chunk_excess;   /* chunk framing bytes beyond what the chunk
                            * data so far earns; see parse_chunk_size_line()
                            * in chttp1_parser.c */

  size_t header_count;
  size_t total_header_bytes;
  /* Request mode only. Counts the blank lines accepted before the request
   * line itself (RFC 7230 SS3.5), bounded by CHTTP1_MAX_LEADING_BLANK_LINES,
   * a constant private to chttp1_parser.c. */
  size_t leading_blank_lines;

  /* Request mode only. Counts the Host header lines in the current request's
   * header block. RFC 7230 SS5.4 makes the count itself a validity rule of
   * the message and not a routing concern: a server must answer 400 for any
   * request with more than one Host line, and for an HTTP/1.1 request with
   * none. Two Host lines let a front end and a back end disagree about which
   * authority the request names, which is what request smuggling needs. The
   * count saturates, since nobody asks for more than "0, 1 or many". The rule
   * is applied at the blank line that ends the header block, because only
   * there is the count final. Response mode never touches this field. */
  uint8_t host_count;

  /* Response mode only. True once chttp1_parser_enable_line_spill() has
   * allowed a header or trailer line to outgrow line_buf. line_spill_mp are
   * that heap buffer's memory procs, and line_spill is the buffer itself
   * once a line needed it, or NULL. */
  bool line_spill_enabled;

  /** An optional per-parser override of the built-in caps
   * CHTTP1_MAX_HEADER_COUNT and CHTTP1_MAX_TOTAL_HEADER_BYTES, both private
   * to chttp1_parser.c. 0 means "use the built-in default", and
   * chttp1_parser_init() and chttp1_parser_init_request() leave both fields
   * at 0. Set either one above 0 at any time before feeding the first byte to
   * override that cap for this parser instance. They exist for chttpserver's
   * configurable chttpsvr_config_t.max_header_bytes. chttpclient has no
   * public control like that and never sets them, so it gets the built-in
   * caps.
   */
  size_t max_header_count_override;
  size_t max_total_header_bytes_override;

  /** An optional per-parser cap on the declared size of one chunk. It only
   * applies to chunked bodies and has no effect on a Content-Length-framed
   * body. The default 0 means no cap, and then a chunk-size line is accepted
   * for any value that fits in a uint64_t. Above 0, a chunk-size line whose
   * decoded value exceeds the cap is rejected with CHTTP1_ERROR as soon as
   * the line is parsed (see chttp1_chunk_size_limit_exceeded()), before
   * waiting for a single byte of that chunk's data. It exists for
   * chttpserver's max_body_size. Without it a peer could declare one
   * absurdly large chunk (the syntax allows any chunk-size token up to
   * UINT64_MAX) and never send it, holding a worker thread until a read
   * timeout fires, because max_body_size itself is only checked against the
   * bytes that arrive. The comment of chttp1_declared_content_length covers
   * the same gap for a Content-Length-framed body. chttpclient.c has no
   * public control like that and never sets this field. Any request parser
   * that serves a body size limit should set it, since the limit alone
   * leaves this hole open. */
  uint64_t max_chunk_size_override;

  /* line_len counts the bytes of the current line in line_buf, or in
   * line_spill once a response parser has spilled that line. line_spill_mp
   * are the heap buffer's memory procs. */
  size_t line_len;
  char *line_spill;
  ccol_memmgmt_procs_t *line_spill_mp;

  size_t consumed; /* the value behind chttp1_parser_consumed(),
                    * which only means something right after
                    * chttp1_parser_execute returns CHTTP1_PAUSED. See that
                    * function's doc comment. */

  const chttp1_settings_t *settings;

  /* A fixed-size buffer that accumulates the status line, every header line,
   * every trailer line and every chunk-size line. It's never used for body
   * bytes or chunk data, which go straight from the caller's buffer to
   * on_body. A partial line spanning two chttp1_parser_execute calls
   * accumulates here. The comment of CHTTP1_MAX_LINE_LEN explains why this
   * buffer has a fixed size and doesn't grow. A response parser that has
   * spilled a line uses line_spill instead, for that line and every later
   * one of the message.
   *
   * This buffer must stay the LAST member. chttp1_parser_init() and
   * chttp1_parser_init_request() clear every member in front of it and never
   * the buffer itself, so a reset costs the size of the state and not the
   * size of the buffer, and an earlier message's bytes stay in it. That's
   * safe because of one invariant: no code reads a byte of line_buf (or of
   * line_spill) that hasn't been written since the last reset. The init sets
   * line_len to 0. accumulate_line() writes byte line_len before counting it,
   * and looks back at most one byte, at a byte it already counted. Every
   * reader of a line (the status line, the request line, a header or
   * trailer line, a chunk-size line, and the copy into line_spill) stops at
   * line_len, and nothing reads the buffer as a NUL-terminated string. A new
   * reader of the buffer must keep this invariant, or else the reset has
   * to clear the buffer again.
   *
   * The buffer is aligned for any object type and its size is a multiple of
   * that alignment, so the struct ends exactly where the buffer ends on every
   * target. That keeps the static assertion next to parser_init_common()
   * exact. Without the alignment, a target whose 8-byte members need 8-byte
   * alignment (armhf) would pad the struct after the buffer, and only a
   * weaker test could pass there, one that a small member placed after the
   * buffer would pass as well. */
  _Alignas(max_align_t) char line_buf[CHTTP1_MAX_LINE_LEN];
};

/* ========================================================================== */
/*                         GRAMMAR HELPERS                                    */
/* ========================================================================== */

/**
 * @brief The tchar rule of RFC 7230 SS3.2.6: true if byte c may legally
 *        appear in a header field NAME. The same rule applies to the token
 *        of an HTTP method.
 *
 * It's exposed for chttp_request_set_header() of chttpclient.c and for
 * chttpsvr_resp_set_header() of chttpserver.c, which both check that a
 * caller's header NAME is a real token before accepting it. The parser
 * applies the same check to bytes from the wire, to a request line's method
 * and to a header name.
 */
bool chttp1_is_tchar(unsigned char c);

/* ========================================================================== */
/*                         LIFECYCLE                                          */
/* ========================================================================== */

/** @brief Sets every byte of a chttp1_settings_t to zero, which leaves every
 *         callback NULL. */
void chttp1_settings_init(chttp1_settings_t *settings);

/**
 * @brief Initializes parser for a new response message. It returns nothing
 *        and can't fail, since it allocates nothing.
 *
 * settings must outlive parser.
 *
 * There's no chttp1_parser_reset(). chttpclient.c never uses one parser
 * instance for two hops, since every hop builds a new instance, in the sync
 * tier and in the async tier, so this parser has no in-place reuse contract
 * either. Build a new instance per message, or run chttp1_parser_init on the
 * instance again. The init overwrites every field except the contents of the
 * line buffer, which no later read reaches before it's written again (see
 * the comment of line_buf). A parser that enabled line spilling must go
 * through chttp1_parser_release() first, or its heap line buffer leaks. The
 * init is cheap: it clears the parser's state and not its 8 KiB line
 * buffer.
 */
void chttp1_parser_init(chttp1_parser_t *parser,
                        const chttp1_settings_t *settings);

/**
 * @brief Initializes parser for a new REQUEST message, with parser->type set
 *        to CHTTP1_PARSE_REQUEST. It returns nothing and can't fail, and
 *        otherwise has the same contract as chttp1_parser_init().
 *
 * The parser then parses "method SP request-target SP HTTP-version CRLF"
 * instead of a status line and fires settings->on_request_line once, before
 * the first header. It applies RFC 7230 SS3.3's body framing rule for a
 * request: a message with neither a Content-Length nor a chunked coding has
 * no body at all and never gets the EOF-delimited framing that only a
 * response can have. It also accepts CHTTP1_HEADERS_DIVERT_BODY from
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
 * fills the fixed buffer moves to one heap buffer, allocated through mp
 * (NULL picks the default allocator) and kept for the rest of the message.
 * The status line and the chunk-size lines stay bounded by
 * CHTTP1_MAX_LINE_LEN until a header line spills. A failed allocation
 * reports the line as too long.
 *
 * Once this has returned true the parser may own that buffer, and the
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
 *        has one. Safe on any initialized parser and safe to call more than
 *        once. The parser must not be fed after this; to reuse the struct,
 *        initialize it again.
 */
void chttp1_parser_release(chttp1_parser_t *parser);

/* ========================================================================== */
/*                         PARSING                                           */
/* ========================================================================== */

/**
 * @brief Feeds len bytes of data to the parser.
 *
 * Three contracts apply, and the first two carry the read loop of
 * chttpclient.c, which calls this once per socket read.
 *
 * 1. In the CHTTP1_OK case it ALWAYS consumes the whole input buffer. A
 *    partial line is buffered internally across calls, and a partial count
 *    of body or chunk bytes is carried across calls as needed, so it never
 *    returns CHTTP1_OK with bytes left over. Only CHTTP1_PAUSED leaves a
 *    well-defined "consumed" position below len; see
 *    chttp1_parser_consumed().
 *
 * 2. Don't feed more bytes to this parser instance after it returns
 *    CHTTP1_PAUSED. The bytes from chttp1_parser_consumed() to len in the
 *    buffer that caused the pause aren't consumed. In response mode, the
 *    only way chttpclient.c uses this parser, the caller throws them away:
 *    chttpclient.c never feeds this instance again after CHTTP1_PAUSED and
 *    has no notion of pipelining at all. In request mode chttpserver.c DOES
 *    treat those bytes as real bytes of a pipelined request instead of
 *    throwing them away; see chttp1_stream_push_back_leftover(), which
 *    exists to feed them into the parse of the next request. This contract
 *    only covers response mode.
 *
 * 3. Contract 2 does NOT cover CHTTP1_HEADERS_ONLY, which only appears in
 *    request mode. That code isn't terminal, so more bytes after it, through
 *    another chttp1_parser_execute call from the thread that now owns the
 *    connection, are expected and break nothing. See the doc comment of
 *    CHTTP1_HEADERS_ONLY.
 *
 * @return CHTTP1_OK          The whole buffer was consumed. The message
 *                            isn't complete yet, so call again with more
 *                            data.
 *         CHTTP1_PAUSED      The message is complete and
 *                            on_message_complete fired. See
 *                            chttp1_parser_consumed().
 *         CHTTP1_HEADERS_ONLY Request mode only. See its own doc comment.
 *         CHTTP1_USER        An application callback reported an error.
 *         CHTTP1_ERROR       The input is malformed. See parser->reason.
 */
chttp1_errno_t chttp1_parser_execute(chttp1_parser_t *parser, const char *data,
                                     size_t len);

/**
 * @brief Call this when the underlying connection reports EOF, meaning a
 *        read() or recv() returned 0. It tells you whether that EOF is a
 *        valid end for the message in progress.
 *
 * Some messages, like an HTTP/1.0-style response or a response with an
 * explicit Connection: close, have neither a Content-Length nor a chunked
 * Transfer-Encoding. Such a message is only complete when the connection
 * closes, because nothing else signals its end. In that case, and only
 * then, this function fires on_message_complete and returns CHTTP1_PAUSED.
 * CHTTP1_PAUSED from THIS function means "yes, the EOF was a valid, clean
 * end of the message", not "keep going"; there's nothing after it to
 * consume, so there's no chttp1_parser_consumed() equivalent to read
 * afterwards.
 *
 * In every other state it returns CHTTP1_OK if the message was already
 * complete, and CHTTP1_ERROR if the EOF came in the middle of the message,
 * which makes the response truncated and invalid. Those other states are
 * five: before the status line, inside the headers, inside a
 * Content-Length-framed body, inside a chunk, and at a clean
 * CHTTP1_ST_MESSAGE_DONE boundary with nothing more expected. One outcome
 * is left: CHTTP1_USER, when the on_message_complete this function fires
 * itself returns non-zero.
 *
 * @return CHTTP1_OK      The parser is already at a clean boundary. The EOF
 *                        is fine, and no callback is needed.
 *         CHTTP1_PAUSED  An EOF-terminated body completed through
 *                        on_message_complete. See above.
 *         CHTTP1_USER    That on_message_complete call reported an error.
 *                        The parser is left dead, exactly as when
 *                        chttp1_parser_execute() reports an error from the
 *                        same callback, so a caller that treats every value
 *                        other than CHTTP1_PAUSED as "this EOF didn't
 *                        complete a message" needs no separate case for it.
 *         CHTTP1_ERROR   The EOF came in the middle of the message, which is
 *                        truncated and invalid.
 */
chttp1_errno_t chttp1_parser_finish(chttp1_parser_t *parser);

/**
 * @brief Returns how many bytes the completed message took from the buffer
 *        of the last chttp1_parser_execute call.
 *
 * Only meaningful right after chttp1_parser_execute returns CHTTP1_PAUSED.
 * It's a plain byte count rather than a raw pointer into the caller's
 * buffer, so the caller never has to subtract one pointer from another. The
 * bytes from this count to the end of that same buffer are trailing data
 * the parser didn't consume and the caller never asked for. See contract 2
 * of chttp1_parser_execute.
 */
size_t chttp1_parser_consumed(const chttp1_parser_t *parser);

/**
 * @brief Reports whether parser has reached a clean message boundary with
 *        the whole message parsed, which is how chttp1_parser_execute() and
 *        chttp1_parser_finish() leave it after returning CHTTP1_PAUSED.
 *
 * A thin, stable accessor over parser->state. A caller that drives one
 * request across a CHTTP1_HEADERS_ONLY divert continues
 * chttp1_parser_execute() from another thread, possibly several calls
 * later, and this gives it a documented way to ask "is this request fully
 * parsed now" without reading parser->state directly. The values of
 * chttp1__state_t aren't part of this module's documented contract and may
 * change.
 */
bool chttp1_parser_message_complete(const chttp1_parser_t *parser);

/**
 * @brief Reports whether the connection this message arrived on can be used
 *        for a later request, as RFC 7230 SS6.3 says.
 *
 * HTTP/1.1 keeps the connection alive by default, while HTTP/1.0 and earlier
 * versions DON'T unless a Connection: keep-alive token arrives. A
 * Connection: close token ends the connection for every version, even next
 * to a keep-alive token on the same line or on another Connection line. An
 * HTTP/1.0 message with Transfer-Encoding never keeps the connection alive
 * either, because RFC 9112 SS6.1 treats its framing as faulty; request mode
 * refuses such a request outright, and for a response the message is
 * processed and the connection closed afterwards. One more rule applies to
 * every version: a body that only the connection close can delimit (no
 * Content-Length and no chunked Transfer-Encoding) never qualifies for
 * keep-alive whatever a Connection header says, because finding the end of
 * that body already took the end of the connection.
 *
 * Only meaningful once the message has completed, that is after
 * chttp1_parser_execute or chttp1_parser_finish has returned CHTTP1_PAUSED.
 */
bool chttp1_should_keep_alive(const chttp1_parser_t *parser);

/**
 * @brief Makes chttp1_should_keep_alive() report false for the message this
 *        parser holds, exactly like a Connection: close token does.
 *
 * A caller that lost bytes it had already taken off the connection, such as
 * the start of a pipelined next message it couldn't keep, calls this so the
 * connection closes after the current message instead of waiting for data
 * the peer already sent. The next chttp1_parser_init() clears it along with
 * every other flag. It does nothing for a NULL parser.
 */
void chttp1_parser_force_close(chttp1_parser_t *parser);

/**
 * @brief Reports whether the headers read hold "Expect: 100-continue", the
 *        only expectation value RFC 7231 SS5.1.1 defines. It does NOT detect
 *        any other Expect value, nor that value next to other tokens; the
 *        caller rejects such a value itself, with a 417 Expectation Failed
 *        for example. An Expect value the caller doesn't know needs that
 *        explicit handling anyway, whatever this parser does.
 *
 * The report is false for an HTTP/1.0 message whatever it carries: RFC 9110
 * SS10.1.1 has a server ignore 100-continue from an HTTP/1.0 client, which
 * sends its body without waiting for an interim response.
 *
 * Meaningful once settings->on_headers_complete has fired, when the parser
 * holds the whole header set. This parser does no I/O of its own, so it
 * does NOT send an interim "HTTP/1.1 100 Continue\r\n\r\n" response by
 * itself. A caller that wants to honor the expectation writes that line to
 * the raw connection itself, usually in its own on_headers_complete callback
 * or right after it, and before feeding more body bytes to
 * chttp1_parser_execute. A caller that wants to reject the request writes a
 * final status response instead and never reads the body.
 *
 * This is the detection half of Expect: 100-continue for request mode, the
 * server side. A response-mode parser has no counterpart spanning the
 * interim and the final response: it reports a "100 Continue" as the
 * ordinary complete, bodiless message that every other 1xx status produces.
 * The client then drives the second message on the same connection through
 * a NEW chttp1_parser_t instance, discarding each interim response as it
 * completes. That keeps one parser instance bound to exactly one message,
 * which is what lets the same value type serve both modes. A caller sending
 * Expect: 100-continue therefore sequences the interim and the final
 * response itself, and chttpclient.c is where that code lives.
 */
bool chttp1_expects_continue(const chttp1_parser_t *parser);

/**
 * @brief Reports whether the header block's merged Transfer-Encoding list
 *        names a coding other than "chunked".
 *
 * This parser decodes chunked framing and no other coding. For a request, a
 * list such as "gzip, chunked" passes the framing rules, and the body that
 * on_body delivers is still gzip-encoded. A server that doesn't decode that
 * coding itself answers 501 Not Implemented (RFC 9112 SS6.1). A list that
 * doesn't end in "chunked" never reaches on_headers_complete in request
 * mode, since its length can't be determined and the parser refuses it
 * first. Meaningful once on_headers_complete has fired.
 */
bool chttp1_has_other_transfer_coding(const chttp1_parser_t *parser);

/**
 * @brief Reports whether the headers read set up Content-Length framing, as
 *        opposed to a chunked Transfer-Encoding or no body at all.
 *
 * Meaningful once settings->on_headers_complete has fired, and by
 * construction it excludes chunked framing: a message that declares both a
 * Content-Length and a chunked Transfer-Encoding is rejected before its
 * headers can complete (RFC 7230 SS3.3.3), so this report and chunked
 * framing can never both be true for one message. Both modes also reject a
 * Content-Length next to a Transfer-Encoding whose final coding is NOT
 * chunked, so a message that reaches headers-complete with Content-Length
 * framing carries no Transfer-Encoding at all. This lets a caller look at
 * the declared body size, which chttp1_declared_content_length() gives,
 * before deciding whether to divert the request to a worker thread at all,
 * without reading parser->flags directly, a field private to
 * chttp1_parser.c.
 */
bool chttp1_has_content_length(const chttp1_parser_t *parser);

/**
 * @brief The declared Content-Length value, once chttp1_has_content_length()
 *        has become true.
 *
 * Only meaningful when chttp1_has_content_length() returns true, because the
 * field behind it does a second job: once the parse of a chunked body has
 * started, that field counts the bytes left in the CURRENT chunk. So for a
 * chunked message the call gives a value that means something else than the
 * total the message declared, and so does a call made after the parse of a
 * Content-Length-framed body has started consuming bytes. Returns 0 for a
 * NULL parser.
 *
 * chttpserver.c is one caller. It uses this to reject a request whose
 * declared Content-Length already exceeds its own configured body size
 * limit right at headers-complete time, instead of reacting byte by byte
 * while reading the body (see the doc comment of
 * chttp1_settings_t.on_body). Without it, a peer that declares an oversized
 * Content-Length and never sends the body would hold a reader until a read
 * timeout fires, since a limit that counts bytes catches nothing if the
 * bytes never come.
 */
uint64_t chttp1_declared_content_length(const chttp1_parser_t *parser);

/**
 * @brief Reports whether the last CHTTP1_ERROR was a chunk-size line above
 *        max_chunk_size_override.
 *
 * Only meaningful right after chttp1_parser_execute() or
 * chttp1_parser_finish() returns CHTTP1_ERROR, and false in every other
 * case, including every other rejection reason CHTTP1_ERROR covers, such as
 * a malformed chunk-size token or a line that's too long. For a caller that
 * sets max_chunk_size_override, it tells "this one chunk declared more than
 * the configured limit" apart from every other parse failure, so the caller
 * can map it to the same "body too large" outcome a max_body_size violation
 * already gives (for chttpserver.c, a 413 and ccol_msg_too_large) instead of
 * reporting a generic aborted transfer.
 */
bool chttp1_chunk_size_limit_exceeded(const chttp1_parser_t *parser);

/**
 * @brief Gives the number of body bytes the current message's framing still
 *        guarantees will come, or 0 when it guarantees none right now.
 *
 * For a Content-Length-framed body the answer is the part of the declared
 * length not consumed yet. For a chunked body it's what the current chunk
 * still needs while the parser is inside that chunk's data, and 0 on a
 * chunk-size line, on the CRLF after a chunk and in the trailers, where the
 * next byte may be the last one. It's 0 in every other state.
 *
 * A caller that waits for readiness can ask the kernel to wake it only once
 * this many bytes are queued (SO_RCVLOWAT), because a peer that follows its
 * own framing sends at least that many before it can expect an answer.
 */
uint64_t chttp1_body_bytes_still_expected(const chttp1_parser_t *parser);

/**
 * @brief Reports whether the peer may still have bytes of the current
 *        message to send that the parser hasn't consumed.
 *
 * It's false once the message is complete, and false for a message whose
 * header block ended without body framing (no Content-Length above 0 and no
 * chunked coding), even when the caller refused it at on_headers_complete.
 * It's true for a message whose body is unfinished, whether the parse is
 * paused, diverted or ended by an error inside the body, and true before the
 * end of the header block or after an error inside it, where the framing
 * isn't known. Returns false for a NULL parser.
 *
 * When a server closes a connection while the peer is still sending, the
 * kernel answers with a reset, and that can destroy a response the peer
 * hasn't read yet. chttpserver.c asks this question before such a close and
 * does a lingering close when the answer is true.
 */
bool chttp1_request_bytes_may_remain(const chttp1_parser_t *parser);

/* ========================================================================== */
/*                    WORKER-PULL BODY/RESPONSE STREAMING                     */
/* ========================================================================== */

/**
 * @brief A small helper, independent of any reactor, that lets a worker
 *        thread read raw bytes off a socket and write them. Every call has
 *        an absolute timeout. It also supports "carry-over" bytes, which a
 *        reactor thread already read before handing the connection off.
 *
 * chttpserver.c uses it internally where a worker thread drives reading the
 * body. It works on a raw file descriptor and depends on no reactor and no
 * ccol_event_loop at all. Unlike a request parser, which allocates nothing,
 * chttp1_stream_t may allocate a heap copy of the carry-over bytes passed to
 * chttp1_stream_prepare(). Its lifetime doesn't need that same constraint,
 * since there's one instance per diverted request and the code doesn't embed
 * it everywhere.
 *
 * The typical use: a reactor thread parses the headers with
 * chttp1_parser_execute() until it returns CHTTP1_HEADERS_ONLY, then hands
 * the fd to a worker thread along with the buffer position
 * chttp1_parser_consumed() gives, and the bytes from that position to the
 * end of that buffer are the carry-over. The worker calls
 * chttp1_stream_prepare() once with that carry-over and then calls
 * chttp1_stream_read() over and over; it drains the carry-over first and
 * then reads the raw fd. The worker feeds each chunk to
 * chttp1_parser_execute(), which keeps the SAME parser instance moving
 * through the body, the chunks and the trailers until the parser returns
 * CHTTP1_PAUSED. chttp1_stream_write() is the matching primitive for the
 * phase where the worker owns writing the response.
 */
typedef struct chttp1_stream {
  int fd;

  /* An opaque ctls_conn_t*, or NULL for a plain-text connection. It's void*
   * rather than ctls_conn_t* so this header doesn't need to #include ctls.h;
   * chttp1_parser.c casts the pointer internally. chttp1_stream_prepare_tls()
   * sets it and chttp1_stream_prepare() doesn't; see that function's doc
   * comment. With it set, chttp1_stream_read() and chttp1_stream_write() call
   * ctls_conn_read() and ctls_conn_write() instead of raw read(2) and
   * write(2). fd is still used for the poll(2) readiness waits, because
   * ctls_conn_t doesn't depend on a reactor on purpose and so has no poll
   * primitive of its own. */
  void *tls;

  /* The carry-over bytes: a heap copy of the bytes the caller gave to
   * chttp1_stream_prepare, drained through carry_pos before any real read(2)
   * touches fd. The pointer is NULL and the length 0 if the caller gave
   * chttp1_stream_prepare no leftover bytes. On a TLS connection these are
   * application bytes that ctls_conn_read() already decrypted while the
   * reactor thread was parsing the headers, not raw wire bytes. They mean the
   * same as in the plain-text case; they're just already past the TLS
   * layer. */
  char *carry;
  size_t carry_len;
  size_t carry_pos;

  /* The allocator every buffer of this stream comes from and goes back to.
   * chttp1_stream_prepare() and chttp1_stream_prepare_tls() set it, and NULL
   * picks the default malloc and free through _ccol_mem_alloc(). The
   * carry-over buffer and the buffer chttp1_stream_take_leftover() hands out
   * both come from it.
   *
   * Without this field the stream would call malloc() and free() directly,
   * and an embedder that installs its own allocator with
   * chttpsvr_set_engine_mem_mgmt_procs() would find the body bytes of every
   * diverted request outside that allocator, with nothing telling it so.
   * Every other buffer a request owns already comes from its server's
   * allocator. */
  ccol_memmgmt_procs_t *mp;

  int last_errno; /* errno of the last failed read or write, or 0 */
  bool timed_out; /* true if the last read or write failed because its
                   * deadline passed, a poll(2) that returned 0. That's not
                   * a real I/O error, and last_errno means nothing for that
                   * call. */
  bool prepared;
  bool released;
  /* When true, chttp1_stream_write() never waits: it acts as if every call
   * passed a timeout_ms of 0, and it reports a socket that can't take more
   * bytes right now as a timeout (chttp1_stream_timed_out() gives true). A
   * caller that must not block a thread on a slow reader sets it and waits
   * for writability through its own reactor instead. The prepare functions
   * clear it. It has no effect on chttp1_stream_read(), whose non-blocking
   * form is an explicit timeout_ms of 0. */
  bool write_nonblocking;
  /* When true, the caller promises fd is non-blocking, and every read and
   * write tries the I/O first and only polls after EWOULDBLOCK or EAGAIN, so
   * a read or write that would have succeeded costs no poll(2). The prepare
   * functions clear it: a read on a plaintext stream over a blocking fd must
   * poll first, or the attempt itself blocks and ignores timeout_ms. A
   * plaintext write never blocks in the attempt (MSG_DONTWAIT) either way. */
  bool fd_nonblocking;
} chttp1_stream_t;

/**
 * @brief Prepares stream for the reads and writes a worker thread pulls from
 *        fd.
 *
 * leftover and leftover_len describe bytes someone else, usually a reactor
 * thread that parsed the headers, has already read off fd. Logically they
 * come BEFORE any further byte a read of fd gives, so the stream must
 * deliver them first. They're copied internally into a heap buffer the
 * stream owns, so the caller can reuse or free its own buffer as soon as this
 * returns. Pass a leftover_len of 0 when there are no such bytes, and
 * leftover may then be NULL.
 *
 * fd must be a connected stream socket. A plaintext write goes out with
 * send(2) or sendmsg(2) and MSG_NOSIGNAL, so writing to a peer that has
 * closed or reset fails with EPIPE or ECONNRESET and never raises SIGPIPE,
 * whatever the process's disposition for that signal. A descriptor that
 * isn't a socket fails every write with ENOTSOCK.
 *
 * Call it synchronously before any other thread starts touching fd at the
 * same time, in other words BEFORE handing fd to whatever wakes the worker
 * thread, not after.
 *
 * @param mp  The allocator for every buffer this stream owns, or NULL for
 *            the default malloc and free. The stream keeps the pointer, so
 *            it must stay valid until chttp1_stream_release(). The same
 *            allocator frees the buffer chttp1_stream_take_leftover() hands
 *            out, and the CALLER of that function must use it too.
 * @return true on success, and a leftover_len of 0 counts as success. false
 *         only after an allocation failure, which needs a leftover_len above
 *         0 and a failed copy. After a failure the stream is all zero bytes,
 *         which is safe: calling chttp1_stream_release() on it is still safe,
 *         though not needed, and does no harm.
 */
bool chttp1_stream_prepare(chttp1_stream_t *stream, int fd,
                           const char *leftover, size_t leftover_len,
                           ccol_memmgmt_procs_t *mp);

/**
 * @brief Prepares stream for the reads and writes a worker thread pulls from
 *        a TLS connection.
 *
 * Same as chttp1_stream_prepare(), with the same contract for leftover and
 * leftover_len; those bytes are application bytes ctls_conn_read() already
 * decrypted while the reactor thread was parsing the headers, not raw wire
 * bytes. One thing differs: chttp1_stream_read() and chttp1_stream_write()
 * then call ctls_conn_read() and ctls_conn_write() on tls_conn instead of
 * raw read(2) and send(2) on fd. The stream still needs fd, but only for the
 * poll(2) readiness waits, since ctls_conn_t doesn't depend on a reactor on
 * purpose and has no poll primitive of its own.
 *
 * @param tls_conn  The ctls_conn_t (from ctls.h) that drives this
 *                  connection's encrypted I/O. Its handshake must already be
 *                  complete, meaning ctls_conn_handshake_step() returned
 *                  CTLS_HANDSHAKE_DONE. Ownership isn't taken: the caller
 *                  still destroys it with ctls_conn_destroy(), after
 *                  chttp1_stream_release().
 * @return The same as chttp1_stream_prepare().
 */
bool chttp1_stream_prepare_tls(chttp1_stream_t *stream, int fd, void *tls_conn,
                               const char *leftover, size_t leftover_len,
                               ccol_memmgmt_procs_t *mp);

/**
 * @brief Reads up to buflen bytes into buf.
 *
 * Whatever carry-over remains is drained first, and while carry-over remains
 * fd isn't touched at all. Once the carry-over is empty, the order of the
 * read and the wait depends on the stream:
 *
 * - A TLS stream, prepared by chttp1_stream_prepare_tls(), tries one
 *   ctls_conn_read() BEFORE looking at poll(2) or timeout_ms, because
 *   OpenSSL may already hold decrypted application bytes in its own internal
 *   buffer, whether or not the raw fd still has anything for the kernel to
 *   give, so checking fd's readiness first would hide those bytes.
 * - A plaintext stream with fd_nonblocking set also tries one read(2) first.
 * - A plaintext stream without fd_nonblocking waits in poll(2) for at most
 *   timeout_ms FIRST and only makes one read(2) once poll(2) reports fd
 *   readable, because its fd may be blocking, and a read(2) on an empty
 *   blocking fd would wait past timeout_ms. A poll(2) that reports nothing
 *   ends the call as a timeout without any read.
 *
 * In the two try-first forms, only after an attempt reports EWOULDBLOCK or
 * EAGAIN does it block in poll(2), waiting for fd for whatever is left of
 * timeout_ms, and then try again. For a TLS stream one readiness event
 * doesn't guarantee application bytes right away: a partial TLS record can
 * need another read, and so can a renegotiation or key-update message that
 * OpenSSL consumes internally. Then ctls_conn_read() reports EWOULDBLOCK or
 * EAGAIN, and this function polls and retries without telling the caller,
 * still within the same total timeout_ms budget rather than a fresh budget
 * per retry.
 *
 * @param timeout_ms  A negative value blocks forever without a timeout,
 *                    matching poll(2)'s -1 convention. 0 returns at once
 *                    if fd isn't readable at that moment. Above 0 it's the
 *                    maximum number of milliseconds to wait across all
 *                    internal retries together. It's ignored while carry-over
 *                    bytes remain, because those always come back
 *                    immediately.
 * @return The number of bytes read, above 0; the carry-over alone may give
 *         all of them. 0 on a clean EOF, meaning a peer that closed its write
 *         side or a clean TLS close_notify. -1 on an error and on a timeout;
 *         chttp1_stream_timed_out() and chttp1_stream_last_error() tell the
 *         two apart.
 */
ssize_t chttp1_stream_read(chttp1_stream_t *stream, char *buf, size_t buflen,
                           int timeout_ms);

/**
 * @brief True while stream still holds carry-over bytes, which the next
 *        chttp1_stream_read() returns without touching the file descriptor.
 */
static inline bool chttp1_stream_has_carry(const chttp1_stream_t *stream) {
  return stream->carry_pos < stream->carry_len;
}

/**
 * @brief Writes up to len bytes from buf to stream's fd. A TLS stream
 *        encrypts them and writes them with ctls_conn_write(). If the fd
 *        isn't writable immediately it blocks in poll(2) for at most
 *        timeout_ms milliseconds, and a TLS stream retries within that same
 *        budget, the way chttp1_stream_read() documents. timeout_ms follows
 *        the same convention as in chttp1_stream_read().
 *
 * The phase where a worker owns writing the response needs this, because it
 * bounds how long a slowly reading peer can hold a worker thread while a
 * response is sent.
 *
 * @return The number of bytes written, which may be less than len, as with
 *         write(2) itself, so a caller that needs to write all of a larger
 *         buffer loops. -1 on an error and on a timeout.
 */
ssize_t chttp1_stream_write(chttp1_stream_t *stream, const char *buf,
                            size_t len, int timeout_ms);

/**
 * @brief The largest total chttp1_stream_writev2() copies into one buffer to
 *        send a head and a body as one TLS record, which is the largest
 *        plaintext one TLS record carries.
 */
#define CHTTP1_TLS_COALESCE_MAX 16384

/**
 * @brief Writes the bytes of a and then those of b, as one write where the
 *        transport allows it. Either part may be empty.
 *
 * With fd_nonblocking set it tries the write first, and only waits in
 * poll(2) for at most timeout_ms after the socket has reported it's full.
 * Without fd_nonblocking it waits in poll(2) for writability first, like
 * chttp1_stream_write(). A plaintext stream sends both parts with one
 * sendmsg(2), the same gather write as writev(2), with MSG_NOSIGNAL. A TLS
 * stream has no gather write: when the two parts together fit in
 * CHTTP1_TLS_COALESCE_MAX bytes it copies them into one buffer and sends
 * them as one record, otherwise it sends a with one write and then tries b
 * immediately, without waiting in between. write_nonblocking applies as for
 * chttp1_stream_write().
 *
 * A TLS write that reported "would block" must be retried with the same
 * bytes at the same stream position, and the next call with the unsent rest
 * of a and b always does that; the connection allows the retry from a
 * different buffer.
 *
 * @return The number of bytes written, counted across a and then b; it may
 *         be less than alen + blen. -1 on an error and on a timeout, like
 *         chttp1_stream_write().
 */
ssize_t chttp1_stream_writev2(chttp1_stream_t *stream, const char *a,
                              size_t alen, const char *b, size_t blen,
                              int timeout_ms);

/**
 * @brief True if the last chttp1_stream_read or chttp1_stream_write call
 *        returned -1 because its deadline passed, a poll(2) that returned 0.
 *        False if that call failed with a real I/O error.
 */
bool chttp1_stream_timed_out(const chttp1_stream_t *stream);

/**
 * @brief Returns the errno of the last chttp1_stream_read or
 *        chttp1_stream_write call that failed. 0 if the last call succeeded,
 *        timed out, or was satisfied by the carry-over alone. For a timeout,
 *        use chttp1_stream_timed_out() instead.
 */
int chttp1_stream_last_error(const chttp1_stream_t *stream);

/**
 * @brief Pushes buf and len back onto stream's carry-over, in front of any
 *        unconsumed bytes already there.
 *
 * For a caller that drives chttp1_parser_execute() over the output of
 * chttp1_stream_read(). execute() can report a message boundary
 * (CHTTP1_HEADERS_ONLY or CHTTP1_PAUSED) with a chttp1_parser_consumed()
 * below the number of bytes the caller just gave it. The trailing bytes are
 * already off the wire and belong to whatever comes next on this connection,
 * most often the next request of a pipeline, so the caller must not drop
 * them. After a push back here, one later call to
 * chttp1_stream_take_leftover(), made once this message has been read
 * completely, gets them back the same way whatever their source was: a real
 * read(2) or ctls_conn_read() call, or part of the carry-over
 * chttp1_stream_prepare() gave the stream that nothing ever needed (that
 * function's doc comment covers the second case).
 *
 * buf is copied, so the caller can reuse or free its own buffer as soon as
 * this returns.
 *
 * @return true on success, and a len of 0 is a safe success that does
 *         nothing. false only after an allocation failure; then any
 *         carry-over the stream holds stays exactly as it was, and this
 *         mechanism can't recover buf and len. That's a rare degradation
 *         nothing can avoid when memory runs out, not a crash and not a
 *         corruption.
 */
bool chttp1_stream_push_back_leftover(chttp1_stream_t *stream, const char *buf,
                                      size_t len);

/**
 * @brief Takes back what's left of stream's carry-over and hands ownership
 *        of those bytes to the caller. They're the bytes chttp1_stream_read
 *        never consumed, which may still be exactly what
 *        chttp1_stream_prepare() got, or what
 *        chttp1_stream_push_back_leftover() replaced them with or added.
 *
 * Call it exactly once, from the caller that drove reading this stream,
 * right before chttp1_stream_release(); otherwise that function silently
 * frees those bytes and nothing can get them back. A caller that skips it
 * loses the next request of a pipeline: that request's bytes are already
 * gone for good from the kernel's socket receive buffer, so they vanish the
 * moment the current request has been read, and the client waits for a
 * response that never comes. After this call stream's carry-over is empty,
 * exactly as if chttp1_stream_prepare() had got none.
 *
 * @param len_out  Set to the number of bytes returned. With a NULL return
 *                 it's set to 0 when there was nothing to take back, and to
 *                 the number of bytes that couldn't be taken back after an
 *                 allocation failure. A caller keeping the connection alive
 *                 must close it instead when it sees NULL with a length above
 *                 0, because the next request on it is lost.
 * @return A heap buffer the caller owns. It comes from the stream's
 *         allocator, the mp that chttp1_stream_prepare() or
 *         chttp1_stream_prepare_tls() got, and the caller must release it
 *         with _ccol_mem_free() and that same mp; a plain free() is wrong
 *         whenever mp is a custom allocator. _ccol_mem_free() accepts NULL,
 *         so the caller can release the result even when *len_out is 0.
 *         NULL when there was nothing left to take back, and after an
 *         allocation failure; *len_out tells the two apart. After such a
 *         failure the bytes can't be recovered, but the stream's state stays
 *         valid and chttp1_stream_release() still cleans it up normally.
 */
char *chttp1_stream_take_leftover(chttp1_stream_t *stream, size_t *len_out);

/**
 * @brief Frees stream's resources, which means its carry-over buffer if it
 *        has one. It does NOT close fd, because chttp1_stream_prepare() never
 *        hands ownership of the file descriptor itself to this struct.
 *
 * Call it exactly once, after reading is completely done: at an EOF, after
 * an error, or when a handler stopped its reads and writes early. Calling it
 * on an all-zero stream that nothing prepared is safe, and so is a second
 * call, which does nothing.
 */
void chttp1_stream_release(chttp1_stream_t *stream);
