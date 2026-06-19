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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* ssize_t, for chttp1_stream_read()/_write() */

/**
 * @file chttp1_parser.h
 * @brief INTERNAL ONLY. A small, hand-written HTTP/1.1 request/response
 *        parser, used by chttpclient.c (response mode) and chttpserver.c
 *        (request mode).
 *
 * This is not a public collections module: it has no macros, no opaque
 * handle type, and is never included by chttp.h/chttpclient.h/chttpserver.h
 * or any other public header. src/chttpclient.c and src/chttpserver.c are
 * the only files meant to #include this header.
 *
 * This parser started as a purpose-built *response*-only parser covering
 * exactly the subset chttpclient.c needs. Request-mode parsing
 * (chttp1_parser_init_request(), settings->on_request_line,
 * CHTTP1_HEADERS_DIVERT_BODY/CHTTP1_HEADERS_ONLY, and the RFC 7230 SS3.3
 * request-specific body-framing rule) was added later, once chttpserver.c
 * needed its own parser, sharing the header/chunked/trailer/size-cap core
 * between both modes rather than duplicating it, exactly as
 * chttp1_parser_t's own internal state machine already made possible (every
 * state after the first line was already common to both grammars).
 *
 * Also added in that same increment: chttp1_stream_t and its
 * prepare/read/write/last_error/release functions, a small, separate,
 * reactor-agnostic worker-pull I/O helper (real read(2)/write(2)/poll(2)
 * syscalls) for reading/writing raw bytes off a file descriptor from a
 * worker thread; see its own section below. Unlike chttp1_parser_t itself,
 * this is a genuine, if small, expansion of this module's dependencies
 * beyond the C standard library (poll(2) plus raw socket read/write); the
 * parser proper remains pure computation with zero I/O and zero
 * allocation, exactly as before.
 *
 * ### Design: a plain, self-contained value type, no heap allocation
 *
 * chttp1_parser_t has zero owned resources: it is safe to embed by value
 * on the stack (as chttpclient.c's synchronous
 * _chttp_read_response does) or as a struct member (as chttpclient.c's
 * chttp_async_ctx_t does), and there is no destroy/free function; there is
 * nothing to release. This is possible because the internal line-
 * accumulation buffer used for the status line, header lines, and
 * chunk-size lines is a FIXED-size array (CHTTP1_MAX_LINE_LEN, defined
 * below) embedded directly in the struct, not a growable/realloc'd
 * buffer; this is also what enforces the new header-size cap described
 * below (a line that hasn't terminated by the time the fixed buffer fills
 * up is rejected as "line too long"). Body/chunk-data bytes are never
 * copied into this buffer at all; they are passed straight from the
 * caller's own read buffer to on_body, zero-copy, exactly like today.
 *
 * ### Header size protections
 *
 * Nothing previously enforced any limit on a single header line's length,
 * the number of headers, or their total size; a slow-drip
 * malicious or misbehaving server could force unbounded client memory
 * growth via one endless header line. This parser enforces three fixed
 * limits (CHTTP1_MAX_LINE_LEN, defined below; CHTTP1_MAX_HEADER_COUNT and
 * CHTTP1_MAX_TOTAL_HEADER_BYTES, both private to chttp1_parser.c since they
 * don't affect this struct's layout) that, combined, bound this. Trailer
 * headers (after a chunked body's terminating 0-length chunk) count against
 * the exact same header-count/total-bytes budget as regular headers;
 * there is no separate trailer allowance to bypass the cap through.
 *
 * ### Thread safety
 *
 * A chttp1_parser_t instance is not thread-safe (nothing here is meant to
 * be shared between threads); this matches how chttpclient.c already uses
 * it; one instance per in-flight hop, touched by whichever single thread
 * (or, in the async tier, whichever single reactor callback invocation) is
 * currently driving that hop. This remains true across a request-mode
 * CHTTP1_HEADERS_ONLY divert: exactly one thread touches the parser at any
 * given moment, it's just that WHICH thread that is may change once (from a
 * reactor thread to a worker thread) at the divert point; there is no
 * concurrent access, only a single-owner handoff, which is the caller's own
 * responsibility to sequence correctly (see CHTTP1_HEADERS_ONLY's own doc
 * comment). chttp1_stream_t instances follow the identical rule: one
 * instance handed off to exactly one worker thread at a time, never shared.
 */

/* ========================================================================== */
/*                         RESULT CODES                                      */
/* ========================================================================== */

