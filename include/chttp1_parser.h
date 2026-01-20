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

/**
 * @file chttp1_parser.h
 * @brief INTERNAL ONLY. A small, hand-written HTTP/1.1 *response* parser,
 *        used solely by chttpclient.c.
 *
 * This is not a public collections module: it has no macros, no opaque
 * handle type, and is never included by chttp.h/chttpclient.h/chttpserver.h
 * or any other public header. src/chttpclient.c is the only file that
 * #includes this header.
 *
 * This replaces the vendored, machine-generated third_party/llhttp (11,562
 * lines) with a purpose-built parser covering exactly the subset
 * chttpclient.c ever needed: parsing an HTTP/1.1 *response* (status line,
 * headers, and a Content-Length/chunked/EOF-delimited body). It never parses
 * a request line, a method, or a URL -- chttpclient.c always constructed the
 * old parser with type HTTP_RESPONSE (never HTTP_REQUEST/HTTP_BOTH), so that
 * half of llhttp's grammar was confirmed-dead code for this codebase.
 *
 * ### Design: a plain, self-contained value type, no heap allocation
 *
 * chttp1_parser_t has zero owned resources, exactly like llhttp_t did: it is
 * safe to embed by value on the stack (as chttpclient.c's synchronous
 * _chttp_read_response does) or as a struct member (as chttpclient.c's
 * chttp_async_ctx_t does), and there is no destroy/free function -- there is
 * nothing to release. This is possible because the internal line-
 * accumulation buffer used for the status line, header lines, and
 * chunk-size lines is a FIXED-size array (CHTTP1_MAX_LINE_LEN, defined
 * below) embedded directly in the struct, not a growable/realloc'd
 * buffer; this is also what enforces the new header-size cap described
 * below (a line that hasn't terminated by the time the fixed buffer fills
 * up is rejected as "line too long"). Body/chunk-data bytes are never
 * copied into this buffer at all -- they are passed straight from the
 * caller's own read buffer to on_body, zero-copy, exactly like today.
 *
 * ### One protection added that the old llhttp integration lacked
 *
 * Neither llhttp nor chttpclient.c enforced any limit on a single header
 * line's length, the number of headers, or their total size -- a slow-drip
 * malicious or misbehaving server could force unbounded client memory
 * growth via one endless header line. This parser enforces three fixed
 * limits (CHTTP1_MAX_LINE_LEN, defined below; CHTTP1_MAX_HEADER_COUNT and
 * CHTTP1_MAX_TOTAL_HEADER_BYTES, both private to chttp1_parser.c since they
 * don't affect this struct's layout) that, combined, bound this. Trailer
 * headers (after a chunked body's terminating 0-length chunk) count against
 * the exact same header-count/total-bytes budget as regular headers --
 * there is no separate trailer allowance to bypass the cap through.
 *
 * ### Thread safety
 *
 * A chttp1_parser_t instance is not thread-safe (nothing here is meant to
 * be shared between threads); this matches how chttpclient.c already uses
 * it -- one instance per in-flight hop, touched by whichever single thread
 * (or, in the async tier, whichever single reactor callback invocation) is
 * currently driving that hop.
 */

/* ========================================================================== */
/*                         RESULT CODES                                      */
/* ========================================================================== */

/**
 * @brief Outcome of chttp1_parser_execute()/chttp1_parser_finish().
 *
 * Mirrors the exact three-way split chttpclient.c already branches on
 * against llhttp's HPE_PAUSED/HPE_USER/other-error: CHTTP1_OK means "keep
 * reading, message not yet complete"; CHTTP1_PAUSED means the message just
 * completed (see the hard contract on chttp1_parser_execute below);
 * CHTTP1_USER means an application callback (on_header /
 * on_headers_complete / on_body / on_message_complete) itself reported an
 * error; CHTTP1_ERROR covers every parse failure (malformed status line,
 * invalid header, chunk-size overflow, oversized header, etc.) -- this
 * parser does not further subdivide CHTTP1_ERROR the way llhttp's ~30
 * distinct HPE_* codes did, since chttpclient.c never distinguished any of
 * them beyond "not HPE_OK/HPE_PAUSED/HPE_USER" either. A human-readable
 * reason string for the specific rejection is still available via
 * chttp1_parser_t.reason, for diagnostics/tests.
 */
typedef enum {
  CHTTP1_OK = 0,
  CHTTP1_PAUSED,
  CHTTP1_USER,
  CHTTP1_ERROR
} chttp1_errno_t;

