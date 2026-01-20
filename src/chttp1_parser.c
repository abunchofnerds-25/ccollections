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
#include <stdint.h>
#include <string.h>
#include <strings.h>

/* ========================================================================== */
/*                         PRIVATE CONSTANTS                                  */
/* ========================================================================== */

/* Regular headers + trailers combined; see chttp1_parser.h's file-level doc
 * comment. Neither constant affects chttp1_parser_t's layout (unlike
 * CHTTP1_MAX_LINE_LEN, which is public precisely because it does), so both
 * stay private to this file. */
#define CHTTP1_MAX_HEADER_COUNT 100
#define CHTTP1_MAX_TOTAL_HEADER_BYTES (64 * 1024)

/* chttp1_parser_t.flags bits. */
#define F_CHUNKED 0x01u
#define F_CONTENT_LENGTH 0x02u
#define F_TRANSFER_ENCODING 0x04u
#define F_CONNECTION_CLOSE 0x08u
#define F_CONNECTION_KEEP_ALIVE 0x10u

/* ========================================================================== */
/*                         SMALL CHARACTER HELPERS                            */
/* ========================================================================== */

static bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

static bool is_ows(unsigned char c) { return c == ' ' || c == '\t'; }

/* RFC 7230 SS3.2.6 tchar: the set of bytes a header field NAME may contain. */
static bool is_tchar(unsigned char c) {
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

/* Same two-step (multiply-then-add, each checked) overflow-guard shape
 * verified correct in the vendored llhttp.c this module replaces; reimplemented
 * from first principles here (it is a standard, obvious technique, not an
 * llhttp-specific expression), for both radices this parser ever needs. */

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
 * Any '\n' not immediately preceded by '\r' is LINE_BAD_EOL -- this rejects
 * both a bare LF used as a line terminator and a bare LF embedded mid-line
 * (e.g. unfolded multi-line header content), which is exactly the strict
 * (this parser's only mode) behaviour: neither llhttp lenient flag that
 * would relax either case (LENIENT_OPTIONAL_LF_AFTER_CR,
 * LENIENT_OPTIONAL_CR_BEFORE_LF) is ever enabled anywhere in this codebase.
 * A bare CR that is NOT immediately followed by LF is deliberately not
 * specially rejected here -- it simply becomes ordinary line content, which
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

/* ========================================================================== */
/*                         STATUS LINE                                        */
/* ========================================================================== */

/*
 * status-line = HTTP-version SP status-code [ SP reason-phrase ]
 * HTTP-version = "HTTP/" DIGIT "." DIGIT
 *
 * Accepts any single-digit major/minor (not hard-coded to 1.x: this parser
 * doesn't restrict it beyond the grammar, matching llhttp, since
 * chttp1_should_keep_alive already needs to compare arbitrary major/minor
 * values correctly regardless) and exactly 3 decimal digits for the status
 * code with no numeric-range restriction (this project's own llhttp.h
 * HTTP_STATUS_MAP lists real, in-use nonstandard codes up to 599 and does
 * not claim that list is exhaustive; validate digit-count only and let the
 * caller judge magnitude, exactly as before).
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

/* ========================================================================== */
/*                         HEADER / TRAILER LINES                             */
/* ========================================================================== */

static bool header_name_is(const char *name, size_t name_len,
                           const char *literal) {
  size_t lit_len = strlen(literal);
  return name_len == lit_len && strncasecmp(name, literal, lit_len) == 0;
}

/* Whether the (already OWS-trimmed) Transfer-Encoding value's LAST
 * comma-separated token is "chunked" -- the only thing this parser needs to
 * know about Transfer-Encoding, since chttpclient.c never wires a callback
 * that would need to see individual encodings, and (per RFC 7230 SS3.3.3,
 * see _decide_body_framing's own comment) a non-final "chunked" only matters
 * for requests, which this parser never parses. */
static bool value_ends_with_chunked(const char *v, size_t len) {
  size_t tok_end = len;
  size_t tok_start = len;
  while (tok_start > 0 && v[tok_start - 1] != ',') tok_start--;
  while (tok_start < tok_end && is_ows((unsigned char)v[tok_start]))
    tok_start++;
  size_t tok_len = tok_end - tok_start;
  return tok_len == 7 && strncasecmp(v + tok_start, "chunked", 7) == 0;
}

/* Sets F_CONNECTION_CLOSE/F_CONNECTION_KEEP_ALIVE from a comma-separated
 * Connection header value. "upgrade" (the third token llhttp itself
 * recognised here) is deliberately not checked: this parser has no upgrade
 * handling at all (chttpclient.c never sends CONNECT or Upgrade requests),
 * so there would be nothing to do with that flag even if set. */
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

typedef enum { PH_OK, PH_ERROR, PH_USER } ph_result_t;

/*
 * Parses parser->line_buf[0..line_len) as one header/trailer line ("name:
 * value"), validates it (including the three size caps -- see
 * chttp1_parser.h's file-level doc comment -- and the framing-relevant
 * checks: duplicate Content-Length, Content-Length+chunked conflict,
 * Content-Length decimal overflow), updates framing flags for the three
 * header names this parser cares about, and invokes settings->on_header.
 * Shared verbatim between CHTTP1_ST_HEADERS and CHTTP1_ST_BODY_CHUNK_TRAILERS
 * -- there is no regular-vs-trailer distinction at this layer, matching
 * chttpclient.c's own existing behaviour of inserting both into the same
 * headers map.
 */
static ph_result_t process_header_line(chttp1_parser_t *parser) {
  size_t contribution =
      parser->line_len + 2; /* +2: the CRLF accumulate_line stripped */
  if (parser->header_count + 1 > CHTTP1_MAX_HEADER_COUNT) {
    parser->reason = "Too many headers";
    return PH_ERROR;
  }
  if (parser->total_header_bytes + contribution >
      CHTTP1_MAX_TOTAL_HEADER_BYTES) {
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
    if (!is_tchar((unsigned char)name[i])) {
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
    parser->flags |= F_TRANSFER_ENCODING;
    if (value_ends_with_chunked(vstart, value_len)) parser->flags |= F_CHUNKED;
  } else if (header_name_is(name, name_len, "connection")) {
    parse_connection_tokens(parser, vstart, value_len);
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
 * unvalidated, discarded span: chttpclient.c never wired
 * on_chunk_extension_name/value (llhttp had callbacks for them; this parser
 * doesn't), so there is nothing to validate those bytes for. This is
 * deliberately more lenient than upstream llhttp, which does validate
 * extension token/quoted-string grammar -- a conscious, low-risk scope
 * reduction, not an oversight (the discarded bytes are never consumed
 * either way).
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

void chttp1_parser_init(chttp1_parser_t *parser,
                        const chttp1_settings_t *settings) {
  memset(parser, 0, sizeof(*parser));
  parser->state = CHTTP1_ST_STATUS_LINE;
  parser->finish_state = CHTTP1_FINISH_UNSAFE;
  parser->settings = settings;
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
      case CHTTP1_ST_STATUS_LINE: {
        line_result_t lr = accumulate_line(parser, &p, end);
        if (lr == LINE_NEED_MORE) break;
        if (lr == LINE_TOO_LONG) return fail(parser, "Status line too long");
        if (lr == LINE_BAD_EOL) return fail(parser, "Expected CRLF");
        if (!parse_status_line(parser)) return fail_already_set(parser);
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
          int hint = 0;
          if (parser->settings && parser->settings->on_headers_complete)
            hint = parser->settings->on_headers_complete(parser);
          if (hint != 0 && hint != 1) return user_error(parser);

          bool no_body = (hint == 1);
          if (!no_body &&
              ((parser->status_code >= 100 && parser->status_code < 200) ||
               parser->status_code == 204 || parser->status_code == 304)) {
            /* RFC 7230 SS3.3: 1xx/204/304 never have a body. (100 Continue,
             * specifically: this parser reports it as the complete message,
             * same as any other no-body response, rather than replicating
             * llhttp's "silently restart and keep parsing a second message
             * on the same parser instance" handling for interim 1xx
             * responses -- chttpclient.c never sends "Expect:
             * 100-continue" and has no code path that would know what to do
             * with a second, later message on the same hop, so that
             * behaviour would be untested, unused complexity; a conscious
             * scope reduction, not an oversight.) */
            no_body = true;
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
          } else if (parser->flags & F_CONTENT_LENGTH) {
            if (parser->content_length == 0) {
              parser->finish_state = CHTTP1_FINISH_SAFE;
              return complete_message(parser, p, data);
            }
            parser->state = CHTTP1_ST_BODY_CONTENT_LENGTH;
            /* Likewise: an EOF before exactly content_length bytes have
             * arrived is a truncation, not a valid completion. */
            parser->finish_state = CHTTP1_FINISH_UNSAFE;
          } else {
            /* Neither Content-Length nor chunked: either an explicit
             * Transfer-Encoding whose last token isn't "chunked" (RFC 7230
             * SS3.3.3: for a response -- never a request, which this parser
             * never parses -- the body length is then determined by
             * reading until the connection closes) or no framing at all
             * (same EOF-delimited outcome). This is the ONE body-framing
             * mode where EOF is itself the valid, expected way to end the
             * message -- see chttp1_should_keep_alive's needs_eof check,
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
         * CHTTP1_PAUSED/an error" contract -- see chttp1_parser.h). Defensive
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

bool chttp1_should_keep_alive(const chttp1_parser_t *parser) {
  if (parser->http_major > 0 && parser->http_minor > 0) {
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
   * exactly (and only) that one body-framing mode, and -- unlike
   * parser->state, which has already moved on to CHTTP1_ST_MESSAGE_DONE by
   * the time this is ever meaningfully called (see this function's own doc
   * comment: only meaningful once the message is complete) -- stays that
   * value all the way through message completion, so it alone is sufficient
   * here. */
  bool needs_eof = (parser->finish_state == CHTTP1_FINISH_SAFE_WITH_CB);
  return !needs_eof;
}