/**
 * @brief Outcome of chttp1_parser_execute()/chttp1_parser_finish().
 *
 * Mirrors the exact three-way split chttpclient.c already branches on:
 * CHTTP1_OK means "keep reading, message not yet complete"; CHTTP1_PAUSED
 * means the message just completed (see the hard contract on
 * chttp1_parser_execute below); CHTTP1_USER means an application callback
 * (on_header / on_headers_complete / on_body / on_message_complete) itself
 * reported an error; CHTTP1_ERROR covers every parse failure (malformed
 * status line, invalid header, chunk-size overflow, oversized header,
 * etc.); this parser does not further subdivide CHTTP1_ERROR into
 * distinct per-cause codes, since chttpclient.c never needs to distinguish
 * a parse failure beyond "not OK/PAUSED/USER" anyway. A human-readable
 * reason string for the specific rejection is still available via
 * chttp1_parser_t.reason, for diagnostics/tests.
 */
typedef enum {
  CHTTP1_OK = 0,
  CHTTP1_PAUSED,
  CHTTP1_USER,
  CHTTP1_ERROR,
  /**
   * Request-mode only (see chttp1_parser_init_request()): headers are fully
   * parsed and validated, and settings->on_headers_complete returned
   * CHTTP1_HEADERS_DIVERT_BODY (see that callback's own doc comment) to
   * pause parsing here, before any body byte is consumed, rather than
   * continuing to parse body content within this same
   * chttp1_parser_execute() call. chttp1_parser_consumed() reports exactly
   * how many bytes of THIS call's buffer were the header block; anything
   * from there to len is unconsumed (likely the start of the body, or
   * pipelined bytes) and is the caller's to hand off however it likes (e.g.
   * as chttp1_stream_prepare()'s leftover argument, when diverting body
   * ingestion to a worker thread).
   *
   * Unlike CHTTP1_PAUSED, this is NOT a terminal outcome: the parser is
   * still mid-message (parser->state already correctly positioned at
   * whatever body-framing state applies; content-length, chunked, or
   * already complete if the request turned out to have no body at all,
   * in which case this code is never returned; see the callback's own doc
   * comment). Feeding more bytes via chttp1_parser_execute(); from
   * whichever thread now owns the connection; resumes exactly where this
   * call left off, and is expected, not a contract violation the way
   * feeding more bytes after CHTTP1_PAUSED would be.
   */
  CHTTP1_HEADERS_ONLY
} chttp1_errno_t;

/**
 * @brief Which grammar chttp1_parser_t parses its first line as. Set by
 *        chttp1_parser_init() (CHTTP1_PARSE_RESPONSE) or
 *        chttp1_parser_init_request() (CHTTP1_PARSE_REQUEST); read-only
 *        afterward.
 */
typedef enum {
  CHTTP1_PARSE_RESPONSE = 0,
  CHTTP1_PARSE_REQUEST
} chttp1_parser_type_t;

/**
 * @brief settings->on_headers_complete return-value constants.
 *
 * CHTTP1_HEADERS_HAS_BODY and CHTTP1_HEADERS_NO_BODY apply in both parsing
 * modes, exactly as before (this parser used to document these as bare 0/1
 * without names; existing response-mode callers returning literal 0/1
 * continue to work unchanged). CHTTP1_HEADERS_DIVERT_BODY is new and valid
 * ONLY in request mode (see chttp1_settings_t.on_headers_complete's own doc
 * comment for its full contract); returning it in response mode is treated
 * as an invalid hint (CHTTP1_USER), since chttpclient.c has no worker-thread
 * diversion concept to hand off to.
 */
#define CHTTP1_HEADERS_HAS_BODY 0
#define CHTTP1_HEADERS_NO_BODY 1
#define CHTTP1_HEADERS_DIVERT_BODY 2

/* ========================================================================== */
/*                         INTERNAL STATE (opaque to chttpclient.c)          */
/* ========================================================================== */

/* Not part of this module's public-to-chttpclient.c contract; declared here
 * only because chttp1_parser_t must be a complete, stack-embeddable type. */
typedef enum {
  CHTTP1_ST_FIRST_LINE = 0, /* status line (response mode) or request line
                             * (request mode); which grammar applies is
                             * decided by parser->type, not by two separate
                             * states, since every other state after this one
                             * is already fully shared between both modes. */
  CHTTP1_ST_HEADERS,
  CHTTP1_ST_BODY_CONTENT_LENGTH,
  CHTTP1_ST_BODY_CHUNK_SIZE,
  CHTTP1_ST_BODY_CHUNK_DATA,
  CHTTP1_ST_BODY_CHUNK_CRLF,
  CHTTP1_ST_BODY_CHUNK_TRAILERS,
  CHTTP1_ST_BODY_EOF,
  CHTTP1_ST_MESSAGE_DONE,
  CHTTP1_ST_DEAD /* a hard parse error already occurred; execute() is a no-op */
} chttp1__state_t;

/* A three-way finish-state split (clean boundary / EOF-delimited
 * completion / truncated mid-message), which is what makes
 * chttp1_parser_finish() correct without the call site needing to know
 * which body-framing mode was in effect. */
