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

#include <chttp1_parser.h>
#include <ctls.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* ========================================================================== */
/*                         PRIVATE CONSTANTS                                  */
/* ========================================================================== */

/* Regular headers + trailers combined; see chttp1_parser.h's file-level doc
 * comment. Neither constant affects chttp1_parser_t's layout (unlike
 * CHTTP1_MAX_LINE_LEN, which is public precisely because it does), so both
 * stay private to this file. */
#define CHTTP1_MAX_HEADER_COUNT 100
#define CHTTP1_MAX_TOTAL_HEADER_BYTES (64 * 1024)

/* RFC 7230 SS3.5 recommends a server ignore at least one leading empty line
 * before the request-line (a stray CRLF some clients send after a POST
 * body); bounded rather than skipped unconditionally, matching every other
 * cap in this file, so a client that never stops sending blank lines is
 * treated as malformed input instead of tolerated indefinitely. Response
 * mode never uses this (a leading blank line before a status line has no
 * equivalent real-world client bug to work around). */
#define CHTTP1_MAX_LEADING_BLANK_LINES 25

/* chttp1_parser_t.flags bits. */
#define F_CHUNKED 0x01u
#define F_CONTENT_LENGTH 0x02u
#define F_TRANSFER_ENCODING 0x04u
#define F_CONNECTION_CLOSE 0x08u
#define F_CONNECTION_KEEP_ALIVE 0x10u
#define F_EXPECT_100_CONTINUE 0x20u
#define F_CHUNK_TOO_LARGE                          \
  0x40u /* set alongside a max_chunk_size_override \
         * rejection; see                          \
         * chttp1_chunk_size_limit_exceeded() */

/* ========================================================================== */
/*                         SMALL CHARACTER HELPERS                            */
/* ========================================================================== */

static bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

static bool is_ows(unsigned char c) { return c == ' ' || c == '\t'; }

/* RFC 7230 SS3.2.6 tchar: the set of bytes a header field NAME may contain.
 * Not static: exposed to chttpclient.c/chttpserver.c via chttp1_parser.h;
 * see that declaration's own doc comment. */
bool chttp1_is_tchar(unsigned char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c))
    return true;
  switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
      return true;
    default:
      return false;
  }
}

/* Whether a byte is disallowed inside a header VALUE under this parser's
 * only (strict) mode: any control character other than HTAB, including a
 * bare CR or NUL that survived line-splitting as ordinary content (see
 * _accumulate_line's own comment for why those are not rejected there). */
static bool is_invalid_value_byte(unsigned char c) {
  return (c < 0x20 && c != '\t') || c == 0x7f;
}

/* ========================================================================== */
/*                         OVERFLOW-CHECKED INTEGER PARSING */
/* ========================================================================== */

/* A standard two-step (multiply-then-add, each checked) overflow-guard
 * shape, used for both radices this parser ever needs. */

static bool parse_uint64_decimal(const char *s, size_t len, uint64_t *out) {
  if (len == 0) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (!is_digit(c)) return false;
    unsigned d = (unsigned)(c - '0');
    if (v > UINT64_MAX / 10) return false;
    v *= 10;
    if (v > UINT64_MAX - d) return false;
    v += d;
  }
  *out = v;
  return true;
}

