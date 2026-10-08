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

#include <errno.h>
#include <internal/chttp1_parser.h>
#include <internal/csock.h>
#include <internal/ctls.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

/* ========================================================================== */
/*                         PRIVATE CONSTANTS                                  */
/* ========================================================================== */

/* These caps cover the ordinary headers and the trailers together. See the
 * doc comment at the top of chttp1_parser.h. Neither constant changes the
 * layout of chttp1_parser_t. CHTTP1_MAX_LINE_LEN is different, and that is
 * exactly why it is public. Both of these therefore stay private to this
 * file. */
#define CHTTP1_MAX_HEADER_COUNT 100
#define CHTTP1_MAX_TOTAL_HEADER_BYTES (64 * 1024)

/* RFC 7230 SS3.5 recommends that a server ignore at least one empty line
 * before the request line. Some clients send such a stray CRLF after the
 * body of a POST. This parser bounds the number of those lines, and it does
 * not skip them without a limit. Every other cap in this file works the same
 * way. A client that never stops to send blank lines is therefore malformed
 * input, and the parser does not tolerate it forever. Response mode never
 * uses this cap, because a blank line before a status line matches no real
 * bug of a client. */
#define CHTTP1_MAX_LEADING_BLANK_LINES 25

/* chttp1_parser_t.flags bits. */
#define F_CHUNKED 0x01u
#define F_CONTENT_LENGTH 0x02u
#define F_TRANSFER_ENCODING 0x04u
#define F_CONNECTION_CLOSE 0x08u
#define F_CONNECTION_KEEP_ALIVE 0x10u
#define F_EXPECT_100_CONTINUE 0x20u
#define F_CHUNK_TOO_LARGE                            \
  0x40u /* the parser sets this bit with a rejection \
         * that max_chunk_size_override causes. See  \
         * chttp1_chunk_size_limit_exceeded() */
#define F_CHUNKED_APPLIED                                                 \
  0x80u /* a "chunked" transfer coding appeared somewhere in the merged   \
         * Transfer-Encoding list up to this point. See the rejection in  \
         * process_header_line of a coding that is applied more than one  \
         * time. This bit is not F_CHUNKED. F_CHUNKED tracks only whether \
         * the last Transfer-Encoding line ENDS in "chunked", and a later \
         * line that does not end in "chunked" clears it again. */
#define F_OTHER_CODING                                                  \
  0x100u /* the merged Transfer-Encoding list names a coding other than \
          * "chunked". See chttp1_has_other_transfer_coding(). */
#define F_HEADERS_DONE                                           \
  0x200u /* the header block ended and its framing is known; see \
          * chttp1_request_bytes_may_remain(). */

/* The chunk-size lines, chunk extensions and chunk-terminating CRLFs of one
 * message may carry at most this many bytes beyond what the data of the
 * chunks earns; see parse_chunk_size_line(). */
#define CHTTP1_MAX_CHUNK_OVERHEAD_EXCESS (16u * 1024u)

/* ========================================================================== */
/*                         SMALL CHARACTER HELPERS                            */
/* ========================================================================== */

static bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

static bool is_ows(unsigned char c) { return c == ' ' || c == '\t'; }

/* RFC 7230 SS3.2.6 tchar: the set of bytes that a header field NAME may
 * hold. This function is not static. chttp1_parser.h exposes it to
 * chttpclient.c and chttpserver.c. See the doc comment of that
 * declaration. */
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

/* Reports whether a header VALUE may not hold this byte. This parser has
 * one mode, and that mode is strict. It forbids every control character
 * other than HTAB. That includes a bare CR or a NUL that the line split left
 * behind as ordinary content. See the comment of accumulate_line() for why
 * it does not reject those bytes there. */
static bool is_invalid_value_byte(unsigned char c) {
  return (c < 0x20 && c != '\t') || c == 0x7f;
}

/* Reports whether a request-target may not hold this byte. RFC 7230 SS3.1.1
 * separates the three fields of the request-line with single spaces. SS5.3
 * allows no whitespace at all inside the target itself. This test is
 * therefore stricter than is_invalid_value_byte(). HTAB is ordinary content
 * in a header VALUE, and it is forbidden here. Without that rule,
 * "GET /a<HTAB>b" reaches the routing step as one target. A front-end that
 * splits on whitespace then reads a different request than this server
 * does. */
static bool is_invalid_target_byte(unsigned char c) {
  return c <= 0x20 || c == 0x7f;
}

/* ========================================================================== */
/*                         OVERFLOW-CHECKED INTEGER PARSING */
/* ========================================================================== */

/* This is the standard two-step overflow guard. It multiplies, then it
 * adds, and it checks each of the two steps. Both radices that this parser
 * needs use it. */

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

/* The buffer that holds the line that the parser accumulates, and its
 * capacity: the fixed line_buf, or the heap buffer of a response parser whose
 * line outgrew it; see chttp1_parser_enable_line_spill(). */
static inline char *line_ptr(chttp1_parser_t *parser) {
  return parser->line_spill ? parser->line_spill : parser->line_buf;
}

/* Moves the line that fills line_buf into a heap buffer of
 * CHTTP1_MAX_SPILL_LINE_LEN bytes, so that it can grow further. It returns
 * false when the parser may not spill, when the line could no longer fit in
 * the header budget anyway, or when the allocation fails. */
static bool line_spill_grow(chttp1_parser_t *parser,
                            size_t header_budget_left) {
  if (!parser->line_spill_enabled || parser->line_spill) return false;
  if (header_budget_left <= CHTTP1_MAX_LINE_LEN) return false;
  char *heap =
      (char *)_ccol_mem_alloc(parser->line_spill_mp, CHTTP1_MAX_SPILL_LINE_LEN);
  if (!heap) return false;
  memcpy(heap, parser->line_buf, parser->line_len);
  parser->line_spill = heap;
  return true;
}

/* The capacity of the heap line buffer for the line at hand: the whole
 * buffer, or less when the header budget left cannot hold a longer line
 * anyway, so that such a line is refused as soon as it passes the budget
 * and not only once the buffer fills. */
static inline size_t spill_cap(size_t spill_budget) {
  return spill_budget && spill_budget < CHTTP1_MAX_SPILL_LINE_LEN
             ? spill_budget
             : (size_t)CHTTP1_MAX_SPILL_LINE_LEN;
}

/*
 * Appends bytes from *pp, up to end, to the line buffer of the parser (see
 * line_ptr()). It stops when it has a full line that ends with CRLF, when the
 * buffer fills up, or when the input runs out.
 *
 * The code always advances *pp to show exactly how much it consumed. For
 * LINE_NEED_MORE and LINE_TOO_LONG that is end. For LINE_READY and
 * LINE_BAD_EOL it is one byte past the byte that ends the line. This matches
 * the "always consume everything" contract of chttp1_parser_execute() for
 * the cases that are not terminal.
 *
 * On LINE_READY, parser->line_len is the content length of the line, with
 * the trailing CRLF already removed. On every other result, parser->line_len
 * keeps whatever the code accumulated so far. On LINE_TOO_LONG that is the
 * full capacity of the buffer.
 *
 * spill_budget is 0 for a line that must fit in the fixed line_buf. For a
 * header or trailer line of a response parser that may spill, it is what is
 * left of the header byte budget; a line that fills line_buf then moves to a
 * heap buffer of CHTTP1_MAX_SPILL_LINE_LEN bytes when the budget can still
 * hold a longer line.
 *
 * Every '\n' that does not come directly after a '\r' gives LINE_BAD_EOL.
 * This rejects a bare LF used to end a line. It also rejects a bare LF in
 * the middle of a line, for example unfolded content of a header that spans
 * several lines. That is the strict behaviour, and strict is the only mode
 * of this parser. No lenient mode anywhere in this codebase relaxes either
 * case. Such a mode would accept a bare LF, or a CR that no LF follows, as a
 * valid way to end a line.
 *
 * The code does not reject a bare CR that no LF follows. Such a CR becomes
 * ordinary line content. The caller then catches it with its own check for a
 * control character in that content. is_invalid_value_byte() and the scan of
 * the reason phrase in parse_status_line() do that. This layer therefore
 * does not repeat the check.
 *
 * The loop keeps the buffer, its capacity and the length in locals. A store
 * through the char buffer may alias any field of the parser, so a loop that
 * works on the fields reloads them after every byte.
 */
static line_result_t accumulate_line(chttp1_parser_t *parser, const char **pp,
                                     const char *end, size_t spill_budget) {
  const char *p = *pp;
  char *buf = line_ptr(parser);
  size_t cap = parser->line_spill ? spill_cap(spill_budget)
                                  : (size_t)CHTTP1_MAX_LINE_LEN;
  size_t len = parser->line_len;
  while (p < end) {
    if (len >= cap) {
      parser->line_len = len;
      if (spill_budget && line_spill_grow(parser, spill_budget)) {
        buf = parser->line_spill;
        cap = spill_cap(spill_budget);
        continue;
      }
      /* The contract of this function is that *pp shows exactly how much the
       * call consumed, and that a result which is not a completed line
       * consumes everything. A stop at the current p would report that the
       * unread tail is still pending on a parser that can never read it: the
       * only caller of this result turns it straight into a terminal
       * failure. Reporting `end` keeps consumed() meaningful for every
       * result of this function rather than for some of them. */
      *pp = end;
      return LINE_TOO_LONG;
    }
    char c = *p++;
    buf[len++] = c;
    if (c == '\n') {
      if (len < 2 || buf[len - 2] != '\r') {
        parser->line_len = len;
        *pp = p;
        return LINE_BAD_EOL;
      }
      parser->line_len = len - 2;
      *pp = p;
      return LINE_READY;
    }
  }
  parser->line_len = len;
  *pp = p;
  return LINE_NEED_MORE;
}