typedef enum {
  CHTTP1_FINISH_SAFE = 0,     /* clean message boundary; nothing pending */
  CHTTP1_FINISH_SAFE_WITH_CB, /* EOF-delimited body: EOF itself completes the
                                 message */
  CHTTP1_FINISH_UNSAFE        /* EOF arrived mid-message: truncated/invalid */
} chttp1__finish_state_t;

/**
 * @brief Fixed size of the internal status-line/header-line/chunk-size-line
 *        accumulation buffer (chttp1_parser_t._line_buf below).
 *
 * This single constant does double duty as both the buffer's size AND the
 * "header line too long" rejection threshold (see the file-level doc
 * comment): a line that has not terminated by the time this many bytes have
 * accumulated is rejected outright, so there is no separate "cap" concept
 * to keep in sync with the buffer's actual size. It must live here, not in
 * chttp1_parser.c, because it determines chttp1_parser_t's layout (the
 * struct must be a complete type for chttpclient.c to embed it by value).
 * 8192 matches the read-buffer size chttpclient.c already hard-codes at
 * both of its socket-read call sites.
 *
 * The other two new limits (max header count, max cumulative header bytes)
 * do not affect this struct's layout; they are plain running counts
 * compared against constants private to chttp1_parser.c.
 */
#define CHTTP1_MAX_LINE_LEN 8192

typedef struct chttp1_parser chttp1_parser_t;

/**
 * @brief Callback settings: exactly the 4 callbacks chttpclient.c needs:
 *        on_header (a header line is always assembled whole, internally,
 *        before this fires, so there is no fragment-reassembly protocol for
 *        the caller to implement), on_headers_complete, on_body, and
 *        on_message_complete.
 *
 * Every callback is NULL-checked before being invoked, even though
 * chttpclient.c always wires all four in practice.
 */
typedef struct {
  /**
   * Fired once per complete header line (and once per trailer line, after a
   * chunked body's terminating 0-length chunk; there is no regular-vs-
   * trailer distinction at this callback, matching chttpclient.c's own
   * existing behavior of inserting both into the same headers map).
   *
   * name/value point into this parser's own internal line buffer and are
   * valid ONLY for the duration of this call; a callback that needs to keep
   * them must copy them before returning. name is exactly as received on
   * the wire (this parser does not lower-case it); value has already had
   * leading/trailing optional whitespace (SP/HTAB) trimmed.
   *
   * Return 0 on success, or a nonzero value to abort the parse with
   * CHTTP1_USER (chttp1_parser_execute's caller sees that value only
   * indirectly, via CHTTP1_USER; the raw return value itself is not
   * otherwise surfaced).
   */
  int (*on_header)(chttp1_parser_t *p, const char *name, size_t name_len,
                   const char *value, size_t value_len);

  /**
   * Request mode only (chttp1_parser_init_request()); never fired in
   * response mode. Fired once, after the request line is fully parsed and
   * validated, before any header line. p->http_major/http_minor are already
   * set.
   *
   * method/target point into this parser's own internal line buffer and are
   * valid ONLY for the duration of this call, exactly like on_header's
   * name/value; a callback that needs to keep them must copy them before
   * returning. Neither is validated beyond basic grammar (method is a
   * tchar-only token; target is any non-empty run of bytes containing no
   * control characters); this parser does not distinguish origin-form
   * from absolute-form/authority-form/asterisk-form targets, or reject an
   * unrecognized method name; that is the caller's own routing concern
   * (e.g. an unsupported method surfaces as an ordinary 405, not a parse
   * error).
   *
   * Return 0 on success, or nonzero to abort the parse with CHTTP1_USER.
   */
  int (*on_request_line)(chttp1_parser_t *p, const char *method,
                         size_t method_len, const char *target,
                         size_t target_len);

  /**
   * Fired once, after the blank line ending the header block.
   * In response mode, p->status_code is already set.
   *
   * Return CHTTP1_HEADERS_HAS_BODY (0) if this message has a body (the
   * normal case), CHTTP1_HEADERS_NO_BODY (1) if the caller already knows
   * (out of band, e.g. because the request method was HEAD) that no body
   * follows regardless of any Content-Length header present,
   * CHTTP1_HEADERS_DIVERT_BODY (2,
   * request mode only) to pause parsing right here instead of continuing
   * into body content within this same chttp1_parser_execute() call (see
   * that function's own CHTTP1_HEADERS_ONLY doc comment for the full
   * contract; this is what lets a caller route the request, based on
   * headers alone, before deciding whether/how to read its body, e.g. to
   * hand body ingestion off to a worker thread), or any OTHER value
   * (including a negative one) to abort the parse with CHTTP1_USER; this
   * split is deliberate, not merely tolerated: a real on_headers_complete
   * implementation needs a genuine error-reporting path here (e.g. an
   * allocation failure while duplicating a Location header during
   * redirect detection, on the response side), so anything outside
   * {CHTTP1_HEADERS_HAS_BODY, CHTTP1_HEADERS_NO_BODY,
   * CHTTP1_HEADERS_DIVERT_BODY-in-request-mode} is always treated as a
   * reported error, never silently coerced into "no body".
   *
   * CHTTP1_HEADERS_DIVERT_BODY is downgraded to ordinary has-body behavior
   * (i.e. treated the same as CHTTP1_HEADERS_HAS_BODY, continuing to parse
   * body content in this same call) whenever there turns out to be nothing
   * meaningful to divert: a request with neither Content-Length nor chunked
   * Transfer-Encoding has no body at all regardless of this return value
   * (see CHTTP1_HEADERS_ONLY's own doc comment), and a request with an
   * explicit "Content-Length: 0" completes immediately for the same reason
   *; there is no point pausing to divert an empty body to a worker
   * thread. A chunked body, even one whose very first chunk turns out to
   * be the empty terminating chunk, is always genuinely diverted, since
   * that can only be discovered by reading the first chunk-size line,
   * which has not happened yet at this callback's own call time.
   */
  int (*on_headers_complete)(chttp1_parser_t *p);

  /**
   * Fired zero or more times with body bytes as they are parsed out of
   * Content-Length-delimited or chunked framing. at points directly into
   * the buffer passed to chttp1_parser_execute (NOT into this parser's own
   * line buffer; body bytes are never copied internally) and is valid
   * only for the duration of this call.
   *
   * Return 0 on success, or nonzero to abort with CHTTP1_USER.
   */
  int (*on_body)(chttp1_parser_t *p, const char *at, size_t len);

  /**
   * Fired exactly once, when the message (headers + whatever body framing
   * applies) is fully parsed. This parser already knows independently that
   * it has just reached a message boundary; it drives its own explicit
   * state machine rather
   * than relying on a callback's return value to learn that; so this
   * follows the same plain convention as the other three callbacks: return
   * 0 on success, or nonzero to abort with CHTTP1_USER. chttp1_parser_execute
   * and chttp1_parser_finish report the pause itself via their own
   * CHTTP1_PAUSED return value regardless of what this callback returns
   * (beyond that zero/nonzero distinction).
   */
  int (*on_message_complete)(chttp1_parser_t *p);
} chttp1_settings_t;