static bool parse_uint64_hex(const char *s, size_t len, uint64_t *out) {
  if (len == 0) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    unsigned d;
    if (c >= '0' && c <= '9')
      d = (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f')
      d = (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      d = (unsigned)(c - 'A' + 10);
    else
      return false;
    if (v > UINT64_MAX / 16) return false;
    v *= 16;
    if (v > UINT64_MAX - d) return false;
    v += d;
  }
  *out = v;
  return true;
}

/* ========================================================================== */
/*                         LINE ACCUMULATION                                  */
/* ========================================================================== */

typedef enum {
  LINE_NEED_MORE,
  LINE_READY,
  LINE_TOO_LONG,
  LINE_BAD_EOL
} line_result_t;

/*
 * Appends bytes from *pp (up to end) into parser->line_buf until a full
 * CRLF-terminated line is assembled, or the fixed buffer fills up, or input
 * runs out. *pp is always advanced to reflect exactly how much was consumed
 * (== end on LINE_NEED_MORE/LINE_TOO_LONG, one past the line-ending byte on
 * LINE_READY/LINE_BAD_EOL), matching chttp1_parser_execute's own "always
 * consume everything" contract for the non-terminal cases.
 *
 * On LINE_READY, parser->line_len is the line's content length with the
 * trailing CRLF already stripped; on any other result, parser->line_len is
 * left as whatever was accumulated so far (including, on LINE_TOO_LONG, the
 * full CHTTP1_MAX_LINE_LEN).
 *
 * Any '\n' not immediately preceded by '\r' is LINE_BAD_EOL; this rejects
 * both a bare LF used as a line terminator and a bare LF embedded mid-line
 * (e.g. unfolded multi-line header content), which is exactly the strict
 * (this parser's only mode) behaviour: there is no lenient mode anywhere in
 * this codebase that would relax either case (e.g. by accepting a bare LF,
 * or a CR not immediately followed by LF, as a valid line terminator).
 * A bare CR that is NOT immediately followed by LF is deliberately not
 * specially rejected here; it simply becomes ordinary line content, which
 * is then caught by the caller's own control-character validation of that
 * content (is_invalid_value_byte, or the reason-phrase scan in
 * _parse_status_line) rather than duplicating that check at this layer.
 */
static line_result_t accumulate_line(chttp1_parser_t *parser, const char **pp,
                                     const char *end) {
  const char *p = *pp;
  while (p < end) {
    if (parser->line_len >= CHTTP1_MAX_LINE_LEN) {
      *pp = p;
      return LINE_TOO_LONG;
    }
    char c = *p++;
    parser->line_buf[parser->line_len++] = c;
    if (c == '\n') {
      if (parser->line_len < 2 ||
          parser->line_buf[parser->line_len - 2] != '\r') {
        *pp = p;
        return LINE_BAD_EOL;
      }
      parser->line_len -= 2;
      *pp = p;
      return LINE_READY;
    }
  }
  *pp = p;
  return LINE_NEED_MORE;
}

/* Shared by parse_request_line() and process_header_line() below (both need
 * to distinguish a malformed-input rejection from an application callback's
 * own reported error). */
typedef enum { PH_OK, PH_ERROR, PH_USER } ph_result_t;

/* ========================================================================== */
/*                         STATUS LINE                                        */
/* ========================================================================== */

/*
 * status-line = HTTP-version SP status-code [ SP reason-phrase ]
 * HTTP-version = "HTTP/" DIGIT "." DIGIT
 *
 * Accepts any single-digit major/minor (not hard-coded to 1.x: this parser
 * doesn't restrict it beyond the grammar, since chttp1_should_keep_alive
 * already needs to compare arbitrary major/minor values correctly
 * regardless) and exactly 3 decimal digits for the status code with no
 * numeric-range restriction (real, in-use nonstandard status codes go up to
 * 599 and there is no claim that is exhaustive; validate digit-count only
 * and let the caller judge magnitude).
 *
 * The reason phrase, and the single space before it, are BOTH optional:
 * RFC 7230's ABNF technically requires a trailing SP even for an empty
 * reason phrase, but real servers routinely omit it entirely (e.g.
 * "HTTP/1.1 304\r\n"); rejecting that would be a real-world compatibility
 * regression versus the parser this replaces.
 */
static bool parse_status_line(chttp1_parser_t *parser) {
  const char *s = parser->line_buf;
  size_t len = parser->line_len;

  if (len < 8 || memcmp(s, "HTTP/", 5) != 0) {
    parser->reason = "Invalid HTTP version";
    return false;
  }
  size_t i = 5;
  if (!is_digit((unsigned char)s[i])) {
    parser->reason = "Invalid HTTP version";
    return false;
  }
  parser->http_major = (uint8_t)(s[i] - '0');
  i++;
  if (s[i] != '.') {
    parser->reason = "Invalid HTTP version";
    return false;
  }
  i++;
  if (!is_digit((unsigned char)s[i])) {
    parser->reason = "Invalid HTTP version";
    return false;
  }
  parser->http_minor = (uint8_t)(s[i] - '0');
  i++;
  if (i >= len || s[i] != ' ') {
    parser->reason = "Invalid HTTP version";
    return false;
  }
  i++;

  if (i + 3 > len || !is_digit((unsigned char)s[i]) ||
      !is_digit((unsigned char)s[i + 1]) ||
      !is_digit((unsigned char)s[i + 2])) {
    parser->reason = "Invalid status code";
    return false;
  }
  parser->status_code =
      (s[i] - '0') * 100 + (s[i + 1] - '0') * 10 + (s[i + 2] - '0');
  i += 3;

  if (i < len) {
    if (s[i] != ' ') {
      parser->reason = "Invalid status line";
      return false;
    }
    i++;
    for (; i < len; i++) {
      if (is_invalid_value_byte((unsigned char)s[i])) {
        parser->reason = "Invalid character in reason phrase";
        return false;
      }
    }
  }
  return true;
}

/*
 * request-line = method SP request-target SP HTTP-version CRLF
 *
 * method is validated as a tchar-only token (the same character class
 * header field names use, per RFC 7230 SS3.1.1/SS3.2.6) but not matched
 * against any specific set of known method names; an unrecognized method
 * is the caller's own routing concern (e.g. surfaced as an ordinary 405),
 * not this parser's to reject. request-target is validated only for the
 * absence of control characters; this parser does not distinguish
 * origin-form from absolute-form/authority-form/asterisk-form, matching
 * chttpserver's own scope as an origin server with no CONNECT/proxy
 * support. HTTP-version uses the identical grammar/validation as
 * parse_status_line's own version field.
 */
static ph_result_t parse_request_line(chttp1_parser_t *parser) {
  const char *s = parser->line_buf;
  size_t len = parser->line_len;

  size_t i = 0;
  while (i < len && s[i] != ' ') i++;
  if (i == 0 || i >= len) {
    parser->reason = "Invalid request line";
    return PH_ERROR;
  }
  const char *method = s;
  size_t method_len = i;
  for (size_t j = 0; j < method_len; j++) {
    if (!chttp1_is_tchar((unsigned char)method[j])) {
      parser->reason = "Invalid method";
      return PH_ERROR;
    }
  }
  i++;

  size_t target_start = i;
  while (i < len && s[i] != ' ') i++;
  if (i == target_start || i >= len) {
    parser->reason = "Invalid request line";
    return PH_ERROR;
  }
  const char *target = s + target_start;
  size_t target_len = i - target_start;
  for (size_t j = 0; j < target_len; j++) {
    if (is_invalid_value_byte((unsigned char)target[j])) {
      parser->reason = "Invalid character in request target";
      return PH_ERROR;
    }
  }
  i++;

  if (len - i < 8 || memcmp(s + i, "HTTP/", 5) != 0) {
    parser->reason = "Invalid HTTP version";
    return PH_ERROR;
  }
  i += 5;
  if (!is_digit((unsigned char)s[i])) {
    parser->reason = "Invalid HTTP version";
    return PH_ERROR;
  }
  parser->http_major = (uint8_t)(s[i] - '0');
  i++;
  if (s[i] != '.') {
    parser->reason = "Invalid HTTP version";
    return PH_ERROR;
  }
  i++;
  if (!is_digit((unsigned char)s[i])) {
    parser->reason = "Invalid HTTP version";
    return PH_ERROR;
  }
  parser->http_minor = (uint8_t)(s[i] - '0');
  i++;
  if (i != len) {
    parser->reason = "Invalid request line";
    return PH_ERROR;
  }

  if (parser->settings && parser->settings->on_request_line) {
    int err = parser->settings->on_request_line(parser, method, method_len,
                                                target, target_len);
    if (err != 0) return PH_USER;
  }
  return PH_OK;
}

/* ========================================================================== */
/*                         HEADER / TRAILER LINES                             */
/* ========================================================================== */

static bool header_name_is(const char *name, size_t name_len,
                           const char *literal) {
  size_t lit_len = strlen(literal);
  return name_len == lit_len && strncasecmp(name, literal, lit_len) == 0;
}

/* Whether the (already OWS-trimmed) Transfer-Encoding value's LAST
 * comma-separated token is "chunked"; the only thing this parser's
 * response-parsing side needs to know about Transfer-Encoding, since
 * chttpclient.c never wires a callback that would need to see individual
 * encodings. */
static bool value_ends_with_chunked(const char *v, size_t len) {
  size_t tok_end = len;
  size_t tok_start = len;
  while (tok_start > 0 && v[tok_start - 1] != ',') tok_start--;
  while (tok_start < tok_end && is_ows((unsigned char)v[tok_start]))
    tok_start++;
  size_t tok_len = tok_end - tok_start;
  return tok_len == 7 && strncasecmp(v + tok_start, "chunked", 7) == 0;
}

/* RFC 7230 SS3.3.1: "chunked" MUST be the final transfer-coding in a
 * Transfer-Encoding list. This only matters for REQUESTS (a response's
 * framing that ignores this rule is this parser's own, pre-existing,
 * documented scope reduction; see value_ends_with_chunked's own doc
 * comment; chttpclient.c only ever parses responses, so this check is
 * gated to CHTTP1_PARSE_REQUEST call sites only). Returns true iff
 * "chunked" (case-insensitive) appears as some comma-separated token that
 * is NOT the last one; the request-smuggling-shaped case a front/back
 * server disagreement could otherwise arise from (see
 * chunked_not_last_in_transfer_encoding_list_rejected in tests.c). */
static bool transfer_encoding_has_nonfinal_chunked(const char *v, size_t len) {
  size_t i = 0;
  while (i < len) {
    size_t start = i;
    while (i < len && v[i] != ',') i++;
    size_t tok_end = i;
    bool is_last_token = (i >= len);
    while (tok_end > start && is_ows((unsigned char)v[tok_end - 1])) tok_end--;
    size_t ts = start;
    while (ts < tok_end && is_ows((unsigned char)v[ts])) ts++;
    size_t tok_len = tok_end - ts;
    if (!is_last_token && tok_len == 7 &&
        strncasecmp(v + ts, "chunked", 7) == 0)
      return true;
    if (i < len) i++; /* skip the comma */
  }
  return false;
}

/* Sets F_CONNECTION_CLOSE/F_CONNECTION_KEEP_ALIVE from a comma-separated
 * Connection header value. A possible "upgrade" token is deliberately not
 * checked: this parser has no upgrade handling at all (chttpclient.c never
 * sends CONNECT or Upgrade requests), so there would be nothing to do with
 * that flag even if set. */
static void parse_connection_tokens(chttp1_parser_t *parser, const char *v,
                                    size_t len) {
  size_t i = 0;
  while (i < len) {
    while (i < len && (is_ows((unsigned char)v[i]) || v[i] == ',')) i++;
    size_t start = i;
    while (i < len && v[i] != ',') i++;
    size_t tok_end = i;
    while (tok_end > start && is_ows((unsigned char)v[tok_end - 1])) tok_end--;
    size_t tok_len = tok_end - start;
    if (tok_len == 5 && strncasecmp(v + start, "close", 5) == 0) {
      parser->flags |= F_CONNECTION_CLOSE;
    } else if (tok_len == 10 && strncasecmp(v + start, "keep-alive", 10) == 0) {
      parser->flags |= F_CONNECTION_KEEP_ALIVE;
    }
  }
}

/*
 * Parses parser->line_buf[0..line_len) as one header/trailer line ("name:
 * value"), validates it (including the three size caps; see
 * chttp1_parser.h's file-level doc comment; and the framing-relevant
 * checks: duplicate Content-Length, Content-Length+chunked conflict,
 * Content-Length decimal overflow), updates framing flags for the three
 * header names this parser cares about, and invokes settings->on_header.
 * Shared verbatim between CHTTP1_ST_HEADERS and CHTTP1_ST_BODY_CHUNK_TRAILERS
 *; there is no regular-vs-trailer distinction at this layer, matching
 * chttpclient.c's own existing behaviour of inserting both into the same
 * headers map.
 */
/* Shared by process_header_line and the CHTTP1_ST_FIRST_LINE case below, so
 * the request/status line's own contribution to total_header_bytes is
 * checked against the exact same effective cap a header line is. */
static size_t effective_max_total_header_bytes(const chttp1_parser_t *parser) {
  return parser->max_total_header_bytes_override
             ? parser->max_total_header_bytes_override
             : CHTTP1_MAX_TOTAL_HEADER_BYTES;
}

static ph_result_t process_header_line(chttp1_parser_t *parser) {
  size_t contribution =
      parser->line_len + 2; /* +2: the CRLF accumulate_line stripped */
  size_t max_count = parser->max_header_count_override
                         ? parser->max_header_count_override
                         : CHTTP1_MAX_HEADER_COUNT;
  size_t max_bytes = effective_max_total_header_bytes(parser);
  if (parser->header_count + 1 > max_count) {
    parser->reason = "Too many headers";
    return PH_ERROR;
  }
  if (parser->total_header_bytes + contribution > max_bytes) {
    parser->reason = "Header block too large";
    return PH_ERROR;
  }

  char *line = parser->line_buf;
  size_t line_len = parser->line_len;
  char *colon = memchr(line, ':', line_len);
  if (!colon || colon == line) {
    parser->reason = "Invalid header field";
    return PH_ERROR;
  }
  char *name = line;
  size_t name_len = (size_t)(colon - line);
  for (size_t i = 0; i < name_len; i++) {
    if (!chttp1_is_tchar((unsigned char)name[i])) {
      parser->reason = "Invalid header field char";
      return PH_ERROR;
    }
  }

  char *vstart = colon + 1;
  char *vend = line + line_len;
  while (vstart < vend && is_ows((unsigned char)*vstart)) vstart++;
  while (vend > vstart && is_ows((unsigned char)vend[-1])) vend--;
  size_t value_len = (size_t)(vend - vstart);
  for (size_t i = 0; i < value_len; i++) {
    if (is_invalid_value_byte((unsigned char)vstart[i])) {
      parser->reason = "Invalid header value char";
      return PH_ERROR;
    }
  }

  if (header_name_is(name, name_len, "content-length")) {
    if (parser->flags & F_CONTENT_LENGTH) {
      parser->reason = "Duplicate Content-Length";
      return PH_ERROR;
    }
    uint64_t v;
    if (!parse_uint64_decimal(vstart, value_len, &v)) {
      parser->reason = "Invalid Content-Length";
      return PH_ERROR;
    }
    parser->flags |= F_CONTENT_LENGTH;
    parser->content_length = v;
  } else if (header_name_is(name, name_len, "transfer-encoding")) {
    /* RFC 7230 SS3.2.2 treats repeated header field lines with the same
     * name as one concatenated comma-separated list, in order; a request's
     * Transfer-Encoding legitimately CAN span multiple header lines (e.g.
     * "Transfer-Encoding: gzip" followed by "Transfer-Encoding: chunked" is
     * a valid, if unusual, way of saying the merged list is
     * "gzip, chunked"). F_CHUNKED therefore reflects only whether the
     * MOST RECENTLY processed Transfer-Encoding line ends in "chunked",
     * re-derived from scratch on every line rather than only ever set
     * (never cleared) as an earlier version of this code did; a
     * chunked-final claim from an earlier line must NOT survive a later
     * line that changes the picture (see the two regression tests this
     * fix added: chunked-then-identity now correctly rejected,
     * gzip-then-chunked now correctly still accepted). Whether the FINAL
     * merged list actually ends in "chunked" can only be known once every
     * Transfer-Encoding line has been seen, i.e. at headers-complete time
     * (see the CHTTP1_ST_HEADERS blank-line branch below), not per line
     * here. */
    parser->flags |= F_TRANSFER_ENCODING;
    if (parser->type == CHTTP1_PARSE_REQUEST &&
        transfer_encoding_has_nonfinal_chunked(vstart, value_len)) {
      /* Unlike the cross-line case above, "chunked" appearing before the
       * end of a SINGLE line's own token list (e.g. "chunked, identity" on
       * one line) is unconditionally wrong regardless of what any other
       * Transfer-Encoding line says, so this is checked and rejected
       * immediately rather than deferred. */
      parser->reason = "chunked must be the last Transfer-Encoding token";
      return PH_ERROR;
    }
    if (value_ends_with_chunked(vstart, value_len))
      parser->flags |= F_CHUNKED;
    else
      parser->flags &= (uint16_t)~F_CHUNKED;
  } else if (header_name_is(name, name_len, "connection")) {
    parse_connection_tokens(parser, vstart, value_len);
  } else if (header_name_is(name, name_len, "expect")) {
    /* RFC 7231 SS5.1.1's only defined expectation value; see
     * chttp1_expects_continue()'s own doc comment for the full contract
     * (detection only; this parser performs no I/O and does not itself
     * send an interim "100 Continue" response). */
    if (value_len == 12 && strncasecmp(vstart, "100-continue", 12) == 0)
      parser->flags |= F_EXPECT_100_CONTINUE;
  }

  if ((parser->flags & F_CONTENT_LENGTH) && (parser->flags & F_CHUNKED)) {
    parser->reason =
        "Content-Length can't be present with chunked Transfer-Encoding";
    return PH_ERROR;
  }

  if (parser->settings && parser->settings->on_header) {
    int err =
        parser->settings->on_header(parser, name, name_len, vstart, value_len);
    if (err != 0) return PH_USER;
  }

  parser->header_count++;
  parser->total_header_bytes += contribution;
  return PH_OK;
}

/* ========================================================================== */
/*                         CHUNK SIZE LINE                                    */
/* ========================================================================== */

/*
 * chunk-size line = 1*HEXDIG [ ";" chunk-ext ] ; chunk-ext is opaque here
 *
 * Everything from the first ';' to the line's CRLF is treated as an
 * unvalidated, discarded span: chttpclient.c has no need to inspect chunk
 * extensions, so there is nothing to validate those bytes for. This parser
 * deliberately does not validate extension token/quoted-string grammar at
 * all; a conscious, low-risk scope reduction, not an oversight (the
 * discarded bytes are never consumed either way).
 */
static bool parse_chunk_size_line(chttp1_parser_t *parser) {
  const char *s = parser->line_buf;
  size_t len = parser->line_len;
  size_t hex_len = 0;
  while (hex_len < len && s[hex_len] != ';') hex_len++;

  uint64_t v;
  if (!parse_uint64_hex(s, hex_len, &v)) {
    parser->reason = "Invalid chunk size";
    return false;
  }
  if (parser->max_chunk_size_override && v > parser->max_chunk_size_override) {
    parser->flags |= F_CHUNK_TOO_LARGE;
    parser->reason = "Chunk size exceeds configured maximum";
    return false;
  }
  parser->content_length = v;
  return true;
}

/* ========================================================================== */
/*                         TERMINAL-OUTCOME HELPERS                           */
/* ========================================================================== */

static chttp1_errno_t fail(chttp1_parser_t *parser, const char *reason) {
  parser->reason = reason;
  parser->state = CHTTP1_ST_DEAD;
  return CHTTP1_ERROR;
}

/* Used when a deeper helper (process_header_line, parse_status_line, ...)
 * already set parser->reason before reporting failure. */
static chttp1_errno_t fail_already_set(chttp1_parser_t *parser) {
  parser->state = CHTTP1_ST_DEAD;
  return CHTTP1_ERROR;
}

static chttp1_errno_t user_error(chttp1_parser_t *parser) {
  parser->state = CHTTP1_ST_DEAD;
  if (!parser->reason) parser->reason = "Callback reported an error";
  return CHTTP1_USER;
}

/*
 * Fires on_message_complete and transitions to the paused/done terminal
 * state. p is the current read position (used to compute
 * chttp1_parser_consumed()); data is the start of the buffer passed to this
 * chttp1_parser_execute call.
 */
static chttp1_errno_t complete_message(chttp1_parser_t *parser, const char *p,
                                       const char *data) {
  if (parser->settings && parser->settings->on_message_complete) {
    int err = parser->settings->on_message_complete(parser);
    if (err != 0) return user_error(parser);
  }
  parser->consumed = (size_t)(p - data);
  parser->state = CHTTP1_ST_MESSAGE_DONE;
  return CHTTP1_PAUSED;
}

/* ========================================================================== */
/*                         LIFECYCLE                                          */
/* ========================================================================== */

void chttp1_settings_init(chttp1_settings_t *settings) {
  memset(settings, 0, sizeof(*settings));
}

static void parser_init_common(chttp1_parser_t *parser,
                               const chttp1_settings_t *settings,
                               chttp1_parser_type_t type) {
  memset(parser, 0, sizeof(*parser));
  parser->type = type;
  parser->state = CHTTP1_ST_FIRST_LINE;
  parser->finish_state = CHTTP1_FINISH_UNSAFE;
  parser->settings = settings;
}

void chttp1_parser_init(chttp1_parser_t *parser,
                        const chttp1_settings_t *settings) {
  parser_init_common(parser, settings, CHTTP1_PARSE_RESPONSE);
}

void chttp1_parser_init_request(chttp1_parser_t *parser,
                                const chttp1_settings_t *settings) {
  parser_init_common(parser, settings, CHTTP1_PARSE_REQUEST);
}

/* ========================================================================== */
/*                         MAIN EXECUTE LOOP                                  */
/* ========================================================================== */

chttp1_errno_t chttp1_parser_execute(chttp1_parser_t *parser, const char *data,
                                     size_t len) {
  const char *p = data;
  const char *end = data + len;

  if (parser->state == CHTTP1_ST_DEAD ||
      parser->state == CHTTP1_ST_MESSAGE_DONE)
    return CHTTP1_ERROR;

  while (p < end) {
    switch (parser->state) {
      case CHTTP1_ST_FIRST_LINE: {
        line_result_t lr = accumulate_line(parser, &p, end);
        if (lr == LINE_NEED_MORE) break;
        if (lr == LINE_TOO_LONG)
          return fail(parser, parser->type == CHTTP1_PARSE_REQUEST
                                  ? "Request line too long"
                                  : "Status line too long");
        if (lr == LINE_BAD_EOL) return fail(parser, "Expected CRLF");
        if (parser->type == CHTTP1_PARSE_REQUEST && parser->line_len == 0) {
          /* RFC 7230 SS3.5: ignore a leading blank line before the
           * request-line itself (some clients erroneously send a stray
           * CRLF after a POST body); see CHTTP1_MAX_LEADING_BLANK_LINES's
           * own comment for why this is bounded, not unconditional. Without
           * this, parse_request_line below would reject the empty line
           * outright (method_len == 0), hard-closing an otherwise-healthy
           * keep-alive/pipelined connection over one stray CRLF instead of
           * absorbing it. */
          if (++parser->leading_blank_lines > CHTTP1_MAX_LEADING_BLANK_LINES)
            return fail(parser, "Too many leading blank lines");
          parser->line_len = 0;
          break;
        }
        {
          /* The request/status line's own bytes count against the same
           * total_header_bytes budget a header line does (chttpsvr_config_t.
           * max_header_bytes is documented as bounding "request line + all
           * header lines", not just the headers that follow it); without
           * this, a request-target far larger than the configured cap
           * (bounded only by the much larger CHTTP1_MAX_LINE_LEN) would
           * still be accepted. */
          size_t contribution = parser->line_len + 2;
          if (parser->total_header_bytes + contribution >
              effective_max_total_header_bytes(parser))
            return fail(parser, parser->type == CHTTP1_PARSE_REQUEST
                                    ? "Request line too large"
                                    : "Status line too large");
          parser->total_header_bytes += contribution;
        }
        if (parser->type == CHTTP1_PARSE_REQUEST) {
          ph_result_t plr = parse_request_line(parser);
          if (plr == PH_ERROR) return fail_already_set(parser);
          if (plr == PH_USER) return user_error(parser);
        } else {
          if (!parse_status_line(parser)) return fail_already_set(parser);
        }
        parser->line_len = 0;
        parser->finish_state = CHTTP1_FINISH_UNSAFE;
        parser->state = CHTTP1_ST_HEADERS;
        break;
      }

      case CHTTP1_ST_HEADERS: {
        line_result_t lr = accumulate_line(parser, &p, end);
        if (lr == LINE_NEED_MORE) break;
        if (lr == LINE_TOO_LONG) return fail(parser, "Header line too long");
        if (lr == LINE_BAD_EOL) return fail(parser, "Expected CRLF");

        if (parser->line_len == 0) {
          /* Blank line: header block is done. */
          int hint = CHTTP1_HEADERS_HAS_BODY;
          if (parser->settings && parser->settings->on_headers_complete)
            hint = parser->settings->on_headers_complete(parser);
          bool want_divert = (hint == CHTTP1_HEADERS_DIVERT_BODY &&
                              parser->type == CHTTP1_PARSE_REQUEST);
          if (hint != CHTTP1_HEADERS_HAS_BODY &&
              hint != CHTTP1_HEADERS_NO_BODY && !want_divert)
            return user_error(parser);

          bool no_body = (hint == CHTTP1_HEADERS_NO_BODY);
          if (!no_body && parser->type == CHTTP1_PARSE_RESPONSE &&
              ((parser->status_code >= 100 && parser->status_code < 200) ||
               parser->status_code == 204 || parser->status_code == 304)) {
            /* RFC 7230 SS3.3: 1xx/204/304 never have a body. (100 Continue,
             * specifically: this parser reports it as the complete message,
             * same as any other no-body response, rather than automatically
             * restarting to keep parsing a second message on the same
             * parser instance for an interim 1xx response. chttpclient.c's
             * Tier 1 (chttp_do_internal, via chttp_request_t.expect_continue)
             * does now send "Expect: 100-continue" and does drive a second
             * message on the same connection after a "100 Continue" interim
             * response; but it does so with a FRESH chttp1_parser_t
             * instance for that second message (see chttpclient.c's
             * _chttp_read_message, which creates a new parser per call),
             * matching this codebase's own "fresh state per message"
             * convention, rather than needing this parser instance to loop
             * internally past the interim response. See
             * chttp1_expects_continue's own doc comment for why the server
             * side of this same feature is handled differently.) */
            no_body = true;
          }

          if (!no_body && parser->type == CHTTP1_PARSE_REQUEST &&
              (parser->flags & F_TRANSFER_ENCODING) &&
              !(parser->flags & F_CHUNKED)) {
            /* RFC 7230 SS3.3.3: for a REQUEST specifically (never a
             * response; see value_ends_with_chunked's own doc comment for
             * that side's separate, pre-existing, documented scope
             * reduction), a Transfer-Encoding whose final coding is not
             * "chunked" leaves the message length genuinely indeterminate;
             * a request has no EOF-delimited body-framing mode to fall
             * back to the way a response does (see the request/response
             * asymmetry documented where CHTTP1_ST_BODY_EOF is entered
             * below), so silently falling through to the "no framing
             * headers means no body" branch just below (the pre-existing
             * behavior here) let a front-end that disagrees about framing
             * desync from this parser: a conforming origin server MUST
             * reject such a request outright instead. Checked here, once
             * every Transfer-Encoding line has actually been seen (see
             * process_header_line's own comment for why this can't be
             * decided any earlier), not per individual header line. */
            return fail(parser,
                        "Transfer-Encoding present but final encoding is "
                        "not chunked");
          }

          if (no_body) {
            parser->finish_state = CHTTP1_FINISH_SAFE;
            return complete_message(parser, p, data);
          }

          if (parser->flags & F_CHUNKED) {
            parser->state = CHTTP1_ST_BODY_CHUNK_SIZE;
            /* An EOF arriving anywhere in the chunked body is a truncation:
             * chunked framing requires an explicit terminating 0-length
             * chunk, the peer disconnecting early is never a valid way to
             * end it. */
            parser->finish_state = CHTTP1_FINISH_UNSAFE;
            if (want_divert) {
              parser->consumed = (size_t)(p - data);
              return CHTTP1_HEADERS_ONLY;
            }
          } else if (parser->flags & F_CONTENT_LENGTH) {
            if (parser->content_length == 0) {
              parser->finish_state = CHTTP1_FINISH_SAFE;
              return complete_message(parser, p, data);
            }
            parser->state = CHTTP1_ST_BODY_CONTENT_LENGTH;
            /* Likewise: an EOF before exactly content_length bytes have
             * arrived is a truncation, not a valid completion. */
            parser->finish_state = CHTTP1_FINISH_UNSAFE;
            if (want_divert) {
              parser->consumed = (size_t)(p - data);
              return CHTTP1_HEADERS_ONLY;
            }
          } else if (parser->type == CHTTP1_PARSE_REQUEST) {
            /* RFC 7230 SS3.3: unlike a response, a request with neither
             * Content-Length nor chunked Transfer-Encoding simply has no
             * body at all; there is no EOF-delimited framing mode for
             * requests (the connection isn't even closing; the client is
             * the one sending). Nothing to divert either way. */
            parser->finish_state = CHTTP1_FINISH_SAFE;
            return complete_message(parser, p, data);
          } else {
            /* Neither Content-Length nor chunked: either an explicit
             * Transfer-Encoding whose last token isn't "chunked" (RFC 7230
             * SS3.3.3: for a response (never a request, handled in the
             * branch above instead) the body length is then determined by
             * reading until the connection closes) or no framing at all
             * (same EOF-delimited outcome). This is the ONE body-framing
             * mode where EOF is itself the valid, expected way to end the
             * message; see chttp1_should_keep_alive's needs_eof check,
             * which relies on finish_state staying CHTTP1_FINISH_SAFE_WITH_CB
             * for exactly this case and no other. */
            parser->state = CHTTP1_ST_BODY_EOF;
            parser->finish_state = CHTTP1_FINISH_SAFE_WITH_CB;
          }
        } else {
          ph_result_t ph = process_header_line(parser);
          if (ph == PH_ERROR) return fail_already_set(parser);
          if (ph == PH_USER) return user_error(parser);
          parser->line_len = 0;
        }
        break;
      }

      case CHTTP1_ST_BODY_CONTENT_LENGTH: {
        size_t avail = (size_t)(end - p);
        size_t take = (uint64_t)avail < parser->content_length
                          ? avail
                          : (size_t)parser->content_length;
        if (take > 0) {
          if (parser->settings && parser->settings->on_body) {
            int err = parser->settings->on_body(parser, p, take);
            if (err != 0) return user_error(parser);
          }
          p += take;
          parser->content_length -= take;
        }
        if (parser->content_length == 0) {
          parser->finish_state = CHTTP1_FINISH_SAFE;
          return complete_message(parser, p, data);
        }
        break;
      }

      case CHTTP1_ST_BODY_CHUNK_SIZE: {
        line_result_t lr = accumulate_line(parser, &p, end);
        if (lr == LINE_NEED_MORE) break;
        if (lr == LINE_TOO_LONG)
          return fail(parser, "Chunk size line too long");
        if (lr == LINE_BAD_EOL) return fail(parser, "Expected CRLF");
        if (!parse_chunk_size_line(parser)) return fail_already_set(parser);
        parser->line_len = 0;
        if (parser->content_length == 0) {
          parser->state = CHTTP1_ST_BODY_CHUNK_TRAILERS;
        } else {
          parser->state = CHTTP1_ST_BODY_CHUNK_DATA;
        }
        break;
      }

      case CHTTP1_ST_BODY_CHUNK_DATA: {
        size_t avail = (size_t)(end - p);
        size_t take = (uint64_t)avail < parser->content_length
                          ? avail
                          : (size_t)parser->content_length;
        if (take > 0) {
          if (parser->settings && parser->settings->on_body) {
            int err = parser->settings->on_body(parser, p, take);
            if (err != 0) return user_error(parser);
          }
          p += take;
          parser->content_length -= take;
        }
        if (parser->content_length == 0)
          parser->state = CHTTP1_ST_BODY_CHUNK_CRLF;
        break;
      }

      case CHTTP1_ST_BODY_CHUNK_CRLF: {
        line_result_t lr = accumulate_line(parser, &p, end);
        if (lr == LINE_NEED_MORE) break;
        if (lr == LINE_TOO_LONG || lr == LINE_BAD_EOL || parser->line_len != 0)
          return fail(parser, "Expected CRLF after chunk data");
        parser->state = CHTTP1_ST_BODY_CHUNK_SIZE;
        break;
      }

      case CHTTP1_ST_BODY_CHUNK_TRAILERS: {
        line_result_t lr = accumulate_line(parser, &p, end);
        if (lr == LINE_NEED_MORE) break;
        if (lr == LINE_TOO_LONG) return fail(parser, "Trailer line too long");
        if (lr == LINE_BAD_EOL) return fail(parser, "Expected CRLF");

        if (parser->line_len == 0) {
          parser->finish_state = CHTTP1_FINISH_SAFE;
          return complete_message(parser, p, data);
        }
        ph_result_t ph = process_header_line(parser);
        if (ph == PH_ERROR) return fail_already_set(parser);
        if (ph == PH_USER) return user_error(parser);
        parser->line_len = 0;
        break;
      }

      case CHTTP1_ST_BODY_EOF: {
        size_t avail = (size_t)(end - p);
        if (avail > 0) {
          if (parser->settings && parser->settings->on_body) {
            int err = parser->settings->on_body(parser, p, avail);
            if (err != 0) return user_error(parser);
          }
          p = end;
        }
        /* Only chttp1_parser_finish() (a real socket EOF) can end this
         * state; there is no in-content signal for "body is done" here. */
        break;
      }

      case CHTTP1_ST_MESSAGE_DONE:
      case CHTTP1_ST_DEAD:
      default:
        /* Unreachable in practice: CHTTP1_ST_MESSAGE_DONE and CHTTP1_ST_DEAD
         * are both already handled by the early-return check at the top of
         * this function (the hard "never call execute() again after
         * CHTTP1_PAUSED/an error" contract; see chttp1_parser.h). Defensive
         * only. */
        return CHTTP1_ERROR;
    }
  }
  return CHTTP1_OK;
}

/* ========================================================================== */
/*                         FINISH (EOF HANDLING)                              */
/* ========================================================================== */

chttp1_errno_t chttp1_parser_finish(chttp1_parser_t *parser) {
  if (parser->state == CHTTP1_ST_DEAD) return CHTTP1_ERROR;
  /* Already at a clean message boundary (reached either via
   * chttp1_parser_execute's own CHTTP1_PAUSED return, or via a PRIOR call to
   * this same function completing an EOF-delimited body below): per this
   * function's own documented contract ("already at a clean
   * CHTTP1_ST_MESSAGE_DONE boundary ... returns CHTTP1_OK"), a second call
   * is a no-op, not a re-fire. Without this check, the CHTTP1_FINISH_SAFE_
   * WITH_CB case below (which never updates finish_state, only state, since
   * chttp1_should_keep_alive's needs_eof check depends on finish_state
   * staying CHTTP1_FINISH_SAFE_WITH_CB forever after completion) would
   * re-enter that same branch on every subsequent call, re-invoking
   * on_message_complete each time, in violation of that callback's own
   * documented "fired exactly once" contract. */
  if (parser->state == CHTTP1_ST_MESSAGE_DONE) return CHTTP1_OK;

  switch (parser->finish_state) {
    case CHTTP1_FINISH_SAFE:
      return CHTTP1_OK;
    case CHTTP1_FINISH_SAFE_WITH_CB:
      if (parser->settings && parser->settings->on_message_complete) {
        int err = parser->settings->on_message_complete(parser);
        if (err != 0) return user_error(parser);
      }
      parser->state = CHTTP1_ST_MESSAGE_DONE;
      return CHTTP1_PAUSED;
    case CHTTP1_FINISH_UNSAFE:
    default:
      return fail(parser, "Invalid EOF state");
  }
}

/* ========================================================================== */
/*                         ACCESSORS                                          */
/* ========================================================================== */

size_t chttp1_parser_consumed(const chttp1_parser_t *parser) {
  return parser->consumed;
}

bool chttp1_parser_message_complete(const chttp1_parser_t *parser) {
  return parser->state == CHTTP1_ST_MESSAGE_DONE;
}

bool chttp1_should_keep_alive(const chttp1_parser_t *parser) {
  /* "HTTP/1.1 or later" is major > 1, or major == 1 with minor >= 1; NOT
   * "both major and minor are nonzero". The latter (this function's
   * previous condition) misclassifies any version whose minor happens to be
   * 0 while major is already >= 2 (e.g. a literal "HTTP/2.0" status line,
   * which this parser's grammar accepts, per parse_status_line's own doc
   * comment: "not hard-coded to 1.x... since chttp1_should_keep_alive
   * already needs to compare arbitrary major/minor values correctly
   * regardless") as HTTP/1.0-or-earlier, wrongly requiring an explicit
   * Connection: keep-alive token that a real server of that vintage would
   * never send, and silently disabling connection reuse against it. */
  bool is_1_1_or_later = parser->http_major > 1 ||
                         (parser->http_major == 1 && parser->http_minor >= 1);
  if (is_1_1_or_later) {
    /* HTTP/1.1 (or later): keep-alive by default unless Connection: close
     * was seen. */
    if (parser->flags & F_CONNECTION_CLOSE) return false;
  } else {
    /* HTTP/1.0 or earlier: NOT keep-alive by default unless
     * Connection: keep-alive was seen. */
    if (!(parser->flags & F_CONNECTION_KEEP_ALIVE)) return false;
  }

  /* An EOF-delimited body is never keep-alive eligible regardless of any
   * Connection header value: ending the body already required ending the
   * connection. finish_state is set to CHTTP1_FINISH_SAFE_WITH_CB in
   * exactly (and only) that one body-framing mode, and (unlike
   * parser->state, which has already moved on to CHTTP1_ST_MESSAGE_DONE by
   * the time this is ever meaningfully called; see this function's own doc
   * comment: only meaningful once the message is complete) stays that
   * value all the way through message completion, so it alone is sufficient
   * here. */
  bool needs_eof = (parser->finish_state == CHTTP1_FINISH_SAFE_WITH_CB);
  return !needs_eof;
}

bool chttp1_expects_continue(const chttp1_parser_t *parser) {
  return (parser->flags & F_EXPECT_100_CONTINUE) != 0;
}

bool chttp1_has_content_length(const chttp1_parser_t *parser) {
  return parser && (parser->flags & F_CONTENT_LENGTH) != 0;
}

uint64_t chttp1_declared_content_length(const chttp1_parser_t *parser) {
  return parser ? parser->content_length : 0;
}

bool chttp1_chunk_size_limit_exceeded(const chttp1_parser_t *parser) {
  return parser && (parser->flags & F_CHUNK_TOO_LARGE) != 0;
}

/* ========================================================================== */
/*                    WORKER-PULL BODY/RESPONSE STREAMING                     */
/* ========================================================================== */

static bool _stream_prepare_common(chttp1_stream_t *stream, int fd,
                                   void *tls_conn, const char *leftover,
                                   size_t leftover_len) {
  memset(stream, 0, sizeof(*stream));
  stream->fd = fd;
  stream->tls = tls_conn;
  stream->prepared = true;
  if (leftover_len == 0) return true;

  char *copy = (char *)malloc(leftover_len);
  if (!copy) {
    /* Leave stream genuinely safe to treat as never-prepared, matching this
     * function's own documented "zero-initialized-equivalent on failure"
     * contract: fd/tls/prepared were already set above, before the
     * allocation that just failed, and must be rolled back rather than left
     * half-committed. No current caller dereferences these fields without
     * first checking this function's return value, but this is the one
     * place that contract is actually established, and it should hold
     * regardless of what any particular caller happens to check today.
     * fd is rolled back to -1, not 0: 0 is a real, valid file descriptor
     * (stdin), so leaving it there would make a caller that ever skipped
     * the return-value check silently poll/read/write against fd 0 instead
     * of failing loudly; -1 matches this codebase's own "no fd" sentinel
     * convention everywhere else (e.g. chttp_conn_t.fd, chttp_async_ctx_t.
     * fd). */
    stream->fd = -1;
    stream->tls = NULL;
    stream->prepared = false;
    return false;
  }
  memcpy(copy, leftover, leftover_len);
  stream->carry = copy;
  stream->carry_len = leftover_len;
  stream->carry_pos = 0;
  return true;
}

bool chttp1_stream_prepare(chttp1_stream_t *stream, int fd,
                           const char *leftover, size_t leftover_len) {
  return _stream_prepare_common(stream, fd, NULL, leftover, leftover_len);
}

bool chttp1_stream_prepare_tls(chttp1_stream_t *stream, int fd, void *tls_conn,
                               const char *leftover, size_t leftover_len) {
  return _stream_prepare_common(stream, fd, tls_conn, leftover, leftover_len);
}

/* Shared by chttp1_stream_read/_write: blocks via poll(2) until fd is ready
 * for the requested direction, or the deadline elapses, or a signal
 * interrupts the wait (retried transparently; an interrupted poll(2) is
 * not a real timeout or error and must not be reported as either). Returns
 * true if fd is ready to proceed, false if the deadline elapsed (check
 * stream->timed_out, already set by this function) or a real poll(2) error
 * occurred (stream->last_errno already set). */
static bool wait_for_ready(chttp1_stream_t *stream, short events,
                           int timeout_ms) {
  struct pollfd pfd = {.fd = stream->fd, .events = events, .revents = 0};
  for (;;) {
    int rv = poll(&pfd, 1, timeout_ms);
    if (rv > 0) return true;
    if (rv == 0) {
      stream->timed_out = true;
      return false;
    }
    if (errno == EINTR) continue;
    stream->last_errno = errno;
    return false;
  }
}

/* Recomputes the remaining milliseconds until deadline (only meaningful
 * when has_deadline is true), clamped to >= 0; the caller treats <= 0 as
 * "already expired". Needed because a TLS stream's single logical read/
 * write may require several poll+attempt iterations (a partial TLS record,
 * or a renegotiation/key-update message OpenSSL consumes internally,
 * neither of which produces application bytes immediately), and each
 * iteration must wait no longer than what's left of the ORIGINAL timeout_ms
 * budget, not a fresh timeout_ms each time. */
static long _remaining_ms(const struct timespec *deadline) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (long)(deadline->tv_sec - now.tv_sec) * 1000 +
         (deadline->tv_nsec - now.tv_nsec) / 1000000L;
}