/* parse_request_line() and process_header_line() below both use this. Each
 * needs to tell a rejection of malformed input apart from an error that a
 * callback of the application reports. */
typedef enum { PH_OK, PH_ERROR, PH_USER } ph_result_t;

/* ========================================================================== */
/*                         STATUS LINE                                        */
/* ========================================================================== */

/*
 * status-line = HTTP-version SP status-code [ SP reason-phrase ]
 * HTTP-version = "HTTP/" DIGIT "." DIGIT
 *
 * This accepts any single digit as the major and as the minor. It is not
 * fixed to 1.x. This parser restricts nothing beyond the grammar, because
 * chttp1_should_keep_alive() must already compare any major and minor values
 * correctly.
 *
 * It accepts exactly 3 decimal digits for the status code, with no limit on
 * the numeric range. Real nonstandard status codes that are in use go up to
 * 599, and even that is not a complete list. The code therefore checks the
 * number of digits only. The caller judges the magnitude.
 *
 * The reason phrase, and the single space before it, are BOTH optional. The
 * ABNF of RFC 7230 requires a trailing SP even for an empty reason phrase.
 * But real servers often leave it out completely, for example in
 * "HTTP/1.1 304\r\n". To reject that breaks interoperability with those
 * servers.
 */
static bool parse_status_line(chttp1_parser_t *parser) {
  const char *s = line_ptr(parser);
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
 * The code checks that method is a token of tchar bytes only. That is the
 * same character class that a header field name uses, per RFC 7230 SS3.1.1
 * and SS3.2.6. It does not compare method against any set of known method
 * names. A method that the server does not know is a routing concern of the
 * caller, which usually answers it with an ordinary 405. This parser does
 * not reject it.
 *
 * For request-target the code checks only that it holds no control
 * character, no DEL and no whitespace. See is_invalid_target_byte(). This
 * parser does not tell origin-form apart from absolute-form,
 * authority-form and asterisk-form. That matches the scope of chttpserver,
 * which is an origin server with no CONNECT support and no proxy support.
 *
 * HTTP-version uses the same grammar and the same checks as the version
 * field of parse_status_line().
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
    if (is_invalid_target_byte((unsigned char)target[j])) {
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

static bool is_hexdig(unsigned char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* unreserved and sub-delims of RFC 3986 SS2.2 and SS2.3. */
static bool is_unreserved_or_sub_delim(unsigned char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c))
    return true;
  switch (c) {
    case '-':
    case '.':
    case '_':
    case '~':
    case '!':
    case '$':
    case '&':
    case '\'':
    case '(':
    case ')':
    case '*':
    case '+':
    case ',':
    case ';':
    case '=':
      return true;
    default:
      return false;
  }
}

/* Reports whether a Host field-value is a valid uri-host with an optional
 * port, as RFC 9112 SS3.2 defines it through RFC 3986 SS3.2.2 and SS3.2.3:
 *
 *   Host       = uri-host [ ":" port ]
 *   uri-host   = IP-literal / IPv4address / reg-name
 *   IP-literal = "[" ( IPv6address / IPvFuture ) "]"
 *   reg-name   = *( unreserved / pct-encoded / sub-delims )
 *   port       = *DIGIT
 *
 * RFC 9112 SS3.2 tells a server to answer 400 for "a Host header field with
 * an invalid field-value". A value outside this grammar names an authority
 * that recipients can read differently: "a/b" or "a?b" splits into a host
 * and a path or a query, "u@h" carries userinfo, and whitespace makes two
 * tokens of one. A front end that reads one authority and a back end that
 * reads another is what request smuggling and cache poisoning need.
 *
 * An IPv4address is a reg-name as far as the characters go, so it takes
 * the reg-name branch. Inside the brackets of an IP-literal the check keeps
 * to the characters: HEXDIG, ":" and "." for an IPv6address, with at least
 * one ":", and the IPvFuture form "v" 1*HEXDIG "." followed by unreserved,
 * sub-delims and ":". An empty host is refused, because it names no
 * authority at all. */
static bool host_value_is_valid(const char *v, size_t len) {
  size_t i = 0;
  if (len == 0) return false;
  if (v[0] == '[') {
    size_t close = 1;
    while (close < len && v[close] != ']') close++;
    if (close >= len || close == 1) return false;
    const char *lit = v + 1;
    size_t lit_len = close - 1;
    if (lit[0] == 'v' || lit[0] == 'V') {
      size_t j = 1;
      while (j < lit_len && is_hexdig((unsigned char)lit[j])) j++;
      if (j == 1 || j >= lit_len || lit[j] != '.') return false;
      j++;
      if (j >= lit_len) return false;
      for (; j < lit_len; j++) {
        unsigned char c = (unsigned char)lit[j];
        if (!is_unreserved_or_sub_delim(c) && c != ':') return false;
      }
    } else {
      bool colon = false;
      for (size_t j = 0; j < lit_len; j++) {
        unsigned char c = (unsigned char)lit[j];
        if (c == ':')
          colon = true;
        else if (!is_hexdig(c) && c != '.')
          return false;
      }
      if (!colon) return false;
    }
    i = close + 1;
  } else {
    while (i < len && v[i] != ':') {
      unsigned char c = (unsigned char)v[i];
      if (c == '%') {
        if (i + 2 >= len || !is_hexdig((unsigned char)v[i + 1]) ||
            !is_hexdig((unsigned char)v[i + 2]))
          return false;
        i += 3;
        continue;
      }
      if (!is_unreserved_or_sub_delim(c)) return false;
      i++;
    }
    if (i == 0) return false;
  }
  if (i == len) return true;
  if (v[i] != ':') return false;
  for (i++; i < len; i++)
    if (!is_digit((unsigned char)v[i])) return false;
  return true;
}

static bool header_name_is(const char *name, size_t name_len,
                           const char *literal) {
  size_t lit_len = strlen(literal);
  return name_len == lit_len && strncasecmp(name, literal, lit_len) == 0;
}

/* Walks the comma-separated transfer-codings of one Transfer-Encoding header
 * value once, and reports the three things that the framing rules need to
 * know about "chunked" on that line. The comparison ignores case. The caller
 * trims the OWS of the whole value first. Like the rest of this parser the
 * scan allocates nothing and copies nothing.
 *
 * ONE function answers all three questions on purpose. Two scanners that
 * walked the same bytes with their own idea of where a token begins and ends
 * could disagree about the same header, and the two answers drive opposite
 * decisions: whether the message is framed as chunked, and whether it is
 * refused outright. A disagreement between them is exactly the ambiguity that
 * request smuggling needs.
 *
 * An EMPTY list element is ignored and never counts as a token. RFC 7230
 * SS7 says that a recipient must parse and ignore a reasonable number of
 * empty list elements, and RFC 7230 SS4.1 gives the same treatment to the
 * "#rule" list syntax that Transfer-Encoding uses. "chunked," and
 * ", chunked" and "chunked, ," therefore all name the one-element list
 * "chunked". Without that rule, the trailing comma of "chunked," makes the
 * empty element the final token. The list then does not end in "chunked",
 * and a response takes the read-until-close framing of RFC 7230 SS3.3.3
 * while every recipient that follows SS7 de-chunks it. The two then
 * disagree about where the message ends.
 *
 * *chunked_count receives how many codings of this line are "chunked". It
 * saturates at 2. The only question that anything asks of it is whether a
 * sender applies chunked more than once. RFC 7230 SS3.3.1 says: "A sender
 * MUST NOT apply chunked more than once to a message body". A count past 2
 * has no consumer. Two applications make the framing of the body ambiguous
 * in exactly the way that a disagreement between a front-end and a back-end
 * needs. The library therefore refuses such a message in BOTH modes.
 *
 * The caller must accumulate the count ACROSS lines. RFC 7230 SS3.2.2 merges
 * repeated Transfer-Encoding lines into one list. A second "chunked" is
 * equally forbidden whether or not it shares a line with the first one.
 *
 * *nonfinal_chunked receives whether "chunked" appears among the codings of
 * this line at a place that is not the last one. RFC 7230 SS3.3.1 needs
 * "chunked" to be the final transfer-coding of a REQUEST. Request mode
 * therefore rejects that shape. See
 * chunked_not_last_in_transfer_encoding_list_rejected in
 * tests/chttpserver/tests.c.
 *
 * The same shape is not a fault in a RESPONSE. RFC 7230 SS3.3.1 lets a
 * sender apply a coding that is not chunked last, as long as it then ends
 * the message by a close of the connection. RFC 7230 SS3.3.3 frames exactly
 * that response by a read until the close. A response whose merged
 * Transfer-Encoding list does not end in "chunked" is therefore framed by
 * EOF, and not rejected. To reject it for that alone refuses a message that
 * the spec gives a framing for.
 *
 * *ends_with_chunked receives whether the LAST non-empty token of this line
 * is "chunked". That is what decides the framing, and the caller must derive
 * it again on every Transfer-Encoding line that names a coding; see the
 * F_CHUNKED handling in process_header_line.
 *
 * *names_a_coding receives whether this line holds at least one non-empty
 * token. A line that holds none, such as "Transfer-Encoding:" or
 * "Transfer-Encoding: ,", adds only empty elements to the merged list, and
 * RFC 7230 SS7 has a recipient ignore those. Such a line therefore
 * leaves the final coding of the merged list where the earlier lines put
 * it, and *ends_with_chunked (false for it) must not be applied. The answer
 * comes from this one scanner, like every other answer about the list, so
 * that no second notion of where a token starts can disagree with it.
 *
 * *names_other_coding receives whether this line names a coding that is not
 * "chunked". This parser decodes no coding but chunked. A request that
 * applies another one hands its caller a body that is still encoded, so the
 * caller refuses it; see chttp1_has_other_transfer_coding().
 *
 * A response is refused for one more thing: an AMBIGUOUS framing. That is
 * the same Transfer-Encoding together with a Content-Length. See the place
 * where chttp1_parser_execute() enters CHTTP1_ST_BODY_EOF. */
static void transfer_encoding_scan(
    const char *v, size_t len, unsigned *chunked_count, bool *nonfinal_chunked,
    bool *ends_with_chunked, bool *names_a_coding, bool *names_other_coding) {
  unsigned count = 0;
  bool nonfinal = false;
  bool any_token = false;
  bool other = false;
  /* True when the most recent NON-EMPTY token was "chunked". At the end of
   * the walk it is the answer for *ends_with_chunked. During the walk, a
   * later non-empty token proves that this "chunked" was not the final one. */
  bool last_token_was_chunked = false;
  size_t i = 0;
  while (i < len) {
    size_t start = i;
    while (i < len && v[i] != ',') i++;
    size_t tok_end = i;
    while (tok_end > start && is_ows((unsigned char)v[tok_end - 1])) tok_end--;
    size_t ts = start;
    while (ts < tok_end && is_ows((unsigned char)v[ts])) ts++;
    size_t tok_len = tok_end - ts;
    if (i < len) i++;           /* skip the comma */
    if (tok_len == 0) continue; /* an empty list element names no coding */
    bool is_chunked = (tok_len == 7 && strncasecmp(v + ts, "chunked", 7) == 0);
    if (last_token_was_chunked) nonfinal = true;
    if (is_chunked && count < 2) count++;
    if (!is_chunked) other = true;
    last_token_was_chunked = is_chunked;
    any_token = true;
  }
  *chunked_count = count;
  *nonfinal_chunked = nonfinal;
  *ends_with_chunked = last_token_was_chunked;
  *names_a_coding = any_token;
  *names_other_coding = other;
}

/* Sets F_CONNECTION_CLOSE or F_CONNECTION_KEEP_ALIVE from a comma-separated
 * Connection header value. The code does not check for an "upgrade" token.
 * This parser has no handling for an upgrade at all, and chttpclient.c never
 * sends a CONNECT request or an Upgrade request. There would be nothing to
 * do with such a flag. */
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
 * Parses parser->line_buf[0..line_len) as one header line or one trailer
 * line, in the form "name: value", and checks it. The checks are the three
 * size caps, the grammar of the name, which is a token, and the grammar of
 * the value, which holds no control character. The file-level doc comment of
 * chttp1_parser.h describes the caps. Both kinds of line get the same checks
 * and the same charge against the caps. is_trailer decides everything else.
 *
 * For a header line, where is_trailer is false, the code also runs the
 * checks that matter for the framing. Those are a duplicate Content-Length,
 * a conflict between Content-Length and chunked, and a decimal overflow of
 * Content-Length. It updates the framing flags, the connection flags and the
 * expectation flags for the four header names that this parser reads. It
 * then calls settings->on_header.
 *
 * For a trailer line, where is_trailer is true, the code touches none of
 * that state, and it calls settings->on_trailer instead. RFC 7230 SS4.1.2
 * puts trailer fields after the point where the framing that they would
 * describe is already settled and acted on. A trailer that could set
 * Content-Length, change Transfer-Encoding, add a Connection token or raise
 * an Expect would let a peer rewrite the view that this parser has of a
 * message that it already framed. Here such a trailer is inert by
 * construction.
 *
 * The separate callback is the other half of the same property. A caller
 * that keeps one collection of headers cannot have a real header replaced by
 * a trailer field of the same name that it never told apart.
 */
/* process_header_line() and the CHTTP1_ST_FIRST_LINE case below both use
 * this. The bytes that the request line or the status line adds to
 * total_header_bytes therefore go against the same effective cap as the
 * bytes of a header line. */
static size_t effective_max_total_header_bytes(const chttp1_parser_t *parser) {
  return parser->max_total_header_bytes_override
             ? parser->max_total_header_bytes_override
             : CHTTP1_MAX_TOTAL_HEADER_BYTES;
}

/* What accumulate_line() gets as its spill budget for a header or trailer
 * line: 0 unless the parser may spill a long line to the heap, and otherwise
 * what is left of the header byte budget. */
static size_t header_spill_budget(const chttp1_parser_t *parser) {
  if (!parser->line_spill_enabled) return 0;
  size_t max = effective_max_total_header_bytes(parser);
  return max > parser->total_header_bytes ? max - parser->total_header_bytes
                                          : 0;
}

static ph_result_t process_header_line(chttp1_parser_t *parser,
                                       bool is_trailer) {
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

  char *line = line_ptr(parser);
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

  if (is_trailer) {
    if (parser->settings && parser->settings->on_trailer) {
      int err = parser->settings->on_trailer(parser, name, name_len, vstart,
                                             value_len);
      if (err != 0) return PH_USER;
    }
    parser->header_count++;
    parser->total_header_bytes += contribution;
    return PH_OK;
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
    /* RFC 7230 SS3.2.2 treats repeated header field lines of the same name
     * as one comma-separated list, joined in order. The Transfer-Encoding of
     * a request CAN therefore span several header lines. For example,
     * "Transfer-Encoding: gzip" followed by "Transfer-Encoding: chunked" is
     * a valid, if unusual, way to say that the merged list is
     * "gzip, chunked".
     *
     * F_CHUNKED therefore shows only whether the Transfer-Encoding line
     * that the code processed MOST RECENTLY, among the lines that name at
     * least one coding, ends in "chunked". The code derives it again from
     * nothing on every such line. It does not set the flag once and then
     * never clear it. A claim of a final chunked from an earlier line must
     * NOT survive a later line that changes the picture. That is what makes
     * chunked-then-identity a rejection and gzip-then-chunked an acceptance.
     *
     * A line that names no coding, such as "Transfer-Encoding:" or
     * "Transfer-Encoding: ,", contributes only empty list elements, which a
     * recipient ignores (RFC 7230 SS7). It leaves F_CHUNKED as it is.
     * "chunked" and then an empty line is therefore the list "chunked", as
     * the single line "chunked," is. Clearing the flag there frames a
     * chunked response by EOF, and refuses a chunked request, while every
     * recipient that ignores empty elements de-chunks the same message.
     *
     * Whether the FINAL merged list ends in "chunked" is known only after
     * the code sees every Transfer-Encoding line. That is at the moment
     * when the headers are complete; see the blank-line branch of
     * CHTTP1_ST_HEADERS below. It is not known for each line here. */
    parser->flags |= F_TRANSFER_ENCODING;
    unsigned line_chunked_count;
    bool line_has_nonfinal_chunked;
    bool line_ends_with_chunked;
    bool line_names_a_coding;
    bool line_names_other_coding;
    transfer_encoding_scan(vstart, value_len, &line_chunked_count,
                           &line_has_nonfinal_chunked, &line_ends_with_chunked,
                           &line_names_a_coding, &line_names_other_coding);
    if (line_names_other_coding) parser->flags |= F_OTHER_CODING;
    if (line_chunked_count > 1 ||
        (line_chunked_count > 0 && (parser->flags & F_CHUNKED_APPLIED))) {
      /* RFC 7230 SS3.3.1 says that a sender must not apply chunked more
       * than once to one message body. The code refuses both spellings of
       * that, in both modes. The first spelling is twice in the list of one
       * line, as in "chunked, chunked". The second is once per line across
       * two Transfer-Encoding lines that RFC 7230 SS3.2.2 merges into one
       * list, as in "chunked, gzip" and then "chunked". F_CHUNKED_APPLIED
       * carries the first occurrence across lines. F_CHUNKED cannot do
       * that, because a line that does not end in "chunked" clears it
       * again.
       *
       * The framing is ambiguous either way. A recipient that de-chunks
       * once and a recipient that de-chunks twice disagree about where the
       * body ends. That disagreement is what request smuggling and response
       * smuggling need. The code therefore refuses the message and does not
       * resolve it in either direction.
       *
       * It refuses the message in response mode too. A "chunked" that is
       * not final is otherwise legitimate there; see the comment of
       * transfer_encoding_scan(). But two applications of the
       * coding are forbidden wherever the two occurrences sit in the
       * list. */
      parser->reason = "chunked can't be applied more than once";
      return PH_ERROR;
    }
    if (line_chunked_count > 0) parser->flags |= F_CHUNKED_APPLIED;
    if (parser->type == CHTTP1_PARSE_REQUEST && line_has_nonfinal_chunked) {
      /* This differs from the case across lines above. A "chunked" that
       * appears before the end of the token list of a SINGLE line is always
       * wrong for a request, for example "chunked, identity" on one line.
       * No other Transfer-Encoding line can change that. The code therefore
       * checks it and rejects it at once, and does not defer it. */
      parser->reason = "chunked must be the last Transfer-Encoding token";
      return PH_ERROR;
    }
    if (line_ends_with_chunked)
      parser->flags |= F_CHUNKED;
    else if (line_names_a_coding)
      parser->flags &= (uint16_t)~F_CHUNKED;
  } else if (parser->type == CHTTP1_PARSE_REQUEST &&
             header_name_is(name, name_len, "host")) {
    /* RFC 7230 SS5.4. The count is what matters, and the blank line that
     * ends the header block is where the code acts on it; see the
     * CHTTP1_ST_HEADERS case of chttp1_parser_execute(). The count is
     * final only there. The field saturates, because no reader asks for
     * more than "0, 1 or many".
     *
     * The value check runs here instead, on each line, because it is a
     * property of that one line and needs no other line to decide it.
     *
     * Response mode never reaches this branch. A Host header in a response
     * is an ordinary, meaningless header field, and the framing of a
     * response does not depend on it. */
    if (parser->host_count < 2) parser->host_count++;
    if (!host_value_is_valid(vstart, value_len)) {
      parser->reason = "Invalid Host header value";
      return PH_ERROR;
    }
  } else if (header_name_is(name, name_len, "connection")) {
    parse_connection_tokens(parser, vstart, value_len);
  } else if (header_name_is(name, name_len, "expect")) {
    /* This is the only expectation value that RFC 7231 SS5.1.1 defines. See
     * the doc comment of chttp1_expects_continue() for the full contract.
     * This parser only detects the value. It does no I/O, and it never sends
     * an interim "100 Continue" response itself. */
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
 * RFC 9112 SS7.1.1:
 *
 *   chunk-ext      = *( BWS ";" BWS chunk-ext-name
 *                       [ BWS "=" BWS chunk-ext-val ] )
 *   chunk-ext-name = token
 *   chunk-ext-val  = token / quoted-string
 *
 * BWS is optional whitespace that a recipient must accept and ignore. This
 * library reads no chunk extension, and it does not hand one to the caller.
 * It still checks the whole grammar, because every recipient on the path
 * has to agree on where the chunk-size line ends and what it says. A parser
 * that skips everything after ";" accepts a bare CR, a NUL or any other
 * control byte there. A recipient that reads the same bytes more strictly
 * then frames the body differently, which is the disagreement that request
 * smuggling and response smuggling need.
 *
 * Returns true when s[i..len) is a well-formed chunk-ext. An empty span is
 * one, because the grammar allows zero extensions.
 */
static bool chunk_ext_is_valid(const char *s, size_t i, size_t len) {
  while (i < len) {
    while (i < len && is_ows((unsigned char)s[i])) i++;
    if (i >= len || s[i] != ';') return false;
    i++;
    while (i < len && is_ows((unsigned char)s[i])) i++;
    size_t name_start = i;
    while (i < len && chttp1_is_tchar((unsigned char)s[i])) i++;
    if (i == name_start) return false;
    size_t after_name = i;
    while (i < len && is_ows((unsigned char)s[i])) i++;
    if (i >= len || s[i] != '=') {
      /* No value. Whitespace that is not followed by another ";" is not
       * part of the grammar, so rewind to the end of the name and let the
       * loop demand the ";" of the next extension. */
      i = after_name;
      continue;
    }
    i++;
    while (i < len && is_ows((unsigned char)s[i])) i++;
    if (i >= len) return false;
    if (s[i] == '"') {
      /* quoted-string = DQUOTE *( qdtext / quoted-pair ) DQUOTE
       * qdtext        = HTAB / SP / %x21 / %x23-5B / %x5D-7E / obs-text
       * quoted-pair   = "\" ( HTAB / SP / VCHAR / obs-text ) */
      i++;
      for (;;) {
        if (i >= len) return false;
        unsigned char c = (unsigned char)s[i];
        if (c == '"') {
          i++;
          break;
        }
        if (c == '\\') {
          i++;
          if (i >= len || is_invalid_value_byte((unsigned char)s[i]))
            return false;
          i++;
          continue;
        }
        if (is_invalid_value_byte(c)) return false;
        i++;
      }
    } else {
      size_t val_start = i;
      while (i < len && chttp1_is_tchar((unsigned char)s[i])) i++;
      if (i == val_start) return false;
    }
  }
  return true;
}

/*
 * chunk-size line = 1*HEXDIG chunk-ext
 *
 * The size is the run of hex digits at the start of the line. Everything
 * after it must be a well-formed chunk-ext; see chunk_ext_is_valid().
 *
 * The bytes that frame the chunks are bounded against the data that they
 * carry. The chunk extensions, which this parser validates and then drops,
 * are otherwise free: a peer could send megabytes of them around one byte
 * of data, and no limit that counts body bytes would see it. Each chunk adds
 * its size line and its two CRLFs to a running excess, and earns 16 bytes
 * plus twice its data back; the excess never goes below 0, and a message
 * whose excess passes CHTTP1_MAX_CHUNK_OVERHEAD_EXCESS is refused. A sender
 * that streams one byte per chunk, which is 5 bytes of framing per byte of
 * data, stays at 0; so does any extension shorter than 12 bytes plus twice
 * the data of its chunk. The rule is the one of the Go net/http chunked
 * reader.
 */
static bool parse_chunk_size_line(chttp1_parser_t *parser) {
  const char *s = line_ptr(parser);
  size_t len = parser->line_len;
  size_t hex_len = 0;
  while (hex_len < len && s[hex_len] != ';' &&
         !is_ows((unsigned char)s[hex_len]))
    hex_len++;

  uint64_t v;
  if (!parse_uint64_hex(s, hex_len, &v)) {
    parser->reason = "Invalid chunk size";
    return false;
  }
  /* A response may carry whitespace between the size and the CRLF with no
   * extension after it ("5 \r\n"). The Go net/http and curl clients accept
   * that, and so does response mode here. Request mode keeps the strict
   * grammar: a server that reads a chunk-size line more loosely than a proxy
   * in front of it is one half of a request smuggling pair. */
  size_t ext_start = hex_len;
  if (parser->type == CHTTP1_PARSE_RESPONSE) {
    size_t j = hex_len;
    while (j < len && is_ows((unsigned char)s[j])) j++;
    if (j == len) ext_start = len;
  }
  if (!chunk_ext_is_valid(s, ext_start, len)) {
    parser->reason = "Invalid chunk extension";
    return false;
  }
  if (parser->max_chunk_size_override && v > parser->max_chunk_size_override) {
    parser->flags |= F_CHUNK_TOO_LARGE;
    parser->reason = "Chunk size exceeds configured maximum";
    return false;
  }
  uint64_t excess = parser->chunk_excess + (uint64_t)len + 4u;
  uint64_t earned = v > (UINT64_MAX - 16u) / 2u ? UINT64_MAX : 16u + 2u * v;
  excess = excess > earned ? excess - earned : 0;
  if (excess > CHTTP1_MAX_CHUNK_OVERHEAD_EXCESS) {
    parser->reason = "Chunk framing exceeds its data";
    return false;
  }
  parser->chunk_excess = excess;
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

/* The code uses this when a deeper helper already set parser->reason before
 * it reported the failure. process_header_line() and parse_status_line() are
 * two such helpers. */
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

bool chttp1_parser_enable_line_spill(chttp1_parser_t *parser,
                                     ccol_memmgmt_procs_t *mp) {
  if (!parser || parser->type != CHTTP1_PARSE_RESPONSE) return false;
  parser->line_spill_enabled = true;
  parser->line_spill_mp = mp;
  return true;
}

void chttp1_parser_release(chttp1_parser_t *parser) {
  if (!parser || !parser->line_spill) return;
  _ccol_mem_free(parser->line_spill_mp, parser->line_spill);
  parser->line_spill = NULL;
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
        line_result_t lr = accumulate_line(parser, &p, end, 0);
        if (lr == LINE_NEED_MORE) break;
        if (lr == LINE_TOO_LONG)
          return fail(parser, parser->type == CHTTP1_PARSE_REQUEST
                                  ? "Request line too long"
                                  : "Status line too long");
        if (lr == LINE_BAD_EOL) return fail(parser, "Expected CRLF");
        if (parser->type == CHTTP1_PARSE_REQUEST && parser->line_len == 0) {
          /* RFC 7230 SS3.5: the parser ignores a blank line that comes
           * before the request-line itself. Some clients wrongly send a
           * stray CRLF after a POST body. See the comment of
           * CHTTP1_MAX_LEADING_BLANK_LINES for why this is bounded and not
           * unconditional. Without this, parse_request_line() below rejects
           * the empty line, because method_len is 0. It then hard-closes a
           * healthy keep-alive or pipelined connection over one stray CRLF
           * instead of absorbing it. */
          if (++parser->leading_blank_lines > CHTTP1_MAX_LEADING_BLANK_LINES)
            return fail(parser, "Too many leading blank lines");
          parser->line_len = 0;
          break;
        }
        {
          /* The bytes of the request line or the status line count against
           * the same total_header_bytes budget as the bytes of a header
           * line. chttpsvr_config_t.max_header_bytes is documented as a
           * bound on "request line + all header lines", and not only on the
           * headers that follow it. Without this, the parser still accepts
           * a request-target that is far larger than the configured cap. The
           * only other bound on it is CHTTP1_MAX_LINE_LEN, which is much
           * larger. */
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
        line_result_t lr =
            accumulate_line(parser, &p, end, header_spill_budget(parser));
        if (lr == LINE_NEED_MORE) break;
        if (lr == LINE_TOO_LONG) return fail(parser, "Header line too long");
        if (lr == LINE_BAD_EOL) return fail(parser, "Expected CRLF");

        if (parser->line_len == 0) {
          /* Blank line: header block is done. */

          /* RFC 7230 SS5.4, and it runs BEFORE on_headers_complete. A
           * message that breaks this rule must never reach the routing of
           * the caller, or its handler, or its body.
           *
           * More than one Host line is refused for EVERY version. The text
           * of the rule is "any request message that contains more than one
           * Host header field", with no version condition, and the reason
           * does not depend on one either: two Host lines let a front end
           * and a back end disagree about the authority that the request
           * names, which is what request smuggling needs.
           *
           * No Host line at all is refused for HTTP/1.1 and later only.
           * There the rule is "any HTTP/1.1 request message that lacks a
           * Host header field". HTTP/1.0 predates the field and a request
           * without one is ordinary there.
           *
           * The version test is the same one that
           * chttp1_should_keep_alive() makes, and for the same reason: a
           * major above 1 with a minor of 0 is later than HTTP/1.1, and a
           * plain "both digits are nonzero" test would put it in the
           * HTTP/1.0 class.
           *
           * The rule stands whatever form the request-target took. RFC 7230
           * SS5.4 tells a server to IGNORE the received Host value when the
           * target is in absolute-form, and this parser does exactly that:
           * it never routes on the value. Ignoring the value is not the
           * same as excusing a message that carries two of them. */
          if (parser->type == CHTTP1_PARSE_REQUEST) {
            bool is_1_1_or_later =
                parser->http_major > 1 ||
                (parser->http_major == 1 && parser->http_minor >= 1);
            if (parser->host_count > 1)
              return fail(parser, "Duplicate Host header");
            if (parser->host_count == 0 && is_1_1_or_later)
              return fail(parser, "Missing Host header");
            /* RFC 9112 SS6.1: a recipient of an HTTP/1.0 message that carries
             * Transfer-Encoding MUST treat its framing as faulty, because an
             * HTTP/1.0 intermediary can forward the header without knowing
             * what it means. A front end that frames such a request by its
             * Content-Length, or by the absence of one, and a back end that
             * de-chunks it, disagree about where the request ends. The bytes
             * in between then parse as a second request, which is request
             * smuggling. The request is refused outright. The caller answers
             * 400 and closes the connection, which is what nginx does. */
            if ((parser->flags & F_TRANSFER_ENCODING) && !is_1_1_or_later)
              return fail(parser, "Transfer-Encoding in an HTTP/1.0 request");
            /* RFC 9112 SS6.3: a request whose Transfer-Encoding does not end
             * in "chunked" has a length that cannot be determined, and the
             * server MUST answer 400 and close. The parser refuses it here,
             * before on_headers_complete, so that the caller never routes a
             * request whose end it cannot find. See the comment of the
             * response half of this rule below. */
            if ((parser->flags & F_TRANSFER_ENCODING) &&
                !(parser->flags & F_CHUNKED))
              return fail(parser,
                          "Transfer-Encoding present but final encoding is "
                          "not chunked");
          }

          parser->flags |= F_HEADERS_DONE;
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
            /* RFC 7230 SS3.3: a 1xx, a 204 and a 304 never have a body.
             *
             * Take 100 Continue as the example. This parser reports it as
             * the complete message, like any other response with no body. It
             * does not restart itself to parse a second message on the same
             * parser instance after an interim 1xx response.
             *
             * Tier 1 of chttpclient.c does send "Expect: 100-continue", and
             * it does drive a second message on the same connection after a
             * "100 Continue" interim response. chttp_do_internal() does that
             * through chttp_request_t.expect_continue. But it uses a FRESH
             * chttp1_parser_t instance for that second message.
             * _chttp_read_message() in chttpclient.c creates a new parser
             * for each call. That matches the "fresh state per message"
             * convention of this codebase. This parser instance therefore
             * never has to loop internally past the interim response.
             *
             * See the doc comment of chttp1_expects_continue() for why the
             * server side of this same feature works differently. */
            no_body = true;
          }

          if (!no_body && (parser->flags & F_TRANSFER_ENCODING) &&
              !(parser->flags & F_CHUNKED) &&
              (parser->flags & F_CONTENT_LENGTH)) {
            /* The code checks this half of RFC 7230 SS3.3.3 here, and not
             * for each header line. It runs after it sees every
             * Transfer-Encoding line, so the final coding of the merged list
             * is known. See the comment of process_header_line() for why it
             * cannot decide this any earlier.
             *
             * For a REQUEST, a Transfer-Encoding whose final coding is not
             * "chunked" leaves the message length indeterminate. A request
             * has no body-framing mode that EOF delimits, and a response
             * does; the comment where the code enters CHTTP1_ST_BODY_EOF
             * below describes that asymmetry. A silent fall-through to the
             * "no framing headers means no body" branch just below lets a
             * front-end that disagrees about the framing desync from this
             * parser. The request half is therefore refused before
             * on_headers_complete; see the host checks above.
             *
             * For a RESPONSE, the same Transfer-Encoding is a legitimate way
             * to say "read until the connection closes". The
             * CHTTP1_ST_BODY_EOF branch below does exactly that with it.
             * That holds only while nothing else claims to frame the same
             * body.
             *
             * A Content-Length beside it is the ambiguous pair that RFC 7230
             * SS3.3.3 tells a recipient to resolve for the
             * Transfer-Encoding, or to reject. Two framings that disagree
             * are exactly what response smuggling needs.
             *
             * Without this rejection, the Content-Length branch below frames
             * the message at exactly N bytes. It leaves whatever the sender
             * appended after them unread, and it reports the message as
             * cleanly complete. chttp1_should_keep_alive() then answers
             * true, because the finish_state is CHTTP1_FINISH_SAFE and not
             * the CHTTP1_FINISH_SAFE_WITH_CB that a body delimited by EOF
             * leaves behind. The connection goes back to a keep-alive idle
             * pool with bytes of the attacker still queued on it. The next
             * request that reuses it reads those bytes as its own response.
             * The library refuses a response with an ambiguous framing
             * instead, before any of that can start.
             *
             * A Content-Length beside a CHUNKED Transfer-Encoding is the
             * same hazard. The code rejects it in both modes the moment that
             * it sees the second of the two headers; see
             * process_header_line(). */
            return fail(parser,
                        "Content-Length can't be present with a "
                        "non-chunked Transfer-Encoding");
          }

          if (no_body) {
            parser->finish_state = CHTTP1_FINISH_SAFE;
            return complete_message(parser, p, data);
          }

          if (parser->flags & F_CHUNKED) {
            parser->state = CHTTP1_ST_BODY_CHUNK_SIZE;
            /* An EOF anywhere in the chunked body is a truncation. Chunked
             * framing needs an explicit 0-length chunk at the end. A peer
             * that disconnects early is never a valid way to end it. */
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
            /* The same holds here. An EOF before exactly content_length
             * bytes arrive is a truncation and not a valid end. */
            parser->finish_state = CHTTP1_FINISH_UNSAFE;
            if (want_divert) {
              parser->consumed = (size_t)(p - data);
              return CHTTP1_HEADERS_ONLY;
            }
          } else if (parser->type == CHTTP1_PARSE_REQUEST) {
            /* RFC 7230 SS3.3: a request differs from a response here. A
             * request with no Content-Length and no chunked
             * Transfer-Encoding has no body at all. There is no framing mode
             * that EOF delimits for a request. The connection is not even
             * closing, because the client is the side that sends. There is
             * nothing to divert either way. */
            parser->finish_state = CHTTP1_FINISH_SAFE;
            return complete_message(parser, p, data);
          } else {
            /* There is no Content-Length and no chunked coding. Two shapes
             * reach here. The first is an explicit Transfer-Encoding whose
             * last token is not "chunked". For a response, RFC 7230 SS3.3.3
             * then sets the body length by a read until the connection
             * closes; the branch above handles a request instead. The second
             * shape is no framing at all, which ends the same way.
             *
             * This is the ONE body-framing mode where EOF is itself the
             * valid, expected way to end the message. See the needs_eof
             * check of chttp1_should_keep_alive(). That check relies on
             * finish_state staying CHTTP1_FINISH_SAFE_WITH_CB for this case
             * and for no other. */
            parser->state = CHTTP1_ST_BODY_EOF;
            parser->finish_state = CHTTP1_FINISH_SAFE_WITH_CB;
          }
        } else {
          ph_result_t ph = process_header_line(parser, false);
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
        line_result_t lr = accumulate_line(parser, &p, end, 0);
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
        line_result_t lr = accumulate_line(parser, &p, end, 0);
        if (lr == LINE_NEED_MORE) break;
        if (lr == LINE_TOO_LONG || lr == LINE_BAD_EOL || parser->line_len != 0)
          return fail(parser, "Expected CRLF after chunk data");
        parser->state = CHTTP1_ST_BODY_CHUNK_SIZE;
        break;
      }

      case CHTTP1_ST_BODY_CHUNK_TRAILERS: {
        line_result_t lr =
            accumulate_line(parser, &p, end, header_spill_budget(parser));
        if (lr == LINE_NEED_MORE) break;
        if (lr == LINE_TOO_LONG) return fail(parser, "Trailer line too long");
        if (lr == LINE_BAD_EOL) return fail(parser, "Expected CRLF");

        if (parser->line_len == 0) {
          parser->finish_state = CHTTP1_FINISH_SAFE;
          return complete_message(parser, p, data);
        }
        ph_result_t ph = process_header_line(parser, true);
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
        /* This is not reachable in practice. The early-return check at the
         * top of this function already handles CHTTP1_ST_MESSAGE_DONE and
         * CHTTP1_ST_DEAD. That is the hard contract "never call execute()
         * again after CHTTP1_PAUSED or after an error"; see
         * chttp1_parser.h. This branch is a defence only. */
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
  /* The parser is already at a clean message boundary. It reaches that
   * boundary in two ways: through the CHTTP1_PAUSED return of
   * chttp1_parser_execute(), or through an EARLIER call to this same
   * function that completed a body delimited by EOF below. The documented
   * contract of this function says "already at a clean
   * CHTTP1_ST_MESSAGE_DONE boundary ... returns CHTTP1_OK". A second call is
   * therefore a no-op and not a second fire.
   *
   * Without this check, the CHTTP1_FINISH_SAFE_WITH_CB case below runs again
   * on every later call. That case updates state only and never
   * finish_state, because the needs_eof check of chttp1_should_keep_alive()
   * needs finish_state to stay CHTTP1_FINISH_SAFE_WITH_CB after the message
   * completes. Each of those calls would call on_message_complete again.
   * That breaks the documented "fired exactly once" contract of that
   * callback. */
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

void chttp1_parser_force_close(chttp1_parser_t *parser) {
  if (parser) parser->flags |= F_CONNECTION_CLOSE;
}

bool chttp1_should_keep_alive(const chttp1_parser_t *parser) {
  /* "HTTP/1.1 or later" means major > 1, or major == 1 with minor >= 1. It
   * does NOT mean "both major and minor are nonzero". That second test puts
   * a version with a minor of 0 and a major of 2 or more into the
   * HTTP/1.0-or-earlier class. A literal "HTTP/2.0" status line is such a
   * version, and the grammar of this parser accepts it. The doc comment of
   * parse_status_line() explains that it is not fixed to 1.x, because
   * chttp1_should_keep_alive() must compare any major and minor values
   * correctly. With that wrong test, the code would need an explicit
   * Connection: keep-alive token that a real server of that age never sends.
   * It would then turn off the reuse of the connection without a trace. */
  bool is_1_1_or_later = parser->http_major > 1 ||
                         (parser->http_major == 1 && parser->http_minor >= 1);
  /* A "close" token ends the connection for EVERY version, whatever other
   * token sits beside it, on the same Connection line or on another one. RFC
   * 9112 SS9.6 makes "close" the signal that the sender will close, and a
   * "keep-alive" beside it cannot take that back. */
  if (parser->flags & F_CONNECTION_CLOSE) return false;
  /* HTTP/1.0 or earlier: NOT keep-alive by default unless
   * Connection: keep-alive was seen. HTTP/1.1 or later: keep-alive by
   * default. */
  if (!is_1_1_or_later && !(parser->flags & F_CONNECTION_KEEP_ALIVE))
    return false;
  /* RFC 9112 SS6.1: the framing of an HTTP/1.0 message with
   * Transfer-Encoding is faulty, and the recipient MUST close the connection
   * after it processes the message. Request mode never gets here with such a
   * message, because it refuses the message outright. A response is
   * processed as its Transfer-Encoding frames it, and nothing after it on the
   * same connection is trusted. */
  if (!is_1_1_or_later && (parser->flags & F_TRANSFER_ENCODING)) return false;

  /* A body that EOF delimits is never eligible for keep-alive, whatever a
   * Connection header value says. To end that body, the peer already had to
   * end the connection.
   *
   * The code sets finish_state to CHTTP1_FINISH_SAFE_WITH_CB in that one
   * body-framing mode, and in no other. It also keeps that value all the way
   * through the completion of the message. parser->state does not: it moves
   * on to CHTTP1_ST_MESSAGE_DONE before any meaningful call of this
   * function. The doc comment of this function says that its answer is
   * meaningful only after the message is complete. finish_state alone is
   * therefore enough here. */
  bool needs_eof = (parser->finish_state == CHTTP1_FINISH_SAFE_WITH_CB);
  return !needs_eof;
}

bool chttp1_expects_continue(const chttp1_parser_t *parser) {
  /* RFC 9110 SS10.1.1: a server that receives 100-continue in an HTTP/1.0
   * request MUST ignore it. Such a client can predate the expectation and
   * sends its body without waiting, and an interim 100 is not an HTTP/1.0
   * response it expects. */
  bool is_1_1_or_later = parser->http_major > 1 ||
                         (parser->http_major == 1 && parser->http_minor >= 1);
  return is_1_1_or_later && (parser->flags & F_EXPECT_100_CONTINUE) != 0;
}

bool chttp1_has_other_transfer_coding(const chttp1_parser_t *parser) {
  return parser && (parser->flags & F_OTHER_CODING) != 0;
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

bool chttp1_request_bytes_may_remain(const chttp1_parser_t *parser) {
  if (!parser || parser->state == CHTTP1_ST_MESSAGE_DONE) return false;
  /* Before the end of the header block, or after an error inside it, the
   * framing is unknown. */
  if (!(parser->flags & F_HEADERS_DONE)) return true;
  if (parser->flags & F_CHUNKED) return true;
  return (parser->flags & F_CONTENT_LENGTH) && parser->content_length > 0;
}

uint64_t chttp1_body_bytes_still_expected(const chttp1_parser_t *parser) {
  if (!parser) return 0;
  switch (parser->state) {
    case CHTTP1_ST_BODY_CONTENT_LENGTH:
    case CHTTP1_ST_BODY_CHUNK_DATA:
      return parser->content_length;
    case CHTTP1_ST_FIRST_LINE:
    case CHTTP1_ST_HEADERS:
    case CHTTP1_ST_BODY_CHUNK_SIZE:
    case CHTTP1_ST_BODY_CHUNK_CRLF:
    case CHTTP1_ST_BODY_CHUNK_TRAILERS:
    case CHTTP1_ST_BODY_EOF:
    case CHTTP1_ST_MESSAGE_DONE:
    case CHTTP1_ST_DEAD:
      return 0;
  }
  return 0;
}

/* ========================================================================== */
/*                    WORKER-PULL BODY/RESPONSE STREAMING                     */
/* ========================================================================== */

static bool _stream_prepare_common(chttp1_stream_t *stream, int fd,
                                   void *tls_conn, const char *leftover,
                                   size_t leftover_len,
                                   ccol_memmgmt_procs_t *mp) {
  memset(stream, 0, sizeof(*stream));
  stream->fd = fd;
  stream->tls = tls_conn;
  stream->mp = mp;
  stream->prepared = true;
  if (leftover_len == 0) return true;

  char *copy = (char *)_ccol_mem_alloc(mp, leftover_len);
  if (!copy) {
    /* This leaves stream safe to treat as never prepared. It matches the
     * documented contract of this function, which says that stream is
     * equivalent to zero-initialized on a failure. The code set fd, tls and
     * prepared above, before the allocation that just failed. It must roll
     * them back and must not leave them half committed.
     *
     * No caller today dereferences these fields without a check of the
     * return value of this function. But this is the one place that
     * establishes that contract, and it should hold whatever any one caller
     * happens to check.
     *
     * The code rolls fd back to -1 and not to 0. 0 is a real, valid file
     * descriptor, which is stdin. A 0 there makes a caller that skips the
     * check of the return value poll, read or write against fd 0 without a
     * trace, instead of a loud failure. -1 is the "no fd" sentinel that the
     * rest of this codebase uses, for example in chttp_conn_t.fd and in
     * chttp_async_ctx_t.fd. */
    stream->fd = -1;
    stream->tls = NULL;
    stream->mp = NULL;
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
                           const char *leftover, size_t leftover_len,
                           ccol_memmgmt_procs_t *mp) {
  return _stream_prepare_common(stream, fd, NULL, leftover, leftover_len, mp);
}

bool chttp1_stream_prepare_tls(chttp1_stream_t *stream, int fd, void *tls_conn,
                               const char *leftover, size_t leftover_len,
                               ccol_memmgmt_procs_t *mp) {
  return _stream_prepare_common(stream, fd, tls_conn, leftover, leftover_len,
                                mp);
}

/* chttp1_stream_read() and chttp1_stream_write() both use this. It blocks
 * in poll(2) until fd is ready for the direction that the caller asks for,
 * until the deadline passes, or until a signal interrupts the wait. It
 * retries a wait that a signal interrupts, and the caller sees nothing of
 * that. An interrupted poll(2) is not a real timeout and not a real error,
 * and the code must not report it as either.
 *
 * It returns true when fd is ready to proceed. It returns false when the
 * deadline passed, and it then sets stream->timed_out. It also returns false
 * on a real poll(2) error, and it then sets stream->last_errno. */
static void _compute_deadline(struct timespec *deadline, int timeout_ms);
static long _remaining_ms(const struct timespec *deadline);

static bool wait_for_ready(chttp1_stream_t *stream, short events,
                           int timeout_ms) {
  struct pollfd pfd = {.fd = stream->fd, .events = events, .revents = 0};
  /* A retry after a signal waits only for what is left of timeout_ms. A
   * fresh timeout_ms on each retry never expires for a thread that receives
   * signals more often than its timeout. The deadline is read before the
   * first poll(2), because the time an interrupted poll(2) already waited is
   * not known afterwards. */
  struct timespec deadline;
  bool bounded = timeout_ms > 0;
  if (bounded) _compute_deadline(&deadline, timeout_ms);
  int wait_ms = timeout_ms;
  for (;;) {
    int rv = poll(&pfd, 1, wait_ms);
    if (rv > 0) return true;
    if (rv == 0) {
      stream->timed_out = true;
      return false;
    }
    if (errno != EINTR) {
      stream->last_errno = errno;
      return false;
    }
    if (bounded) {
      /* A deadline that already passed still gets one poll(2) with a zero
       * timeout, so that a descriptor that became ready counts. */
      long rem = _remaining_ms(&deadline);
      wait_ms = rem > 0 ? (int)rem : 0;
    }
  }
}

/* Computes the milliseconds that are left until deadline again. The answer
 * is meaningful only when has_deadline is true. The caller treats a result
 * of 0 or less as "already expired".
 *
 * One logical read or write on a TLS stream can need several iterations of
 * a poll and an attempt. A partial TLS record causes that. So does a
 * renegotiation message or a key-update message that OpenSSL consumes
 * internally. Neither of those produces application bytes at once. Each
 * iteration must wait no longer than what is left of the ORIGINAL
 * timeout_ms budget, and not a fresh timeout_ms each time. */
static long _remaining_ms(const struct timespec *deadline) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  /* The nanoseconds that are left round up to a whole millisecond. A
   * truncated result reports a deadline that is less than a millisecond
   * away as already passed, and a wait that uses it ends early. */
  long long ns = (long long)(deadline->tv_sec - now.tv_sec) * 1000000000LL +
                 (deadline->tv_nsec - now.tv_nsec);
  if (ns <= 0) return 0;
  long long ms = (ns + 999999LL) / 1000000LL;
  return ms > (long long)INT_MAX ? (long)INT_MAX : (long)ms;
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
      _ccol_mem_free(stream->mp, stream->carry);
      stream->carry = NULL;
      stream->carry_len = 0;
      stream->carry_pos = 0;
    }
    return (ssize_t)take;
  }

  bool has_deadline = timeout_ms >= 0;
  struct timespec deadline;
  if (has_deadline) _compute_deadline(&deadline, timeout_ms);

  /* For a TLS stream the code tries the read FIRST, before it looks at
   * poll(2) or at the deadline. OpenSSL can already hold decrypted
   * application bytes in its own internal buffer. That happens when a
   * caller above this stream called ctls_conn_read() once with a buflen
   * smaller than the TLS record under it, for example while it parsed the
   * headers before the library handed this connection to a worker. Those
   * bytes are there whether or not the raw fd still has anything for the
   * kernel to give.
   *
   * A poll of the raw fd first makes those decrypted bytes invisible to
   * poll(2). The call then stalls for the whole timeout_ms budget, even
   * though the data is available at once.
   *
   * The attempt first is also what makes timeout_ms == 0 mean "return
   * immediately if fd is not already readable right now" for a TLS stream.
   * That is its documented contract. Without the attempt first it would mean
   * "never even try a read", because the very first attempt always happens
   * before any check of the deadline, and that deadline is already past for
   * timeout_ms == 0.
   *
   * The plaintext branch below keeps the opposite order on purpose: it polls
   * first and reads after. The fd of a plaintext stream is not guaranteed to
   * be non-blocking. chttp1_stream_prepare() documents no such need, and
   * _prepare_tls() does. The test suite of this module drives it against a
   * real blocking socketpair, where poll(2) alone gates everything.
   *
   * A raw read(2) there, before the code confirms readiness, hangs inside
   * read(2) itself whenever nothing is available, and ignores timeout_ms
   * completely. The stream.read_timeout_when_nothing_available test of this
   * file is not vacuous against exactly that. The readiness of a raw fd,
   * unlike that of a TLS stream, has no internal buffer that poll(2) could
   * be blind to. A poll first is therefore both correct and needed here. */
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
      /* An EWOULDBLOCK from ctls_conn_read() does not always mean "wait for
       * readable", where the same code from a raw read(2) does. OpenSSL can
       * need to WRITE before this call can make progress. A deferred session
       * ticket after the handshake causes that, and so does a TLS 1.2
       * renegotiation. See the doc comment of ctls_conn_wants_write().
       *
       * An unconditional wait on POLLIN here leaves the connection stalled.
       * No readiness event ever arrives in the direction that it needs. It
       * stays stalled until timeout_ms, or forever for a caller that
       * configured no deadline at all. */
      short want_events =
          ctls_conn_wants_write((ctls_conn_t *)stream->tls) ? POLLOUT : POLLIN;
      if (!wait_for_ready(stream, want_events, this_timeout)) return -1;
    }
  }

  /* This plaintext path does exactly one wait. The TLS branch above is a
   * real retry loop that can spend the deadline across more than one
   * iteration, and so is the shared loop of chttp1_stream_write() below.
   * This wait comes directly after the code computed `deadline` as
   * "now + timeout_ms", a few instructions up.
   *
   * A second clock_gettime() call to derive "the time that is left until it"
   * here would give back timeout_ms itself, less a few nanoseconds of pure
   * call overhead, for any timeout_ms > 0. That is harmless. But for
   * timeout_ms == 0, ANY elapsed time above zero already exceeds a budget of
   * zero length, so that value is always 0 or less.
   *
   * This function would then report "already timed out" without a call to
   * poll(2) and without an attempt at read(2), even when the fd is readable
   * right now. That breaks the documented timeout_ms == 0 contract of this
   * function, which says "return immediately if fd is not already readable
   * right now". It is not merely a missed optimisation.
   *
   * To pass timeout_ms straight through avoids all of that. The timeout_ms
   * == 0 convention of poll(2) is exactly "one immediate, non-blocking
   * check". The behaviour is the same for every timeout_ms above 0, and for
   * a negative value, which means "block forever". */
  /* A caller that guarantees a non-blocking fd gets the order of the TLS
   * branch: the read first, and a wait only after the socket reported that
   * it is empty. A read that would have succeeded then costs no poll(2). */
  if (stream->fd_nonblocking) {
    for (;;) {
      ssize_t got = read(stream->fd, buf, buflen);
      if (got >= 0) return got;
      if (errno == EINTR) continue;
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
      if (!wait_for_ready(stream, POLLIN, this_timeout)) return -1;
    }
  }

  if (!wait_for_ready(stream, POLLIN, timeout_ms)) return -1;

  /* poll(2) reported the fd readable, so a read(2) that a signal interrupts
   * is retried at once: the data it would have returned is still there. */
  ssize_t got;
  do {
    got = read(stream->fd, buf, buflen);
  } while (got < 0 && errno == EINTR);
  if (got < 0) stream->last_errno = errno;
  return got;
}

ssize_t chttp1_stream_write(chttp1_stream_t *stream, const char *buf,
                            size_t len, int timeout_ms) {
  stream->timed_out = false;
  stream->last_errno = 0;
  if (stream->write_nonblocking) timeout_ms = 0;

  bool has_deadline = timeout_ms >= 0;
  struct timespec deadline;
  if (has_deadline) _compute_deadline(&deadline, timeout_ms);

  /* This is the direction to poll for before the NEXT attempt. It starts at
   * POLLOUT, which matches the contract of a plain send(2). The code
   * derives it again after every TLS write attempt below. See the comment of
   * that branch for why an EWOULDBLOCK from ctls_conn_write() can mean the
   * opposite direction. */
  short want_events = POLLOUT;
  /* This is true only for the very first iteration of this loop. The code
   * computed `deadline` as "now + timeout_ms" a few instructions up, so that
   * deadline cannot have passed already.
   *
   * A computation of "the time that is left until it" there anyway would, for
   * timeout_ms == 0, treat the few nanoseconds of pure call overhead since
   * that computation as a budget of zero length that is already spent. It
   * would report a timeout without a call to poll(2) and without the write
   * attempt below, even when the fd is writable right now. That breaks the
   * documented timeout_ms == 0 contract of this function, which says "return
   * immediately if fd is not already writable right now". It is not merely a
   * missed optimisation.
   *
   * From the SECOND iteration onward, a real wait_for_ready() call already
   * spent real wall-clock time, and so possibly did a real write attempt
   * that does not block. A look at the deadline there is therefore correct
   * and needed. It bounds the TOTAL time that this retry loop may spend, and
   * not the time of one attempt. */
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
    /* On a non-blocking fd the first attempt needs no wait: a socket with
     * room takes the bytes at once, and a full one reports EAGAIN. */
    if (!(first_attempt && stream->fd_nonblocking) &&
        !wait_for_ready(stream, want_events, this_timeout))
      return -1;
    first_attempt = false;

    if (stream->tls) {
      ssize_t written = ctls_conn_write((ctls_conn_t *)stream->tls, buf, len);
      if (written >= 0) return written;
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        /* An EWOULDBLOCK from ctls_conn_write() does not always mean "wait
         * for writable". OpenSSL can need to READ before this call can make
         * progress, for example during a TLS 1.2 renegotiation. See the doc
         * comment of ctls_conn_wants_write(). A loop back to a wait on
         * POLLOUT every time leaves the connection stalled. No readiness
         * event ever arrives in the direction that it needs. */
        want_events = ctls_conn_wants_write((ctls_conn_t *)stream->tls)
                          ? POLLOUT
                          : POLLIN;
        continue;
      }
      stream->last_errno = errno;
      return -1;
    }

    /* MSG_NOSIGNAL: a write to a peer that has closed or reset reports
     * EPIPE here and never raises SIGPIPE, so the process disposition of
     * that signal is irrelevant to this module. MSG_DONTWAIT: POLLOUT means
     * that the socket has some room, not room for len bytes, so a blocking
     * send(2) on a blocking fd would wait for the rest past the deadline.
     * The attempt therefore never blocks, and a short count goes back to the
     * caller. macOS ignores MSG_DONTWAIT on a send, and ccol_send_nb() limits
     * the send there instead (see csock.h). */
    ssize_t written = ccol_send_nb(stream->fd, buf, len, CCOL_MSG_NOSIGNAL);
    if (written >= 0) return written;
    /* A signal that interrupts send(2) before it moved a byte is no error;
     * the loop waits again and retries, as chttp1_stream_writev2() does. */
    if (errno == EINTR) continue;
    /* This matches the TLS branch above. A wait_for_ready() that confirms
     * POLLOUT does not guarantee that a later send(2) on a non-blocking fd
     * cannot still report EWOULDBLOCK or EAGAIN. chttpserver.c always
     * accepts with accept4(..., SOCK_NONBLOCK). The code therefore loops
     * back and waits again. It must not report a transient condition that is
     * no error as a hard I/O failure. */
    if (errno == EWOULDBLOCK || errno == EAGAIN) continue;
    stream->last_errno = errno;
    return -1;
  }
}

ssize_t chttp1_stream_writev2(chttp1_stream_t *stream, const char *a,
                              size_t alen, const char *b, size_t blen,
                              int timeout_ms) {
  stream->timed_out = false;
  stream->last_errno = 0;
  if (stream->write_nonblocking) timeout_ms = 0;
  if (alen == 0 && blen == 0) return 0;

  if (stream->tls) {
    if (alen == 0) return chttp1_stream_write(stream, b, blen, timeout_ms);
    if (blen == 0) return chttp1_stream_write(stream, a, alen, timeout_ms);
    /* Two parts that fit in one record go out as one record: one
     * encryption, one send(2), and a copy of at most 16 KiB, which costs
     * far less than the encryption of the same bytes. */
    if (alen <= CHTTP1_TLS_COALESCE_MAX - blen &&
        blen <= CHTTP1_TLS_COALESCE_MAX) {
      char joined[CHTTP1_TLS_COALESCE_MAX];
      memcpy(joined, a, alen);
      memcpy(joined + alen, b, blen);
      return chttp1_stream_write(stream, joined, alen + blen, timeout_ms);
    }
    ssize_t n = chttp1_stream_write(stream, a, alen, timeout_ms);
    if (n < (ssize_t)alen) return n;
    /* The first part is out. The second is tried at once and never waited
     * for: a full socket leaves it to the next call, which starts exactly
     * there. */
    ssize_t m = chttp1_stream_write(stream, b, blen, 0);
    stream->timed_out = false;
    stream->last_errno = 0;
    return m > 0 ? n + m : n;
  }

  struct iovec iov[2];
  int cnt = 0;
  if (alen) iov[cnt++] = (struct iovec){.iov_base = (void *)a, .iov_len = alen};
  if (blen) iov[cnt++] = (struct iovec){.iov_base = (void *)b, .iov_len = blen};
  /* sendmsg(2) with this iovec is the same single gather write as
   * writev(2), MSG_NOSIGNAL makes a write to a peer that has gone report
   * EPIPE instead of raising SIGPIPE, and MSG_DONTWAIT keeps the attempt
   * from blocking on a blocking fd, for the reason that chttp1_stream_write()
   * gives. macOS ignores MSG_DONTWAIT on a send, so there ccol_send_room()
   * limits the bytes that one attempt gives (see csock.h). */
  struct msghdr msg = {.msg_iov = iov, .msg_iovlen = (size_t)cnt};
  bool has_deadline = timeout_ms >= 0;
  struct timespec deadline;
  if (has_deadline) _compute_deadline(&deadline, timeout_ms);
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
    if (!(first_attempt && stream->fd_nonblocking) &&
        !wait_for_ready(stream, POLLOUT, this_timeout))
      return -1;
    first_attempt = false;
#if defined(_CCOL_EMULATE_DARWIN_SOCK)
    struct iovec part[2];
    struct msghdr pmsg = msg;
    size_t room = ccol_send_room(stream->fd, alen + blen);
    if (room == 0) continue; /* no room now: wait again */
    if (room < alen + blen) {
      int n = 0;
      for (int i = 0; i < cnt && room > 0; i++) {
        part[n] = iov[i];
        if (part[n].iov_len > room) part[n].iov_len = room;
        room -= part[n].iov_len;
        n++;
      }
      pmsg.msg_iov = part;
      pmsg.msg_iovlen = n;
    }
    ssize_t written = sendmsg(stream->fd, &pmsg, CCOL_MSG_NOSIGNAL);
#else
    ssize_t written =
        sendmsg(stream->fd, &msg, CCOL_MSG_NOSIGNAL | MSG_DONTWAIT);
#endif
    if (written >= 0) return written;
    if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) continue;
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
  /* This guards the computation of the needed size below against an
   * overflow. It follows the SIZE_MAX-relative guard that the other growable
   * and concatenated buffers of this codebase already use for the same class
   * of computation. _merge_ref_path() and _concat_len() in chttpclient.c are
   * two examples.
   *
   * len and existing have two sizes that are independent of each other. len
   * is a chunk that the code just read. existing is the carry-over that this
   * stream already held. Their sum can therefore come close to SIZE_MAX
   * without either one alone being impossibly large. That is not reachable
   * in practice. But this project treats an overflow in a size computation
   * that a guard could remove as a real bug, however large an input it needs
   * to trigger it. */
  if (len > SIZE_MAX - existing) return false;
  size_t total = len + existing;
  char *nc = (char *)_ccol_mem_alloc(stream->mp, total);
  if (!nc) return false;
  memcpy(nc, buf, len);
  if (existing) memcpy(nc + len, stream->carry + stream->carry_pos, existing);
  _ccol_mem_free(stream->mp, stream->carry);
  stream->carry = nc;
  stream->carry_len = total;
  stream->carry_pos = 0;
  return true;
}

char *chttp1_stream_take_leftover(chttp1_stream_t *stream, size_t *len_out) {
  if (len_out) *len_out = 0;
  if (stream->carry_pos >= stream->carry_len) return NULL;
  size_t remaining = stream->carry_len - stream->carry_pos;
  char *out = (char *)_ccol_mem_alloc(stream->mp, remaining);
  if (!out) {
    /* The carry of stream stays as it is, and chttp1_stream_release still
     * frees it. *len_out reports the bytes that are lost, so that the
     * caller can tell this failure from "nothing to reclaim". */
    if (len_out) *len_out = remaining;
    return NULL;
  }
  memcpy(out, stream->carry + stream->carry_pos, remaining);
  _ccol_mem_free(stream->mp, stream->carry);
  stream->carry = NULL;
  stream->carry_len = 0;
  stream->carry_pos = 0;
  if (len_out) *len_out = remaining;
  return out;
}

void chttp1_stream_release(chttp1_stream_t *stream) {
  if (stream->carry) {
    _ccol_mem_free(stream->mp, stream->carry);
    stream->carry = NULL;
  }
  stream->carry_len = 0;
  stream->carry_pos = 0;
  stream->released = true;
}