/**
 * @brief The parser itself. Fully self-contained (no owned resources; see
 *        the file-level doc comment); safe to embed by value on the stack or
 *        as a struct member. There is no destroy/free function.
 */
struct chttp1_parser {
  /** Opaque to this parser; set once after chttp1_parser_init and read back
   * in every callback to recover the caller's own context. */
  void *data;

  /** Set once, in on_headers_complete, from the parsed status line.
   * Response mode only; always 0 in request mode. */
  int status_code;

  /** Set once by chttp1_parser_init()/chttp1_parser_init_request() and
   * read-only afterward; decides whether CHTTP1_ST_FIRST_LINE parses a
   * status line or a request line, and gates request-mode-only behavior
   * (on_request_line firing, CHTTP1_HEADERS_DIVERT_BODY being accepted from
   * on_headers_complete, the RFC 7230 SS3.3 "no framing means no body, not
   * EOF-delimited" rule). */
  chttp1_parser_type_t type;

  /** Diagnostic only: a static string describing the most recent rejection
   * (e.g. "Duplicate Content-Length"), or NULL. Not consumed by
   * chttpclient.c's own ccol_retval_t-based error reporting today (every
   * CHTTP1_ERROR collapses to the same ccol_http_transfer_aborted regardless
   * of reason); provided for logs and for this module's own test suite to
   * assert specific rejection reasons. */
  const char *reason;

  /* Everything below is internal state; chttpclient.c does not read or
   * write any of it directly. */
  chttp1__state_t state;
  chttp1__finish_state_t finish_state;
  uint8_t http_major;
  uint8_t http_minor;
  uint16_t flags;
  uint64_t content_length; /* remaining bytes for CHTTP1_ST_BODY_CONTENT_LENGTH,
                            * or remaining bytes in the CURRENT chunk for
                            * CHTTP1_ST_BODY_CHUNK_DATA; reused for both. */
  bool is_trailer_section; /* true once parsing trailers after a chunked
                            * body's terminating 0-length chunk; on_header's
                            * header-vs-trailer distinction (none) is the
                            * same either way, this only affects which state
                            * to return to after the blank line. */

  size_t header_count;
  size_t total_header_bytes;