static void _compute_deadline(struct timespec *deadline, int timeout_ms) {
  clock_gettime(CLOCK_MONOTONIC, deadline);
  deadline->tv_sec += timeout_ms / 1000;
  deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (deadline->tv_nsec >= 1000000000L) {
    deadline->tv_nsec -= 1000000000L;
    deadline->tv_sec += 1;
  }
}

ssize_t chttp1_stream_read(chttp1_stream_t *stream, char *buf, size_t buflen,
                           int timeout_ms) {
  stream->timed_out = false;
  stream->last_errno = 0;

  if (stream->carry_pos < stream->carry_len) {
    size_t avail = stream->carry_len - stream->carry_pos;
    size_t take = avail < buflen ? avail : buflen;
    if (take > 0) memcpy(buf, stream->carry + stream->carry_pos, take);
    stream->carry_pos += take;
    if (stream->carry_pos == stream->carry_len) {
      free(stream->carry);
      stream->carry = NULL;
      stream->carry_len = 0;
      stream->carry_pos = 0;
    }
    return (ssize_t)take;
  }

  bool has_deadline = timeout_ms >= 0;
  struct timespec deadline;
  if (has_deadline) _compute_deadline(&deadline, timeout_ms);

  /* For a TLS stream specifically, attempt the read FIRST, before ever
   * consulting poll(2)/the deadline: OpenSSL may already be holding
   * decrypted application bytes in its own internal buffer (e.g. a caller
   * upstream of this stream already called ctls_conn_read() once with a
   * smaller buflen than the underlying TLS record actually contained, such
   * as during header parsing before this connection was ever handed off to
   * a worker), fully independent of whether the raw fd itself currently has
   * anything left to read from the kernel. Polling the raw fd first (the
   * previous shape, still used below for the plaintext case) made those
   * already-decrypted bytes invisible to poll(2), stalling for the full
   * timeout_ms budget despite data being immediately available; this also
   * happens to be what makes timeout_ms == 0 actually mean "return
   * immediately if fd is not already readable right now" (its own
   * documented contract) for a TLS stream, rather than "never even attempt
   * a read", since the very first attempt now always happens before the
   * deadline (already expired for timeout_ms == 0) is ever checked.
   *
   * This is NOT extended to the plaintext branch below: a plaintext
   * stream's fd is not guaranteed to be non-blocking (chttp1_stream_prepare,
   * unlike _prepare_tls, has no such requirement documented anywhere, and
   * this module's own test suite relies on it working correctly against a
   * genuine blocking socketpair, gated entirely by poll(2); attempting a
   * raw read(2) before ever confirming readiness reintroduces exactly the
   * hang this design avoids: a blocking fd with nothing available blocks
   * inside read(2) itself, silently ignoring timeout_ms altogether, a real,
   * reproducible hang caught by this file's own stream.
   * read_timeout_when_nothing_available test). A raw fd's own readiness,
   * unlike a TLS stream's, has no equivalent internal-buffering concern for
   * poll(2) to be blind to in the first place, so poll-first remains both
   * correct and necessary here. */
  if (stream->tls) {
    for (;;) {
      ssize_t got = ctls_conn_read((ctls_conn_t *)stream->tls, buf, buflen);
      if (got >= 0) return got;
      if (!(errno == EWOULDBLOCK || errno == EAGAIN)) {
        stream->last_errno = errno;
        return -1;
      }

      int this_timeout = timeout_ms;
      if (has_deadline) {
        long rem = _remaining_ms(&deadline);
        if (rem <= 0) {
          stream->timed_out = true;
          return -1;
        }
        this_timeout = (int)rem;
      }
      /* ctls_conn_read()'s own EWOULDBLOCK does not always mean "wait for
       * readable" the way a raw read(2)'s would: OpenSSL can need to WRITE
       * before this exact call can make progress (e.g. flushing a deferred
       * post-handshake session ticket, or a TLS 1.2 renegotiation); see
       * ctls_conn_wants_write's own doc comment. Waiting on POLLIN
       * unconditionally here would leave the connection stalled with no
       * further readiness event ever arriving in the direction it actually
       * still needs, until timeout_ms (or, for a caller configured with no
       * deadline at all, forever). */
      short want_events =
          ctls_conn_wants_write((ctls_conn_t *)stream->tls) ? POLLOUT : POLLIN;
      if (!wait_for_ready(stream, want_events, this_timeout)) return -1;
    }
  }

  /* Unlike the TLS branch above (a real retry loop that can genuinely burn
   * down the deadline across more than one iteration) or chttp1_stream_
   * write's own shared loop below, this plaintext path performs exactly one
   * wait, immediately after `deadline` was computed as "now + timeout_ms" a
   * few instructions up; re-deriving "time remaining until it" here via a
   * second clock_gettime() call would, for any timeout_ms > 0, only ever
   * reproduce timeout_ms itself minus a handful of nanoseconds of pure call
   * overhead (harmless), but for timeout_ms == 0 specifically, ANY nonzero
   * elapsed time already exceeds a zero-length budget, so that recomputed
   * value is unconditionally <= 0. That made this function report "already
   * timed out" without ever calling poll(2) or attempting read(2) at all,
   * even when the fd was already readable right now: a real violation of
   * this function's own documented timeout_ms == 0 contract ("return
   * immediately if fd is not already readable right now"), not merely a
   * missed optimisation. Passing timeout_ms straight through fixes this
   * (poll(2)'s own timeout_ms == 0 convention is exactly "one immediate,
   * non-blocking check") while behaving identically to before for every
   * timeout_ms > 0 or negative ("block forever") value. */
  if (!wait_for_ready(stream, POLLIN, timeout_ms)) return -1;

  ssize_t got = read(stream->fd, buf, buflen);
  if (got < 0) stream->last_errno = errno;
  return got;
}