/* ========================================================================== */
/*                         INTERNAL STATE (opaque to chttpclient.c)          */
/* ========================================================================== */

/* Not part of this module's public-to-chttpclient.c contract; declared here
 * only because chttp1_parser_t must be a complete, stack-embeddable type. */
typedef enum {
  CHTTP1_ST_STATUS_LINE = 0,
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

/* Mirrors llhttp's HTTP_FINISH_SAFE / _SAFE_WITH_CB / _UNSAFE three-way,
 * which is what makes chttp1_parser_finish() correct without the call site
 * needing to know which body-framing mode was in effect. */
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
 * do not affect this struct's layout -- they are plain running counts
 * compared against constants private to chttp1_parser.c.
 */
#define CHTTP1_MAX_LINE_LEN 8192

typedef struct chttp1_parser chttp1_parser_t;

/**
 * @brief Callback settings, mirroring llhttp_settings_t's shape but reduced
 *        to exactly the 4 callbacks chttpclient.c ever wired (of llhttp's
 *        ~20): on_header (replacing llhttp's on_header_field/on_header_value
 *        /on_header_value_complete 3-callback fragment protocol -- a header
 *        line is always assembled whole, internally, before this fires, so
 *        there is no fragment-reassembly protocol for the caller to
 *        implement any more), on_headers_complete, on_body, and
 *        on_message_complete.
 *
 * Every callback is NULL-checked before being invoked, even though
 * chttpclient.c always wires all four in practice.
 */
typedef struct {
  /**
   * Fired once per complete header line (and once per trailer line, after a
   * chunked body's terminating 0-length chunk -- there is no regular-vs-
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
   * otherwise surfaced, matching llhttp's own on_header_value_complete
   * contract).
   */
  int (*on_header)(chttp1_parser_t *p, const char *name, size_t name_len,
                   const char *value, size_t value_len);

  /**
   * Fired once, after the blank line ending the header block.
   * p->status_code is already set.
   *
   * Return 0 if this response has a body (the normal case), 1 if the
   * caller already knows -- out of band, e.g. because the request method
   * was HEAD -- that no body follows regardless of any Content-Length
   * header present (llhttp could not infer this from the wire either; this
   * mirrors its on_headers_complete return-code contract exactly, minus the
   * "2 = no body, pause for upgrade" case, which chttpclient.c never used:
   * it never sends CONNECT and has no upgrade handling), or any OTHER value
   * (including a negative one) to abort the parse with CHTTP1_USER -- this
   * three-way split (0 / 1 / anything-else-is-an-error) is deliberate, not
   * merely tolerated: chttpclient.c's real on_headers_complete implementation
   * needs a genuine error-reporting path here (e.g. an allocation failure
   * while duplicating a Location header during redirect detection), so
   * anything outside {0, 1} is always treated as a reported error, never
   * silently coerced into "no body".
   */
  int (*on_headers_complete)(chttp1_parser_t *p);

  /**
   * Fired zero or more times with body bytes as they are parsed out of
   * Content-Length-delimited or chunked framing. at points directly into
   * the buffer passed to chttp1_parser_execute (NOT into this parser's own
   * line buffer -- body bytes are never copied internally) and is valid
   * only for the duration of this call.
   *
   * Return 0 on success, or nonzero to abort with CHTTP1_USER.
   */
  int (*on_body)(chttp1_parser_t *p, const char *at, size_t len);

  /**
   * Fired exactly once, when the message (headers + whatever body framing
   * applies) is fully parsed. Unlike llhttp's identical callback (which had
   * to always return HPE_PAUSED as a signal to its own generated dispatch
   * loop), this parser already knows independently that it has just reached
   * a message boundary -- it drives its own explicit state machine rather
   * than relying on a callback's return value to learn that -- so this
   * follows the same plain convention as the other three callbacks: return
   * 0 on success, or nonzero to abort with CHTTP1_USER. chttp1_parser_execute
   * and chttp1_parser_finish report the pause itself via their own
   * CHTTP1_PAUSED return value regardless of what this callback returns
   * (beyond that zero/nonzero distinction).
   */
  int (*on_message_complete)(chttp1_parser_t *p);
} chttp1_settings_t;

/**
 * @brief The parser itself. Fully self-contained (no owned resources -- see
 *        the file-level doc comment); safe to embed by value on the stack or
 *        as a struct member. There is no destroy/free function.
 */
struct chttp1_parser {
  /** Opaque to this parser; set once after chttp1_parser_init and read back
   * in every callback to recover the caller's own context, exactly like
   * llhttp_t.data. */
  void *data;

  /** Set once, in on_headers_complete, from the parsed status line. */
  int status_code;

  /** Diagnostic only: a static string describing the most recent rejection
   * (e.g. "Duplicate Content-Length"), or NULL. Not consumed by
   * chttpclient.c's own ccol_retval_t-based error reporting today (every
   * CHTTP1_ERROR collapses to the same ccol_http_transfer_aborted regardless
   * of reason) -- provided for logs and for this module's own test suite to
   * assert specific rejection reasons. */
  const char *reason;

  /* --- Everything below is internal state; chttpclient.c does not read or
   * write any of it directly. --- */
  chttp1__state_t state;
  chttp1__finish_state_t finish_state;
  uint8_t http_major;
  uint8_t http_minor;
  uint16_t flags;
  uint64_t content_length; /* remaining bytes for CHTTP1_ST_BODY_CONTENT_LENGTH,
                            * or remaining bytes in the CURRENT chunk for
                            * CHTTP1_ST_BODY_CHUNK_DATA; reused for both,
                            * exactly like llhttp_t.content_length was. */
  bool is_trailer_section; /* true once parsing trailers after a chunked
                            * body's terminating 0-length chunk; on_header's
                            * header-vs-trailer distinction (none) is the
                            * same either way, this only affects which state
                            * to return to after the blank line. */

  size_t header_count;
  size_t total_header_bytes;

  /* Fixed-size accumulation buffer for the status line, each header/trailer
   * line, and each chunk-size line -- never used for body/chunk-data bytes,
   * which are passed straight from the caller's own buffer to on_body. A
   * partial line spanning two chttp1_parser_execute calls accumulates here;
   * see CHTTP1_MAX_LINE_LEN's own comment for why this is fixed-size rather
   * than growable. */
  char line_buf[CHTTP1_MAX_LINE_LEN];
  size_t line_len;

  size_t consumed; /* backs chttp1_parser_consumed(); only meaningful
                    * immediately after chttp1_parser_execute returns
                    * CHTTP1_PAUSED -- see that function's doc comment. */

  const chttp1_settings_t *settings;
};

/* ========================================================================== */
/*                         LIFECYCLE                                          */
/* ========================================================================== */

/** @brief Zero-initialise a chttp1_settings_t (all callbacks NULL). */
void chttp1_settings_init(chttp1_settings_t *settings);

/**
 * @brief Initialise parser for a new message. Void and infallible -- there
 *        is no allocation to fail.
 *
 * settings must outlive parser (the same lifetime contract llhttp_init
 * documented for its own settings pointer).
 *
 * Unlike llhttp_init, there is no "type" parameter: this parser only ever
 * parses responses.
 *
 * There is no chttp1_parser_reset(): chttpclient.c never reused a parser
 * instance across hops (every hop, in both the sync and async tiers, always
 * called llhttp_init fresh), so this parser has no in-place-reuse contract
 * either -- construct a fresh instance (or re-run chttp1_parser_init) per
 * message.
 */
void chttp1_parser_init(chttp1_parser_t *parser,
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
 *    caller's to discard; this parser has no pipelining support (matching
 *    every prior caller of the old llhttp-based parser, which also never
 *    fed it again after HPE_PAUSED).
 *
 * @return CHTTP1_OK      Buffer fully consumed; message not yet complete,
 *                        call again with more data.
 *         CHTTP1_PAUSED  Message complete (on_message_complete just fired);
 *                        see chttp1_parser_consumed().
 *         CHTTP1_USER    An application callback reported an error.
 *         CHTTP1_ERROR   Malformed input; see parser->reason.
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
 * once the connection actually closes -- there is no other signal for it.
 * For that case (and only that case), this fires on_message_complete and
 * this function returns CHTTP1_PAUSED -- CHTTP1_PAUSED from THIS function
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
 * CHTTP1_PAUSED (replaces llhttp_get_error_pos()'s raw pointer-into-the-
 * caller's-own-buffer plus the pointer subtraction every call site had to
 * do itself). Any bytes from this count to the end of that same buffer are
 * unconsumed trailing data the caller never asked for (see
 * chttp1_parser_execute's contract #2).
 */
size_t chttp1_parser_consumed(const chttp1_parser_t *parser);

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