  /** Optional per-parser override of the built-in CHTTP1_MAX_HEADER_COUNT /
   * CHTTP1_MAX_TOTAL_HEADER_BYTES caps (both private to chttp1_parser.c).
   * 0 (the default after chttp1_parser_init()/chttp1_parser_init_request())
   * means "use the built-in default"; set either field to a positive value,
   * any time before feeding the first byte, to override it for this parser
   * instance. Added for chttpserver's own configurable
   * chttpsvr_config_t.max_header_bytes; chttpclient has no equivalent
   * public knob and simply never sets these, getting the original built-in
   * caps unchanged. */
  size_t max_header_count_override;
  size_t max_total_header_bytes_override;

  /* Fixed-size accumulation buffer for the status line, each header/trailer
   * line, and each chunk-size line; never used for body/chunk-data bytes,
   * which are passed straight from the caller's own buffer to on_body. A
   * partial line spanning two chttp1_parser_execute calls accumulates here;
   * see CHTTP1_MAX_LINE_LEN's own comment for why this is fixed-size rather
   * than growable. */
  char line_buf[CHTTP1_MAX_LINE_LEN];
  size_t line_len;

  size_t consumed; /* backs chttp1_parser_consumed(); only meaningful
                    * immediately after chttp1_parser_execute returns
                    * CHTTP1_PAUSED; see that function's doc comment. */

  const chttp1_settings_t *settings;
};

/* ========================================================================== */
/*                         LIFECYCLE                                          */
/* ========================================================================== */

/** @brief Zero-initialise a chttp1_settings_t (all callbacks NULL). */
void chttp1_settings_init(chttp1_settings_t *settings);

/**
 * @brief Initialise parser for a new response message. Void and infallible
 *       ; there is no allocation to fail.
 *
 * settings must outlive parser.
 *
 * There is no chttp1_parser_reset(): chttpclient.c never reuses a parser
 * instance across hops (every hop, in both the sync and async tiers, always
 * constructs a fresh one), so this parser has no in-place-reuse contract
 * either; construct a fresh instance (or re-run chttp1_parser_init) per
 * message.
 */
void chttp1_parser_init(chttp1_parser_t *parser,
                        const chttp1_settings_t *settings);

/**
 * @brief Initialise parser for a new REQUEST message (parser->type becomes
 *        CHTTP1_PARSE_REQUEST). Void and infallible, same contract as
 *        chttp1_parser_init() otherwise.
 *
 * Added for the server side: parses "method SP request-target SP
 * HTTP-version CRLF" instead of a status line, fires settings->
 * on_request_line once before any header, applies RFC 7230 SS3.3's request-
 * specific body-framing rule (neither Content-Length nor chunked means no
 * body at all, never the response-only EOF-delimited framing mode), and
 * accepts CHTTP1_HEADERS_DIVERT_BODY from on_headers_complete.
 */
void chttp1_parser_init_request(chttp1_parser_t *parser,
                                const chttp1_settings_t *settings);

/* ========================================================================== */
/*                         PARSING                                           */
/* ========================================================================== */

/**
 * @brief Feed len bytes of data to the parser.
 *
 * Hard contracts (both load-bearing for chttpclient.c's read loop, which
 * calls this once per socket read):
 *
 * 1. This function ALWAYS consumes the entire input buffer in the
 *    CHTTP1_OK case (buffering a partial line, or continuing a partial
 *    body/chunk byte-count, internally across calls as needed). It never
 *    returns CHTTP1_OK with unconsumed bytes remaining. Only CHTTP1_PAUSED
 *    leaves a well-defined "consumed" position short of len (see
 *    chttp1_parser_consumed()).
 *
 * 2. Once this function returns CHTTP1_PAUSED, no further bytes must be fed
 *    to this parser instance. Bytes from chttp1_parser_consumed() to len in
 *    the buffer that triggered the pause are unconsumed and are the
 *    caller's to discard; this parser has no pipelining support
 *    (chttpclient.c never feeds it again after CHTTP1_PAUSED).
 *
 * 3. CHTTP1_HEADERS_ONLY (request mode only) is NOT covered by contract #2
 *    above: it is not a terminal outcome, and feeding more bytes afterward
 *    (via a further chttp1_parser_execute call, from whichever thread now
 *    owns the connection) is expected, not a violation; see
 *    CHTTP1_HEADERS_ONLY's own doc comment.
 *
 * @return CHTTP1_OK          Buffer fully consumed; message not yet
 *                            complete, call again with more data.
 *         CHTTP1_PAUSED      Message complete (on_message_complete just
 *                            fired); see chttp1_parser_consumed().
 *         CHTTP1_HEADERS_ONLY Request mode only; see its own doc comment.
 *         CHTTP1_USER        An application callback reported an error.
 *         CHTTP1_ERROR       Malformed input; see parser->reason.
 */