ssize_t chttp1_stream_write(chttp1_stream_t *stream, const char *buf,
                            size_t len, int timeout_ms) {
  stream->timed_out = false;
  stream->last_errno = 0;

  bool has_deadline = timeout_ms >= 0;
  struct timespec deadline;
  if (has_deadline) _compute_deadline(&deadline, timeout_ms);

  /* The direction to poll for before the NEXT attempt; POLLOUT to start,
   * matching a plain write(2)'s own contract, but re-derived after every
   * TLS write attempt below; see that branch's own comment for why a
   * ctls_conn_write() EWOULDBLOCK can mean the opposite direction. */
  short want_events = POLLOUT;
  /* True only for this loop's very first iteration, whose `deadline` was
   * just computed as "now + timeout_ms" a few instructions up and therefore
   * cannot legitimately have already expired: recomputing "time remaining
   * until it" there anyway would, for timeout_ms == 0, treat the handful of
   * nanoseconds of pure call overhead since that computation as having
   * already exhausted a zero-length budget, reporting a timeout without
   * ever calling poll(2) or attempting the write below, even when the fd
   * was already writable right now; a real violation of this function's own
   * documented timeout_ms == 0 contract ("return immediately if fd is not
   * already writable right now"), not merely a missed optimisation. From
   * the SECOND iteration onward, a real wait_for_ready() call (and possibly
   * a real, if non-blocking, write attempt) has already consumed genuine
   * wall-clock time, so consulting the deadline there is both necessary
   * (bounding the TOTAL time this retry loop may spend, not just one
   * attempt) and correct. */
  bool first_attempt = true;
  for (;;) {
    int this_timeout = timeout_ms;
    if (has_deadline && !first_attempt) {
      long rem = _remaining_ms(&deadline);
      if (rem <= 0) {
        stream->timed_out = true;
        return -1;
      }
      this_timeout = (int)rem;
    }
    if (!wait_for_ready(stream, want_events, this_timeout)) return -1;
    first_attempt = false;

    if (stream->tls) {
      ssize_t written = ctls_conn_write((ctls_conn_t *)stream->tls, buf, len);
      if (written >= 0) return written;
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        /* ctls_conn_write()'s own EWOULDBLOCK does not always mean "wait
         * for writable": OpenSSL can need to READ before this exact call
         * can make progress (e.g. during a TLS 1.2 renegotiation); see
         * ctls_conn_wants_write's own doc comment. Always looping back to
         * wait on POLLOUT here would leave the connection stalled with no
         * further readiness event ever arriving in the direction it
         * actually still needs. */
        want_events = ctls_conn_wants_write((ctls_conn_t *)stream->tls)
                          ? POLLOUT
                          : POLLIN;
        continue;
      }
      stream->last_errno = errno;
      return -1;
    }

    ssize_t written = write(stream->fd, buf, len);
    if (written >= 0) return written;
    /* Mirrors the TLS branch above: wait_for_ready() confirming POLLOUT does
     * not guarantee a subsequent write(2) on a non-blocking fd (chttpserver.c
     * always accepts via accept4(..., SOCK_NONBLOCK)) cannot itself still
     * report EWOULDBLOCK/EAGAIN; loop back and wait again instead of
     * surfacing a transient non-error as a hard I/O failure. */
    if (errno == EWOULDBLOCK || errno == EAGAIN) continue;
    stream->last_errno = errno;
    return -1;
  }
}