chttp1_errno_t chttp1_parser_execute(chttp1_parser_t *parser, const char *data,
                                     size_t len);

/**
 * @brief Called when the underlying connection reports EOF (a read()/recv()
 *        returning 0), to determine whether that EOF is a valid way for the
 *        current message to end.
 *
 * A message with no Content-Length and no chunked Transfer-Encoding (an
 * HTTP/1.0-style or explicit-Connection:-close response) is only complete
 * once the connection actually closes; there is no other signal for it.
 * For that case (and only that case), this fires on_message_complete and
 * this function returns CHTTP1_PAUSED; CHTTP1_PAUSED from THIS function
 * means "yes, EOF was a valid, clean end to the message", not "keep going";
 * there is nothing after it to consume, so there is no equivalent of
 * chttp1_parser_consumed() to check afterward.
 *
 * For every other state (before the status line, mid-headers, mid-
 * Content-Length-body, mid-chunk, or already at a clean CHTTP1_ST_MESSAGE_DONE
 * boundary with nothing further expected), this returns CHTTP1_OK if the
 * message was already fully done, or CHTTP1_ERROR if EOF arrived mid-message
 * (a truncated/invalid response).
 *
 * @return CHTTP1_OK      Already at a clean boundary; EOF is fine, no
 *                        callback needed.
 *         CHTTP1_PAUSED  EOF-terminated body just completed via
 *                        on_message_complete (see above).
 *         CHTTP1_ERROR   EOF arrived mid-message; truncated/invalid.
 */
chttp1_errno_t chttp1_parser_finish(chttp1_parser_t *parser);

/**
 * @brief Returns how many bytes of the buffer passed to the most recent
 *        chttp1_parser_execute call were actually consumed by the message
 *        that just completed.
 *
 * Only meaningful immediately after chttp1_parser_execute returns
 * CHTTP1_PAUSED: it reports a plain byte count rather than a raw pointer
 * into the caller's own buffer, so the caller never has to do its own
 * pointer-subtraction arithmetic. Any bytes from this count to the end of
 * that same buffer are unconsumed trailing data the caller never asked for
 * (see chttp1_parser_execute's contract #2).
 */
size_t chttp1_parser_consumed(const chttp1_parser_t *parser);

/**
 * @brief Whether parser has reached a clean, fully-parsed message boundary
 *        (the state chttp1_parser_execute()/chttp1_parser_finish() leave it
 *        in after returning CHTTP1_PAUSED).
 *
 * A thin, stable accessor over parser->state, added so callers driving a
 * request across a CHTTP1_HEADERS_ONLY divert (see that code's own doc
 * comment); resuming chttp1_parser_execute() from a different thread,
 * potentially several calls later; have a documented way to ask "is this
 * request now fully parsed" without reaching into parser->state directly
 * (chttp1__state_t's own values are not part of this module's documented
 * contract and may change).
 */
bool chttp1_parser_message_complete(const chttp1_parser_t *parser);

/**
 * @brief Whether the connection this message arrived on may be reused for a
 *        subsequent request, per RFC 7230 SS6.3: HTTP/1.1 defaults to
 *        keep-alive unless a Connection: close token was seen; HTTP/1.0 (or
 *        earlier) defaults to NOT keep-alive unless a Connection: keep-alive
 *        token was seen; and, either way, a body that could only be
 *        delimited by the connection closing (no Content-Length, no
 *        chunked Transfer-Encoding) is never keep-alive eligible regardless
 *        of any Connection header value present, since ending the body
 *        already required ending the connection.
 *
 * Only meaningful once the message is complete (after chttp1_parser_execute
 * returns CHTTP1_PAUSED, or after a CHTTP1_PAUSED from chttp1_parser_finish).
 */
bool chttp1_should_keep_alive(const chttp1_parser_t *parser);

/**
 * @brief Whether the just-parsed headers included "Expect: 100-continue"
 *        (RFC 7231 SS5.1.1's only defined expectation value; any other
 *        Expect value, or a value combined with other tokens, is NOT
 *        detected here and is left to the caller to reject, e.g. with a 417
 *        Expectation Failed, exactly as an unrecognized Expect value would
 *        need explicit handling regardless of what this parser does).
 *
 * Meaningful once settings->on_headers_complete has fired (headers are now
 * fully parsed). This parser performs no I/O of its own and does NOT
 * automatically send an interim "HTTP/1.1 100 Continue\r\n\r\n" response;
 * a caller that wants to honor the expectation must write that line to the
 * raw connection itself (typically from within, or immediately after, its
 * own on_headers_complete callback) before continuing to feed body bytes to
 * chttp1_parser_execute; a caller that wants to reject the request instead
 * writes a final status response and never reads the body at all.
 *
 * This is the request-mode (server-side) detection half of Expect:
 * 100-continue only. The response-mode (client-side) counterpart
 * (correctly handling a "100 Continue" interim response arriving before the
 * real final response on the same connection, rather than treating it as an
 * ordinary complete (no-body) message the way any other 1xx status is
 * treated today) is deliberately deferred: it has no real caller to
 * exercise it until chttpclient.c itself gains Expect: 100-continue
 * request-sending support, which is a separate, later increment, not this
 * one.
 */
bool chttp1_expects_continue(const chttp1_parser_t *parser);

/* ========================================================================== */
/*                    WORKER-PULL BODY/RESPONSE STREAMING                     */
/* ========================================================================== */

/**
 * @brief A small, reactor-agnostic helper for reading (and writing) raw
 *        bytes off a socket from a worker thread, with an absolute
 *        per-call timeout and support for "carry-over" bytes a reactor
 *        thread already read before handing the connection off.
 *
 * Used internally by chttpserver.c's own worker-driven body ingestion,
 * operating on a raw file descriptor with no dependency on any
 * reactor/event_loop at all; unlike chttp1_parser_t itself, chttp1_stream_t
 * is NOT zero-allocation (it may heap-allocate a copy of the carry-over
 * bytes passed to
 * chttp1_stream_prepare()), since its lifecycle (one instance per diverted
 * request, not embedded pervasively) does not need that same constraint.
 *
 * Typical use: a reactor thread parses headers via chttp1_parser_execute()
 * until it returns CHTTP1_HEADERS_ONLY, hands the fd off to a worker thread
 * along with chttp1_parser_consumed()'s buffer position (anything from there to
 * the end of that buffer is carry-over), the worker calls
 * chttp1_stream_prepare() once with that carry-over, then repeatedly calls
 * chttp1_stream_read() (which drains carry-over first, then reads the raw
 * fd) and feeds each chunk to chttp1_parser_execute() to keep driving the
 * SAME parser instance through body/chunk/trailer parsing until it returns
 * CHTTP1_PAUSED. chttp1_stream_write() is the symmetric primitive for the
 * worker-owned response-write phase.
 */
typedef struct chttp1_stream {
  int fd;

  /* Opaque ctls_conn_t*, or NULL for a plaintext connection. Declared void*
   * (rather than ctls_conn_t*) so this header does not need to #include
   * ctls.h; chttp1_parser.c casts it internally. Set via
   * chttp1_stream_prepare_tls() instead of chttp1_stream_prepare(); see
   * that function's own doc comment. When set, chttp1_stream_read()/_write()
   * call ctls_conn_read()/_write() instead of raw read(2)/write(2); fd is
   * still used for poll(2) readiness waits (ctls_conn_t has no polling
   * primitive of its own; it is deliberately reactor-agnostic). */
  void *tls;

  /* Carry-over bytes: heap-allocated copy of whatever chttp1_stream_prepare
   * was given, drained (via carry_pos) before any real read(2) touches fd.
   * NULL/0 if chttp1_stream_prepare was given no leftover bytes. For a TLS
   * connection these are already-decrypted application bytes (produced by
   * ctls_conn_read() during the reactor thread's own header-parsing loop),
   * not raw wire bytes; the same meaning as the plaintext case, just
   * already past the TLS layer. */
  char *carry;
  size_t carry_len;
  size_t carry_pos;

  int last_errno; /* errno from the most recent failed read/write, or 0 */
  bool timed_out; /* true if the most recent read/write failed because its
                   * deadline elapsed (poll(2) returned 0), not a real I/O
                   * error; mutually exclusive with last_errno being
                   * meaningful for that same call */
  bool prepared;
  bool released;
} chttp1_stream_t;

/**
 * @brief Prepares stream for worker-pull reads/writes on fd.
 *
 * leftover/leftover_len are bytes already read off fd by someone else
 * (typically a reactor thread, while parsing headers) that logically come
 * BEFORE any further bytes read from fd itself, and so must be delivered
 * first; they are copied internally (into stream's own heap-allocated
 * buffer), so the caller's own buffer may be reused or freed immediately
 * after this call returns. Pass leftover_len == 0 (leftover may be NULL) if
 * there is none.
 *
 * Must be called synchronously, before any other thread begins touching fd
 * concurrently; i.e. call this BEFORE actually handing fd off to whatever
 * mechanism wakes the worker thread, not after.
 *
 * @return true on success (including when leftover_len == 0); false only on
 *         allocation failure (leftover_len > 0 and copying it failed);
 *         stream is left safely zero-initialized-equivalent on failure, and
 *         chttp1_stream_release() is still safe (and unnecessary, but
 *         harmless) to call on it.
 */
bool chttp1_stream_prepare(chttp1_stream_t *stream, int fd,
                           const char *leftover, size_t leftover_len);