bool chttp1_stream_timed_out(const chttp1_stream_t *stream) {
  return stream->timed_out;
}

int chttp1_stream_last_error(const chttp1_stream_t *stream) {
  return stream->last_errno;
}

bool chttp1_stream_push_back_leftover(chttp1_stream_t *stream, const char *buf,
                                      size_t len) {
  if (len == 0) return true;
  size_t existing = stream->carry_len - stream->carry_pos;
  /* Overflow guard on the "needed size" computation below, matching the
   * SIZE_MAX-relative guard idiom this codebase's other growable/
   * concatenated buffers already use for the identical class of
   * computation (e.g. chttpclient.c's _merge_ref_path/_concat_len): len and
   * existing are two independently sized values (a freshly read chunk and
   * whatever carry-over this stream already held), so their sum reaching
   * close to SIZE_MAX does not require either one alone to be implausibly
   * large. Unreachable in practice, but this project treats a reducible/
   * unguarded overflow in a size computation as a real bug regardless of
   * how large an input is needed to trigger it. */
  if (len > SIZE_MAX - existing) return false;
  size_t total = len + existing;
  char *nc = (char *)malloc(total);
  if (!nc) return false;
  memcpy(nc, buf, len);
  if (existing) memcpy(nc + len, stream->carry + stream->carry_pos, existing);
  free(stream->carry);
  stream->carry = nc;
  stream->carry_len = total;
  stream->carry_pos = 0;
  return true;
}

char *chttp1_stream_take_leftover(chttp1_stream_t *stream, size_t *len_out) {
  if (len_out) *len_out = 0;
  if (stream->carry_pos >= stream->carry_len) return NULL;
  size_t remaining = stream->carry_len - stream->carry_pos;
  char *out = (char *)malloc(remaining);
  if (!out)
    return NULL; /* stream's own carry is left untouched; still
                  * cleaned up normally by chttp1_stream_release */
  memcpy(out, stream->carry + stream->carry_pos, remaining);
  free(stream->carry);
  stream->carry = NULL;
  stream->carry_len = 0;
  stream->carry_pos = 0;
  if (len_out) *len_out = remaining;
  return out;
}

void chttp1_stream_release(chttp1_stream_t *stream) {
  if (stream->carry) {
    free(stream->carry);
    stream->carry = NULL;
  }
  stream->carry_len = 0;
  stream->carry_pos = 0;
  stream->released = true;
}