/**
 * @brief Prepares stream for worker-pull reads/writes on a TLS connection.
 *
 * Identical to chttp1_stream_prepare() (same leftover/leftover_len contract:
 * already-decrypted application bytes the reactor thread produced via
 * ctls_conn_read() during its own header-parsing loop, not raw wire bytes),
 * except chttp1_stream_read()/_write() subsequently call
 * ctls_conn_read()/_write() on tls_conn instead of raw read(2)/write(2) on
 * fd. fd is still needed and still used, purely for poll(2) readiness waits
 *; ctls_conn_t is deliberately reactor-agnostic and has no polling
 * primitive of its own.
 *
 * @param tls_conn  The ctls_conn_t (from ctls.h) driving this connection's
 *                  encrypted I/O; must already have completed its handshake
 *                  (ctls_conn_handshake_step() returned CTLS_HANDSHAKE_DONE).
 *                  Ownership is not transferred: the caller still destroys
 *                  it (via ctls_conn_destroy()) after chttp1_stream_release().
 * @return Same as chttp1_stream_prepare().
 */
bool chttp1_stream_prepare_tls(chttp1_stream_t *stream, int fd, void *tls_conn,
                               const char *leftover, size_t leftover_len);

/**
 * @brief Reads up to buflen bytes into buf.
 *
 * Drains any remaining carry-over bytes first (never touching fd at all
 * while carry-over remains); once carry-over is exhausted, blocks via
 * poll(2) for up to timeout_ms milliseconds waiting for fd to become
 * readable, then performs one read (via ctls_conn_read() if stream was
 * prepared with chttp1_stream_prepare_tls(), or a raw read(2) otherwise).
 * For a TLS stream specifically, a single readiness event does not
 * guarantee application bytes come back immediately (a partial TLS record,
 * or a renegotiation/key-update message OpenSSL consumes internally, can
 * require another read); in that case (ctls_conn_read() reporting
 * EWOULDBLOCK/EAGAIN) this transparently polls and retries, still bounded
 * by the same overall timeout_ms budget, not a fresh one per retry.
 *
 * @param timeout_ms  Negative means block indefinitely (no timeout,
 *                    matching poll(2)'s own -1 convention); 0 means return
 *                    immediately if fd is not already readable right now;
 *                    positive is the maximum number of milliseconds to
 *                    wait, across every internal retry combined. Not
 *                    consulted at all while carry-over bytes remain (those
 *                    are always returned immediately).
 * @return Number of bytes read (> 0, possibly satisfied entirely from
 *         carry-over), 0 on a clean EOF (peer closed its write side, or a
 *         clean TLS close_notify), or -1 on error or timeout; see
 *         chttp1_stream_timed_out() and chttp1_stream_last_error() to
 *         distinguish the two.
 */
ssize_t chttp1_stream_read(chttp1_stream_t *stream, char *buf, size_t buflen,
                           int timeout_ms);

/**
 * @brief Writes up to len bytes from buf to stream's fd (or, for a TLS
 *        stream, encrypts and writes them via ctls_conn_write()), blocking
 *        via poll(2) for up to timeout_ms milliseconds if not immediately
 *        writable, with the same retry-within-the-same-budget behavior
 *        chttp1_stream_read() documents for a TLS stream. Same timeout_ms
 *        convention as chttp1_stream_read().
 *
 * Needed for the worker-owned response-write phase, bounding how long a
 * slow-reading peer can hold a worker thread during response send.
 *
 * @return Number of bytes written (may be less than len, matching write(2)
 *         itself; callers needing to write all of a larger buffer must
 *         loop), or -1 on error or timeout.
 */
ssize_t chttp1_stream_write(chttp1_stream_t *stream, const char *buf,
                            size_t len, int timeout_ms);

/**
 * @brief True if the most recent chttp1_stream_read/_write call returned -1
 *        because its deadline elapsed (poll(2) returned 0) rather than a
 *        real I/O error.
 */
bool chttp1_stream_timed_out(const chttp1_stream_t *stream);

/**
 * @brief Returns the errno from the most recent failed chttp1_stream_read/
 *        _write call, or 0 if the most recent call succeeded, timed out
 *        (see chttp1_stream_timed_out() instead), or was satisfied entirely
 *        from carry-over.
 */
int chttp1_stream_last_error(const chttp1_stream_t *stream);

/**
 * @brief Releases stream's own resources (its carry-over buffer, if any).
 *        Does NOT close fd; ownership of the file descriptor itself was
 *        never transferred to this struct by chttp1_stream_prepare().
 *
 * Must be called exactly once, after ingestion is fully done (EOF, error,
 * or the handler simply stopped reading/writing early). Safe to call on a
 * zero-initialized (never-prepared) stream, or to call a second time (a
 * no-op).
 */
void chttp1_stream_release(chttp1_stream_t *stream);
