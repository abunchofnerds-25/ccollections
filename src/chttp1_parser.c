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
#include <internal/cascii.h>
#include <internal/chttp1_parser.h>
#include <internal/csock.h>
#include <internal/ctls.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

/* ========================================================================== */
/*                         PRIVATE CONSTANTS                                  */
/* ========================================================================== */

/* These caps cover the ordinary headers and the trailers together; see the
 * doc comment at the top of chttp1_parser.h. Neither of them affects the
 * layout of chttp1_parser_t, unlike CHTTP1_MAX_LINE_LEN, which is exactly
 * why that one is public, so these two stay private to this file. */
#define CHTTP1_MAX_HEADER_COUNT 100
#define CHTTP1_MAX_TOTAL_HEADER_BYTES (64 * 1024)

/* RFC 7230 SS3.5 recommends that a server ignore at least one empty line
 * before the request line, since some clients send a stray CRLF like that
 * after a POST body. Like every other cap in this file, we bound how many of
 * those lines we take instead of skipping them without limit, so a client
 * that never stops sending blank lines is malformed input that isn't
 * tolerated forever. Response mode never uses this cap, because a blank line
 * before a status line matches no real client bug. */
#define CHTTP1_MAX_LEADING_BLANK_LINES 25

/* chttp1_parser_t.flags bits. */
#define F_CHUNKED 0x01u
#define F_CONTENT_LENGTH 0x02u
#define F_TRANSFER_ENCODING 0x04u
#define F_CONNECTION_CLOSE 0x08u
#define F_CONNECTION_KEEP_ALIVE 0x10u
#define F_EXPECT_100_CONTINUE 0x20u
#define F_CHUNK_TOO_LARGE                      \
  0x40u /* set together with a rejection that  \
         * max_chunk_size_override causes. See \
         * chttp1_chunk_size_limit_exceeded() */
#define F_CHUNKED_APPLIED                                          \
  0x80u /* a "chunked" transfer coding has shown up somewhere in   \
         * the merged Transfer-Encoding list so far; see the       \
         * rejection of a coding applied more than once in         \
         * process_header_line. It isn't F_CHUNKED: F_CHUNKED only \
         * tracks whether the last Transfer-Encoding line ENDS in  \
         * "chunked", and a later line that doesn't end in         \
         * "chunked" clears it again. */
#define F_OTHER_CODING                                                  \
  0x100u /* the merged Transfer-Encoding list names a coding other than \
          * "chunked". See chttp1_has_other_transfer_coding(). */
#define F_HEADERS_DONE                                           \
  0x200u /* the header block ended and its framing is known; see \
          * chttp1_request_bytes_may_remain(). */

/* The chunk-size lines, chunk extensions and chunk-terminating CRLFs of one
 * message may carry at most this many bytes beyond what the chunk data earns;
 * see parse_chunk_size_line(). */
#define CHTTP1_MAX_CHUNK_OVERHEAD_EXCESS (16u * 1024u)

/* ========================================================================== */
/*                         SMALL CHARACTER HELPERS                            */
/* ========================================================================== */

static bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

static bool is_ows(unsigned char c) { return c == ' ' || c == '\t'; }

/* RFC 7230 SS3.2.6 tchar: the bytes a header field NAME may hold. It's not
 * static because chttp1_parser.h exposes it to chttpclient.c and
 * chttpserver.c; see the doc comment of that declaration. */
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

/* Whether a header VALUE must not hold this byte. This parser has a single
 * mode and that mode is strict: it forbids every control character except
 * HTAB, including a bare CR or a NUL that the line split left behind as
 * ordinary content. The comment of accumulate_line() explains why those
 * bytes aren't rejected there. */
static bool is_invalid_value_byte(unsigned char c) {
  return (c < 0x20 && c != '\t') || c == 0x7f;
}

/* Whether a request-target must not hold this byte. RFC 7230 SS3.1.1
 * separates the three request-line fields with single spaces, and SS5.3
 * allows no whitespace at all inside the target, so this is stricter than
 * is_invalid_value_byte(): HTAB is ordinary content in a header VALUE but
 * forbidden here. Without that rule "GET /a<HTAB>b" would reach routing as
 * one target, while a front end that splits on whitespace would read a
 * different request than this server. */
static bool is_invalid_target_byte(unsigned char c) {
  return c <= 0x20 || c == 0x7f;
}

/* ========================================================================== */
/*                         OVERFLOW-CHECKED INTEGER PARSING */
/* ========================================================================== */

/* The standard two-step overflow guard, which multiplies, then adds, and
 * checks each step. Both radices this parser needs go through it. */

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

/* The buffer holding the line being accumulated, and its capacity: the
 * fixed line_buf, or the heap buffer of a response parser whose line
 * outgrew it; see chttp1_parser_enable_line_spill(). */
static inline char *line_ptr(chttp1_parser_t *parser) {
  return parser->line_spill ? parser->line_spill : parser->line_buf;
}

/* Moves the line filling line_buf into a heap buffer of
 * CHTTP1_MAX_SPILL_LINE_LEN bytes so it can grow further. Returns false when
 * the parser may not spill, when the line couldn't fit in the header budget
 * anyway, or when the allocation fails. */
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

/* The heap line buffer's capacity for the current line: the whole buffer, or
 * less when what's left of the header budget couldn't hold a longer line
 * anyway, so such a line is refused as soon as it goes past the budget and
 * not only once the buffer is full. */
static inline size_t spill_cap(size_t spill_budget) {
  return spill_budget && spill_budget < CHTTP1_MAX_SPILL_LINE_LEN
             ? spill_budget
             : (size_t)CHTTP1_MAX_SPILL_LINE_LEN;
}

/*
 * Appends bytes from *pp, up to end, to the parser's line buffer (see
 * line_ptr()). It stops at a full line ending in CRLF, when the buffer fills
 * up, or when the input runs out.
 *
 * *pp is always advanced to show exactly how much was consumed: to end for
 * LINE_NEED_MORE and LINE_TOO_LONG, and one byte past the byte that ends the
 * line for LINE_READY and LINE_BAD_EOL. That matches the "always consume
 * everything" contract of chttp1_parser_execute() for the non-terminal
 * cases.
 *
 * On LINE_READY parser->line_len is the content length of the line with the
 * trailing CRLF already stripped. On every other result parser->line_len
 * keeps whatever has accumulated so far, which on LINE_TOO_LONG is the
 * buffer's full capacity.
 *
 * spill_budget is 0 for a line that must fit in the fixed line_buf. For a
 * header or trailer line of a response parser that may spill, it's what's
 * left of the header byte budget, and a line that fills line_buf then moves
 * to a heap buffer of CHTTP1_MAX_SPILL_LINE_LEN bytes when the budget can
 * still hold a longer line.
 *
 * Every '\n' that doesn't come right after a '\r' gives LINE_BAD_EOL. That
 * rejects a bare LF used to end a line, and also a bare LF in the middle of
 * a line, such as unfolded content of a multi-line header. That's the strict
 * behaviour, and strict is this parser's only mode: no lenient mode anywhere
 * in this codebase relaxes either case, which would mean accepting a bare
 * LF, or a CR without an LF after it, as a valid line ending.
 *
 * A bare CR without an LF after it isn't rejected here. It becomes ordinary
 * line content, and the caller catches it with its own control character
 * check on that content (is_invalid_value_byte() and the reason phrase scan
 * in parse_status_line() do that), so this layer doesn't repeat the check.
 *
 * The loop keeps the buffer, its capacity and the length in locals, because
 * a store through the char buffer may alias any field of the parser, and a
 * loop working on the fields would reload them after every byte.
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
      /* This function's contract is that *pp shows exactly how much the
       * call consumed, and that a result other than a completed line
       * consumes everything. Stopping at the current p would report the
       * unread tail as pending on a parser that can never read it, since
       * the only caller of this result turns it straight into a terminal
       * failure. Reporting `end` keeps consumed() meaningful for every
       * result of this function and not just some of them. */
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

/* Used by parse_request_line() and process_header_line() below, since both
 * need to tell a rejection of malformed input apart from an error an
 * application callback reports. */
typedef enum { PH_OK, PH_ERROR, PH_USER } ph_result_t;

/* ========================================================================== */
/*                         STATUS LINE                                        */
/* ========================================================================== */

/*
 * status-line = HTTP-version SP status-code [ SP reason-phrase ]
 * HTTP-version = "HTTP/" DIGIT "." DIGIT
 *
 * Any single digit is accepted as the major and as the minor, rather than
 * only 1.x. We restrict nothing beyond the grammar, since
 * chttp1_should_keep_alive() has to compare any major and minor values
 * correctly anyway.
 *
 * The status code must be exactly 3 decimal digits with no limit on the
 * numeric range: nonstandard status codes in real use go up to 599, and
 * even that list isn't complete, so we only check the digit count and leave
 * the magnitude to the caller.
 *
 * The reason phrase and the single space before it are BOTH optional. RFC
 * 7230's ABNF wants a trailing SP even for an empty reason phrase, but real
 * servers often leave it out entirely, as in "HTTP/1.1 304\r\n", and
 * rejecting that would break interoperability with them.
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
 * method is checked to be a token of tchar bytes only, the same character
 * class a header field name uses (RFC 7230 SS3.1.1 and SS3.2.6), without
 * comparing it against any set of known method names. A method the server
 * doesn't know is the caller's routing concern, usually answered with an
 * ordinary 405, so this parser doesn't reject it.
 *
 * For request-target we only check that it holds no control character, no
 * DEL and no whitespace (see is_invalid_target_byte()). Origin-form isn't
 * told apart from absolute-form, authority-form and asterisk-form, which fits
 * chttpserver's scope as an origin server without CONNECT or proxy support.
 *
 * HTTP-version uses the same grammar and checks as the version field of
 * parse_status_line().
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

/* unreserved and sub-delims from RFC 3986 SS2.2 and SS2.3. */
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

/* Whether a Host field-value is a valid uri-host with an optional port, as
 * RFC 9112 SS3.2 defines it through RFC 3986 SS3.2.2 and SS3.2.3:
 *
 *   Host       = uri-host [ ":" port ]
 *   uri-host   = IP-literal / IPv4address / reg-name
 *   IP-literal = "[" ( IPv6address / IPvFuture ) "]"
 *   reg-name   = *( unreserved / pct-encoded / sub-delims )
 *   port       = *DIGIT
 *
 * RFC 9112 SS3.2 tells a server to answer 400 for "a Host header field with
 * an invalid field-value". A value outside this grammar names an authority
 * that different recipients can read differently: "a/b" or "a?b" splits into
 * a host and a path or a query, "u@h" carries userinfo, and whitespace makes
 * two tokens out of one. A front end reading one authority and a back end
 * reading another is what request smuggling and cache poisoning need.
 *
 * Character-wise an IPv4address is a reg-name, so it takes the reg-name
 * branch. Inside the brackets of an IP-literal the check sticks to the
 * characters: HEXDIG, ":" and "." for an IPv6address, with at least one
 * ":", and the IPvFuture form "v" 1*HEXDIG "." followed by unreserved,
 * sub-delims and ":". An empty host is refused because it names no authority
 * at all. */
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
  return name_len == lit_len &&
         ccol_ascii_ncasecmp(name, literal, lit_len) == 0;
}

/* Walks the comma-separated transfer-codings of one Transfer-Encoding header
 * value once and reports what the framing rules need to know about
 * "chunked" on that line. The comparison ignores case, and the caller trims
 * the OWS of the whole value first. Like the rest of this parser, the scan
 * allocates nothing and copies nothing.
 *
 * ONE function answers all these questions on purpose. Two scanners walking
 * the same bytes, each with its own idea of where a token starts and ends,
 * could disagree about the same header, and their answers drive opposite
 * decisions: whether the message is framed as chunked, and whether it's
 * refused outright. A disagreement between them is exactly the ambiguity
 * request smuggling needs. When the framing rules need a new answer about
 * the list, add it to this function rather than writing a second scanner.
 *
 * An EMPTY list element is ignored and never counts as a token. RFC 7230 SS7
 * says a recipient must parse and ignore a reasonable number of empty list
 * elements, and RFC 7230 SS4.1 treats the "#rule" list syntax that
 * Transfer-Encoding uses the same way. So "chunked," and ", chunked" and
 * "chunked, ," all name the one-element list "chunked". Without that rule,
 * the trailing comma of "chunked," would make the empty element the final
 * token, the list wouldn't end in "chunked", and a response would take the
 * read-until-close framing of RFC 7230 SS3.3.3 while every recipient that
 * follows SS7 de-chunks it; the two would then disagree about where the
 * message ends.
 *
 * *chunked_count gets how many codings of this line are "chunked",
 * saturating at 2, because the only thing anybody asks of it is whether a
 * sender applies chunked more than once. RFC 7230 SS3.3.1 says "A sender
 * MUST NOT apply chunked more than once to a message body", so a count past
 * 2 has no consumer. Applying it twice makes the body framing ambiguous in
 * exactly the way a front-end/back-end disagreement needs, so the library
 * refuses such a message in BOTH modes.
 *
 * The caller must accumulate the count ACROSS lines: RFC 7230 SS3.2.2
 * merges repeated Transfer-Encoding lines into one list, so a second
 * "chunked" is just as forbidden whether or not it's on the same line as the
 * first.
 *
 * *nonfinal_chunked gets whether "chunked" appears among this line's codings
 * somewhere other than last. RFC 7230 SS3.3.1 needs "chunked" to be a
 * REQUEST's final transfer-coding, so request mode rejects that shape; see
 * chunked_not_last_in_transfer_encoding_list_rejected in
 * tests/chttpserver/tests.c.
 *
 * In a RESPONSE the same shape isn't a fault. RFC 7230 SS3.3.1 lets a sender
 * apply a non-chunked coding last as long as it then ends the message by
 * closing the connection, and RFC 7230 SS3.3.3 frames exactly that response
 * by reading until the close. So a response whose merged Transfer-Encoding
 * list doesn't end in "chunked" is framed by EOF rather than rejected;
 * rejecting it for that alone would refuse a message the spec gives a
 * framing for.
 *
 * *ends_with_chunked gets whether the LAST non-empty token of this line is
 * "chunked". That's what decides the framing, and the caller has to derive it
 * again on every Transfer-Encoding line that names a coding; see the
 * F_CHUNKED handling in process_header_line.
 *
 * *names_a_coding gets whether this line holds at least one non-empty token.
 * A line with none, like "Transfer-Encoding:" or "Transfer-Encoding: ,",
 * only adds empty elements to the merged list, and RFC 7230 SS7 has a
 * recipient ignore those, so such a line leaves the merged list's final
 * coding where the earlier lines put it, and its *ends_with_chunked (false)
 * must not be applied. The answer comes from this one scanner like every
 * other answer about the list, so no second notion of where a token starts
 * can disagree with it.
 *
 * *names_other_coding gets whether this line names a coding other than
 * "chunked". This parser decodes no coding except chunked, so a request that
 * applies another one hands its caller a body that's still encoded, and the
 * caller refuses it; see chttp1_has_other_transfer_coding().
 *
 * A response gets refused for one more thing, an AMBIGUOUS framing, meaning
 * that same Transfer-Encoding together with a Content-Length; see where
 * chttp1_parser_execute() enters CHTTP1_ST_BODY_EOF. */
static void transfer_encoding_scan(
    const char *v, size_t len, unsigned *chunked_count, bool *nonfinal_chunked,
    bool *ends_with_chunked, bool *names_a_coding, bool *names_other_coding) {
  unsigned count = 0;
  bool nonfinal = false;
  bool any_token = false;
  bool other = false;
  /* True when the most recent NON-EMPTY token was "chunked". At the end of
   * the walk it's the answer for *ends_with_chunked; during the walk, a later
   * non-empty token proves that this "chunked" wasn't the final one. */
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
    bool is_chunked =
        (tok_len == 7 && ccol_ascii_ncasecmp(v + ts, "chunked", 7) == 0);
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
 * Connection header value. There's no check for an "upgrade" token: this
 * parser doesn't handle upgrades at all, and chttpclient.c never sends a
 * CONNECT or an Upgrade request, so nothing would use such a flag. */
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
    if (tok_len == 5 && ccol_ascii_ncasecmp(v + start, "close", 5) == 0) {
      parser->flags |= F_CONNECTION_CLOSE;
    } else if (tok_len == 10 &&
               ccol_ascii_ncasecmp(v + start, "keep-alive", 10) == 0) {
      parser->flags |= F_CONNECTION_KEEP_ALIVE;
    }
  }
}

/*
 * Parses parser->line_buf[0..line_len) as one header or trailer line of the
 * form "name: value", and checks it against the three size caps, the name
 * grammar (a token) and the value grammar (no control characters). The
 * file-level doc comment of chttp1_parser.h describes the caps. Both kinds of
 * line get the same checks and the same charge against the caps, and
 * is_trailer decides everything else.
 *
 * For a header line (is_trailer false) we also run the framing checks: a
 * duplicate Content-Length, a conflict between Content-Length and chunked,
 * and a decimal overflow of Content-Length. The framing, connection and
 * expectation flags are updated for the four header names this parser reads,
 * and then settings->on_header is called.
 *
 * For a trailer line (is_trailer true) none of that state is touched, and
 * settings->on_trailer is called instead. RFC 7230 SS4.1.2 puts trailer
 * fields after the point where the framing they would describe is already
 * settled and acted on, so a trailer able to set Content-Length, change
 * Transfer-Encoding, add a Connection token or raise an Expect would let a
 * peer rewrite this parser's view of a message it has already framed. Here
 * such a trailer is inert by construction.
 *
 * The separate callback is the other half of the same property: a caller
 * that keeps one header collection can't have a real header replaced by a
 * trailer field of the same name that it never told apart.
 */
/* Used by process_header_line() and by the CHTTP1_ST_FIRST_LINE case below,
 * so the bytes the request line or the status line adds to
 * total_header_bytes go against the same effective cap as a header line's
 * bytes. */
static size_t effective_max_total_header_bytes(const chttp1_parser_t *parser) {
  return parser->max_total_header_bytes_override
             ? parser->max_total_header_bytes_override
             : CHTTP1_MAX_TOTAL_HEADER_BYTES;
}

/* The spill budget accumulate_line() gets for a header or trailer line: 0
 * unless the parser may spill a long line to the heap, and otherwise what's
 * left of the header byte budget. */
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
     * as one comma-separated list joined in order, so a request's
     * Transfer-Encoding CAN span several header lines. For example
     * "Transfer-Encoding: gzip" followed by "Transfer-Encoding: chunked" is
     * a valid, if unusual, way of saying the merged list is
     * "gzip, chunked".
     *
     * So F_CHUNKED only shows whether the Transfer-Encoding line processed
     * MOST RECENTLY, among the lines that name at least one coding, ends in
     * "chunked". It's derived again from scratch on every such line instead
     * of being set once and never cleared, because an earlier line's claim
     * of a final chunked must NOT survive a later line that changes the
     * picture. That's what makes chunked-then-identity a rejection and
     * gzip-then-chunked an acceptance.
     *
     * A line that names no coding, like "Transfer-Encoding:" or
     * "Transfer-Encoding: ,", only contributes empty list elements, which a
     * recipient ignores (RFC 7230 SS7), so it leaves F_CHUNKED as it is:
     * "chunked" followed by an empty line is the list "chunked", just like
     * the single line "chunked,". Clearing the flag there would frame a
     * chunked response by EOF and refuse a chunked request, while every
     * recipient that ignores empty elements de-chunks the same message.
     *
     * Whether the FINAL merged list ends in "chunked" is only known once
     * every Transfer-Encoding line has been seen, when the headers are
     * complete (see the blank-line branch of CHTTP1_ST_HEADERS below), not
     * per line here. */
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
      /* RFC 7230 SS3.3.1 says a sender must not apply chunked more than once
       * to one message body, and we refuse both spellings of that in both
       * modes: twice in one line's list, as in "chunked, chunked", and once
       * per line across two Transfer-Encoding lines that RFC 7230 SS3.2.2
       * merges into one list, as in "chunked, gzip" and then "chunked".
       * F_CHUNKED_APPLIED carries the first occurrence across lines, which
       * F_CHUNKED can't do, since a line that doesn't end in "chunked"
       * clears it again.
       *
       * Either way the framing is ambiguous: a recipient that de-chunks once
       * and one that de-chunks twice disagree about where the body ends, and
       * that disagreement is what request and response smuggling need. So
       * we refuse the message instead of resolving it in either direction.
       *
       * Response mode refuses it too. A non-final "chunked" is otherwise
       * legitimate there (see the comment of transfer_encoding_scan()), but
       * applying the coding twice is forbidden wherever in the list the two
       * occurrences sit. */
      parser->reason = "chunked can't be applied more than once";
      return PH_ERROR;
    }
    if (line_chunked_count > 0) parser->flags |= F_CHUNKED_APPLIED;
    if (parser->type == CHTTP1_PARSE_REQUEST && line_has_nonfinal_chunked) {
      /* Unlike the cross-line case above, a "chunked" before the end of a
       * SINGLE line's token list, like "chunked, identity" on one line, is
       * always wrong for a request, and no other Transfer-Encoding line can
       * change that. So it's checked and rejected immediately instead of
       * being deferred. */
      parser->reason = "chunked must be the last Transfer-Encoding token";
      return PH_ERROR;
    }
    if (line_ends_with_chunked)
      parser->flags |= F_CHUNKED;
    else if (line_names_a_coding)
      parser->flags &= (uint16_t)~F_CHUNKED;
  } else if (parser->type == CHTTP1_PARSE_REQUEST &&
             header_name_is(name, name_len, "host")) {
    /* RFC 7230 SS5.4. What matters is the count, and the blank line that
     * ends the header block is where we act on it (see the CHTTP1_ST_HEADERS
     * case of chttp1_parser_execute()), since only there is the count final.
     * The field saturates because nobody asks for more than "0, 1 or many".
     *
     * The value check runs here instead, on each line, because it's a
     * property of that one line and needs no other line to decide.
     *
     * Response mode never gets to this branch: a Host header in a response
     * is an ordinary, meaningless header field, and a response's framing
     * doesn't depend on it. */
    if (parser->host_count < 2) parser->host_count++;
    if (!host_value_is_valid(vstart, value_len)) {
      parser->reason = "Invalid Host header value";
      return PH_ERROR;
    }
  } else if (header_name_is(name, name_len, "connection")) {
    parse_connection_tokens(parser, vstart, value_len);
  } else if (header_name_is(name, name_len, "expect")) {
    /* The only expectation value RFC 7231 SS5.1.1 defines; the doc comment
     * of chttp1_expects_continue() gives the contract. This parser only
     * detects the value; it does no I/O and never sends an interim
     * "100 Continue" response itself. */
    if (value_len == 12 && ccol_ascii_ncasecmp(vstart, "100-continue", 12) == 0)
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
 * BWS is optional whitespace a recipient must accept and ignore. This
 * library reads no chunk extension and hands none to the caller, but it
 * still checks the whole grammar, because every recipient on the path must
 * agree on where the chunk-size line ends and what it says. A parser that
 * skips everything after ";" would accept a bare CR, a NUL or any other
 * control byte there, and a recipient that reads the same bytes more
 * strictly would then frame the body differently, which is the disagreement
 * request and response smuggling need.
 *
 * Returns true when s[i..len) is a well-formed chunk-ext, and an empty span
 * is one, since the grammar allows zero extensions.
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
      /* No value. Whitespace not followed by another ";" isn't part of the
       * grammar, so rewind to the end of the name and let the loop demand
       * the ";" of the next extension. */
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
 * The bytes that frame the chunks are bounded against the data they carry.
 * Chunk extensions, which this parser validates and then drops, would
 * otherwise be free: a peer could send megabytes of them around one byte of
 * data and no limit that counts body bytes would notice. Each chunk adds its
 * size line and its two CRLFs to a running excess and earns 16 bytes plus
 * twice its data back; the excess never goes below 0, and a message whose
 * excess passes CHTTP1_MAX_CHUNK_OVERHEAD_EXCESS is refused. A sender
 * streaming one byte per chunk, 5 bytes of framing per byte of data, stays
 * at 0, and so does any extension shorter than 12 bytes plus twice its
 * chunk's data, so an honest sender never gets near the limit. It's the
 * same rule as the Go net/http chunked reader.
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
  /* A response may carry whitespace between the size and the CRLF without
   * any extension after it ("5 \r\n"). The Go net/http and curl clients
   * accept that and so does response mode here, while request mode keeps
   * the strict grammar: a server that reads a chunk-size line more loosely
   * than a proxy in front of it is one half of a request smuggling pair. */
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

/* Used when a deeper helper such as process_header_line() or
 * parse_status_line() has already set parser->reason before reporting the
 * failure. */
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
 * Fires on_message_complete and moves to the paused/done terminal state. p
 * is the current read position (used for chttp1_parser_consumed()), and data
 * is the start of the buffer passed to this chttp1_parser_execute call.
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

/* The reset clears every member in front of line_buf and leaves the buffer
 * alone; the comment of line_buf in chttp1_parser.h has the invariant that
 * makes this safe. So the buffer must be the last member. */
_Static_assert(offsetof(chttp1_parser_t, line_buf) +
                       sizeof(((chttp1_parser_t *)0)->line_buf) ==
                   sizeof(chttp1_parser_t),
               "line_buf must be the last member of chttp1_parser_t");
_Static_assert(CHTTP1_MAX_LINE_LEN % _Alignof(max_align_t) == 0,
               "the size of line_buf must keep the end of chttp1_parser_t "
               "aligned");

static void parser_init_common(chttp1_parser_t *parser,
                               const chttp1_settings_t *settings,
                               chttp1_parser_type_t type) {
  memset(parser, 0, offsetof(chttp1_parser_t, line_buf));
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
          /* RFC 7230 SS3.5: a blank line before the request-line itself is
           * ignored, because some clients wrongly send a stray CRLF after a
           * POST body. The comment of CHTTP1_MAX_LEADING_BLANK_LINES explains
           * why this is bounded and not unconditional. Without it
           * parse_request_line() below would reject the empty line, since
           * method_len is 0, and hard-close a healthy keep-alive or pipelined
           * connection over one stray CRLF instead of absorbing it. */
          if (++parser->leading_blank_lines > CHTTP1_MAX_LEADING_BLANK_LINES)
            return fail(parser, "Too many leading blank lines");
          parser->line_len = 0;
          break;
        }
        {
          /* The bytes of the request line or the status line count against
           * the same total_header_bytes budget as a header line's bytes,
           * because chttpsvr_config_t.max_header_bytes is documented as a
           * bound on "request line + all header lines", not only on the
           * headers after it. Without this we'd accept a request-target far
           * larger than the configured cap, since the only other bound on it
           * is CHTTP1_MAX_LINE_LEN, which is much bigger. */
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
          /* Blank line: the header block is done. */

          /* RFC 7230 SS5.4. This check runs BEFORE on_headers_complete,
           * because a message breaking this rule must never reach the
           * caller's routing, its handler or its body.
           *
           * More than one Host line is refused for EVERY version. The rule
           * says "any request message that contains more than one Host
           * header field" without a version condition, and the reason
           * doesn't depend on one either: two Host lines let a front end and
           * a back end disagree about the authority the request names, which
           * is what request smuggling needs.
           *
           * A missing Host line is only refused for HTTP/1.1 and later,
           * where the rule is "any HTTP/1.1 request message that lacks a
           * Host header field". HTTP/1.0 predates the field, and a request
           * without one is ordinary there.
           *
           * The version test is the same as in chttp1_should_keep_alive(),
           * for the same reason: a major above 1 with a minor of 0 is later
           * than HTTP/1.1, and a naive "both digits are nonzero" test would
           * put it in the HTTP/1.0 class.
           *
           * The rule holds whatever form the request-target took. RFC 7230
           * SS5.4 tells a server to IGNORE the received Host value when the
           * target is in absolute-form, and this parser does exactly that,
           * it never routes on the value. Ignoring the value doesn't excuse
           * a message that carries two of them, though. */
          if (parser->type == CHTTP1_PARSE_REQUEST) {
            bool is_1_1_or_later =
                parser->http_major > 1 ||
                (parser->http_major == 1 && parser->http_minor >= 1);
            if (parser->host_count > 1)
              return fail(parser, "Duplicate Host header");
            if (parser->host_count == 0 && is_1_1_or_later)
              return fail(parser, "Missing Host header");
            /* RFC 9112 SS6.1: a recipient of an HTTP/1.0 message carrying
             * Transfer-Encoding MUST treat its framing as faulty, because an
             * HTTP/1.0 intermediary may forward the header without knowing
             * what it means. A front end that frames such a request by its
             * Content-Length, or by the lack of one, and a back end that
             * de-chunks it disagree about where the request ends, and the
             * bytes in between parse as a second request, which is request
             * smuggling. So the request is refused outright: the caller
             * answers 400 and closes the connection, as nginx does. */
            if ((parser->flags & F_TRANSFER_ENCODING) && !is_1_1_or_later)
              return fail(parser, "Transfer-Encoding in an HTTP/1.0 request");
            /* RFC 9112 SS6.3: a request whose Transfer-Encoding doesn't end
             * in "chunked" has a length that can't be determined, and the
             * server MUST answer 400 and close. It's refused here, before
             * on_headers_complete, so the caller never routes a request
             * whose end it can't find. See the comment on the response half
             * of this rule below. */
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
             * Take 100 Continue as the example: it's reported as the
             * complete message, like any other bodiless response, and the
             * parser doesn't restart itself to parse a second message on the
             * same instance after an interim 1xx response.
             *
             * Tier 1 of chttpclient.c does send "Expect: 100-continue", and
             * it does drive a second message on the same connection after a
             * "100 Continue" interim response; chttp_do_internal() does that
             * through chttp_request_t.expect_continue. But it uses a FRESH
             * chttp1_parser_t instance for that second message, because
             * _chttp_read_message() in chttpclient.c creates a new parser on
             * every call, following this codebase's "fresh state per
             * message" convention. So this parser instance never needs to loop
             * internally past the interim response.
             *
             * The doc comment of chttp1_expects_continue() explains why the
             * server side of the same feature works differently. */
            no_body = true;
          }

          if (!no_body && (parser->flags & F_TRANSFER_ENCODING) &&
              !(parser->flags & F_CHUNKED) &&
              (parser->flags & F_CONTENT_LENGTH)) {
            /* This half of RFC 7230 SS3.3.3 is checked here and not per
             * header line because it runs after every Transfer-Encoding line
             * has been seen, so the merged list's final coding is known. The
             * comment of process_header_line() explains why it can't be
             * decided any earlier.
             *
             * For a REQUEST, a Transfer-Encoding whose final coding isn't
             * "chunked" leaves the message length indeterminate: a request
             * has no EOF-delimited body framing mode, while a response does
             * (the comment where the code enters CHTTP1_ST_BODY_EOF below
             * describes that asymmetry). Silently falling through to the "no
             * framing headers means no body" branch just below would let a
             * front end that disagrees about the framing desync from this
             * parser, so the request half is refused before
             * on_headers_complete; see the host checks above.
             *
             * For a RESPONSE, the same Transfer-Encoding is a legitimate way
             * of saying "read until the connection closes", and the
             * CHTTP1_ST_BODY_EOF branch below does exactly that with it. That
             * only holds while nothing else claims to frame the same body.
             *
             * A Content-Length next to it is the ambiguous pair that RFC
             * 7230 SS3.3.3 tells a recipient to resolve in favour of the
             * Transfer-Encoding or to reject, and two framings that disagree
             * are exactly what response smuggling needs.
             *
             * Without this rejection the Content-Length branch below would
             * frame the message at exactly N bytes, leave whatever the sender
             * appended after them unread, and report the message as cleanly
             * complete. chttp1_should_keep_alive() would then answer true,
             * because finish_state is CHTTP1_FINISH_SAFE and not the
             * CHTTP1_FINISH_SAFE_WITH_CB an EOF-delimited body leaves behind,
             * so the connection would go back to a keep-alive idle pool with
             * the attacker's bytes queued on it, and the next request reusing
             * it would read those bytes as its own response. So the library
             * refuses a response with an ambiguous framing before any of that
             * can start.
             *
             * A Content-Length next to a CHUNKED Transfer-Encoding is the
             * same hazard, rejected in both modes as soon as the second of
             * the two headers shows up; see process_header_line(). */
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
            /* An EOF anywhere in a chunked body is a truncation: chunked
             * framing needs an explicit 0-length chunk at the end, and a
             * peer disconnecting early is never a valid way to end it. */
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
            /* Same here: an EOF before exactly content_length bytes have
             * arrived is a truncation and not a valid end. */
            parser->finish_state = CHTTP1_FINISH_UNSAFE;
            if (want_divert) {
              parser->consumed = (size_t)(p - data);
              return CHTTP1_HEADERS_ONLY;
            }
          } else if (parser->type == CHTTP1_PARSE_REQUEST) {
            /* RFC 7230 SS3.3: here a request differs from a response. A
             * request with no Content-Length and no chunked
             * Transfer-Encoding has no body at all, since a request has no
             * EOF-delimited framing mode; the connection isn't even closing,
             * because the client is the one sending. There's nothing to
             * divert either way. */
            parser->finish_state = CHTTP1_FINISH_SAFE;
            return complete_message(parser, p, data);
          } else {
            /* No Content-Length and no chunked coding, and two shapes get
             * here. One is an explicit Transfer-Encoding whose last token
             * isn't "chunked", for which, in a response, RFC 7230 SS3.3.3
             * sets the body length by reading until the connection closes
             * (a request is handled by the branch above instead). The other
             * is no framing at all, which ends the same way.
             *
             * This is the ONE body framing mode where EOF itself is the
             * valid, expected end of the message; see the needs_eof check of
             * chttp1_should_keep_alive(), which relies on finish_state
             * staying CHTTP1_FINISH_SAFE_WITH_CB for this case and no
             * other. */
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
         * state; nothing in the content says "the body is done" here. */
        break;
      }

      case CHTTP1_ST_MESSAGE_DONE:
      case CHTTP1_ST_DEAD:
      default:
        /* Not reachable in practice: the early-return check at the top of
         * this function already handles CHTTP1_ST_MESSAGE_DONE and
         * CHTTP1_ST_DEAD, which is the hard contract "never call execute()
         * again after CHTTP1_PAUSED or after an error"; see chttp1_parser.h.
         * This branch is only a defence. */
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
  /* The parser is already at a clean message boundary, which it reaches in
   * two ways: through chttp1_parser_execute() returning CHTTP1_PAUSED, or
   * through an EARLIER call of this same function that completed an
   * EOF-delimited body below. This function's documented contract says
   * "already at a clean CHTTP1_ST_MESSAGE_DONE boundary ... returns
   * CHTTP1_OK", so a second call does nothing rather than fire again.
   *
   * Without this check the CHTTP1_FINISH_SAFE_WITH_CB case below would run
   * again on every later call. That case only updates state and never
   * finish_state, because the needs_eof check of chttp1_should_keep_alive()
   * needs finish_state to stay CHTTP1_FINISH_SAFE_WITH_CB after the message
   * completes, so every one of those calls would call on_message_complete
   * again and break that callback's documented "fired exactly once"
   * contract. */
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
  /* "HTTP/1.1 or later" means major > 1, or major == 1 with minor >= 1, and
   * NOT "both major and minor are nonzero". The second test would put a
   * version with a minor of 0 and a major of 2 or more, such as a literal
   * "HTTP/2.0" status line, which this parser's grammar accepts, into the
   * HTTP/1.0-or-earlier class. The doc comment of parse_status_line()
   * explains why it isn't fixed to 1.x: chttp1_should_keep_alive() must
   * compare any major and minor values correctly. With the wrong test we'd
   * require an explicit Connection: keep-alive token that a real server of
   * that age never sends, and connection reuse would quietly be switched
   * off. */
  bool is_1_1_or_later = parser->http_major > 1 ||
                         (parser->http_major == 1 && parser->http_minor >= 1);
  /* A "close" token ends the connection for EVERY version, whatever other
   * token is next to it, on the same Connection line or on another one. RFC
   * 9112 SS9.6 makes "close" the sender's signal that it will close, and a
   * "keep-alive" beside it can't take that back. */
  if (parser->flags & F_CONNECTION_CLOSE) return false;
  /* HTTP/1.0 or earlier: NOT keep-alive by default unless
   * Connection: keep-alive was seen. HTTP/1.1 or later: keep-alive by
   * default. */
  if (!is_1_1_or_later && !(parser->flags & F_CONNECTION_KEEP_ALIVE))
    return false;
  /* RFC 9112 SS6.1: an HTTP/1.0 message with Transfer-Encoding has faulty
   * framing, and the recipient MUST close the connection after processing
   * it. Request mode never gets here with such a message since it refuses it
   * outright, while a response is processed as its Transfer-Encoding frames
   * it, and nothing after it on the same connection is trusted. */
  if (!is_1_1_or_later && (parser->flags & F_TRANSFER_ENCODING)) return false;

  /* An EOF-delimited body never qualifies for keep-alive, whatever a
   * Connection header value says, because ending that body already took the
   * peer ending the connection.
   *
   * finish_state is set to CHTTP1_FINISH_SAFE_WITH_CB in that one body
   * framing mode and in no other, and keeps that value all the way through
   * the completion of the message. parser->state doesn't: it moves on to
   * CHTTP1_ST_MESSAGE_DONE before any meaningful call of this function.
   * Since this function's doc comment says its answer is only meaningful
   * once the message is complete, finish_state alone is enough here. */
  bool needs_eof = (parser->finish_state == CHTTP1_FINISH_SAFE_WITH_CB);
  return !needs_eof;
}

bool chttp1_expects_continue(const chttp1_parser_t *parser) {
  /* RFC 9110 SS10.1.1: a server that receives 100-continue in an HTTP/1.0
   * request MUST ignore it. Such a client may predate the expectation and
   * sends its body without waiting, and an interim 100 isn't an HTTP/1.0
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
   * framing isn't known. */
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
    /* This leaves stream safe to treat as never prepared, which is this
     * function's documented contract: on failure stream is equivalent to
     * zero-initialized. fd, tls and prepared were set above, before the
     * allocation that just failed, so they have to be rolled back rather
     * than left half committed.
     *
     * No caller dereferences these fields without checking this function's
     * return value, but this is the one place that establishes the
     * contract, and it should hold whatever any particular caller happens
     * to check.
     *
     * fd goes back to -1 and not 0, because 0 is a real, valid file
     * descriptor, stdin: with a 0 there, a caller that skips the return
     * value check would quietly poll, read or write against fd 0 instead of
     * failing loudly. -1 is the "no fd" sentinel used in the rest of this
     * codebase, for example in chttp_conn_t.fd and in
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

/* Used by chttp1_stream_read() and chttp1_stream_write(). It blocks in
 * poll(2) until fd is ready for the direction the caller asks for, until
 * the deadline passes, or until a signal interrupts the wait. A wait that a
 * signal interrupts is retried without the caller noticing, because an
 * interrupted poll(2) is neither a real timeout nor a real error and must
 * not be reported as either.
 *
 * Returns true when fd is ready to proceed, and false when the deadline
 * passed (then stream->timed_out is set) or on a real poll(2) error (then
 * stream->last_errno is set). */
static void _compute_deadline(struct timespec *deadline, int timeout_ms);
static long _remaining_ms(const struct timespec *deadline);

static bool wait_for_ready(chttp1_stream_t *stream, short events,
                           int timeout_ms) {
  struct pollfd pfd = {.fd = stream->fd, .events = events, .revents = 0};
  /* A retry after a signal only waits for what's left of timeout_ms, since
   * a fresh timeout_ms on every retry would never expire for a thread that
   * gets signals more often than its timeout. The deadline is read before
   * the first poll(2), because afterwards there's no way to know how long an
   * interrupted poll(2) already waited. */
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
      /* A deadline that has already passed still gets one more poll(2) with
       * a zero timeout, so a descriptor that has become ready counts. */
      long rem = _remaining_ms(&deadline);
      wait_ms = rem > 0 ? (int)rem : 0;
    }
  }
}

/* Computes again how many milliseconds are left until deadline. Only
 * meaningful when has_deadline is true, and the caller treats 0 or less as
 * "already expired".
 *
 * One logical read or write on a TLS stream can take several rounds of poll
 * and attempt, because of a partial TLS record, or a renegotiation or
 * key-update message that OpenSSL consumes internally, none of which gives
 * application bytes right away. Each round must wait no longer than what's
 * left of the ORIGINAL timeout_ms budget, not a fresh timeout_ms every
 * time. */
static long _remaining_ms(const struct timespec *deadline) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  /* The remaining nanoseconds are rounded up to a whole millisecond, because
   * truncating would report a deadline less than a millisecond away as
   * already passed, and a wait using it would end early. */
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

  /* For a TLS stream we try the read FIRST, before looking at poll(2) or at
   * the deadline, because OpenSSL may already hold decrypted application
   * bytes in its own internal buffer. That happens when a caller above this
   * stream called ctls_conn_read() once with a buflen smaller than the TLS
   * record under it, for example while parsing the headers before the
   * library handed this connection to a worker, and those bytes are there
   * whether or not the raw fd has anything left for the kernel to give.
   *
   * Polling the raw fd first would make those decrypted bytes invisible to
   * poll(2), and the call would stall for the whole timeout_ms budget even
   * though the data is available at once.
   *
   * Trying first is also what makes timeout_ms == 0 mean "return at once
   * if fd isn't readable at this moment" for a TLS stream, which is its
   * documented contract. Otherwise it would mean "never even try a read",
   * because the very first attempt always comes before any deadline check,
   * and for timeout_ms == 0 that deadline has already passed.
   *
   * The plaintext branch below keeps the opposite order on purpose and polls
   * before reading, because a plaintext stream's fd isn't guaranteed to be
   * non-blocking (chttp1_stream_prepare() documents no such requirement,
   * while _prepare_tls() does). This module's test suite drives it against a
   * real blocking socketpair, where poll(2) alone gates everything.
   *
   * A raw read(2) there before readiness is confirmed would hang inside
   * read(2) itself whenever nothing is available, ignoring timeout_ms
   * completely, and the stream.read_timeout_when_nothing_available test of
   * this file is non-vacuous against exactly that. Unlike a TLS stream, a
   * raw fd's readiness has no internal buffer that poll(2) could be blind
   * to, so polling first is both correct and needed here. */
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
      /* An EWOULDBLOCK from ctls_conn_read() doesn't always mean "wait for
       * readable" the way it does from a raw read(2): OpenSSL may need to
       * WRITE before this call can make progress, for example for a deferred
       * session ticket after the handshake or for a TLS 1.2 renegotiation.
       * See the doc comment of ctls_conn_wants_write().
       *
       * Always waiting for POLLIN here would leave the connection stalled
       * until timeout_ms, or forever for a caller with no deadline at all,
       * because no readiness event ever comes in the direction it needs. */
      short want_events =
          ctls_conn_wants_write((ctls_conn_t *)stream->tls) ? POLLOUT : POLLIN;
      if (!wait_for_ready(stream, want_events, this_timeout)) return -1;
    }
  }

  /* This plaintext path makes exactly one wait, unlike the TLS branch above
   * and the shared loop of chttp1_stream_write() below, which are real retry
   * loops that can spend the deadline over several rounds. This wait comes
   * right after `deadline` was computed as "now + timeout_ms" a few
   * instructions up.
   *
   * A second clock_gettime() to work out "the time left until it" here would
   * give back timeout_ms itself, minus a few nanoseconds of call overhead,
   * for any timeout_ms > 0, which is harmless. But for timeout_ms == 0 ANY
   * elapsed time above zero already exceeds a zero-length budget, so that
   * value would always be 0 or less.
   *
   * The function would then report "already timed out" without calling
   * poll(2) and without trying read(2), even when the fd is readable right
   * now. That would break the documented timeout_ms == 0 contract, "return
   * at once if fd isn't readable at this moment", so it's more than a
   * missed optimisation.
   *
   * Passing timeout_ms straight through avoids all of it, because poll(2)'s
   * timeout_ms == 0 convention is exactly "one immediate, non-blocking
   * check". The behaviour is the same for every timeout_ms above 0, and for a
   * negative value, which means "block forever". */
  /* A caller that guarantees a non-blocking fd gets the TLS branch's order:
   * read first, and only wait once the socket has reported that it's empty,
   * so a read that would have succeeded costs no poll(2). */
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
   * is retried immediately; the data it would have returned is still there. */
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

  /* The direction to poll for before the NEXT attempt. It starts at POLLOUT,
   * matching a plain send(2)'s contract, and it's derived again after every
   * TLS write attempt below; that branch's comment explains why an
   * EWOULDBLOCK from ctls_conn_write() can mean the opposite direction. */
  short want_events = POLLOUT;
  /* Only true on the very first round of this loop: `deadline` was computed
   * as "now + timeout_ms" a few instructions up, so it can't have passed
   * yet.
   *
   * Working out "the time left until it" there anyway would, for
   * timeout_ms == 0, treat the few nanoseconds of call overhead since that
   * computation as an already spent zero-length budget, and report a timeout
   * without calling poll(2) and without the write attempt below, even when
   * the fd is writable right now. That would break the documented
   * timeout_ms == 0 contract, "return at once if fd isn't writable at
   * this moment", so it's more than a missed optimisation.
   *
   * From the SECOND round on, a real wait_for_ready() call has already spent
   * real wall-clock time, and possibly a real non-blocking write attempt
   * has too, so checking the deadline there is correct and needed: it bounds
   * the TOTAL time this retry loop may spend, not the time of one attempt. */
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
     * room takes the bytes without waiting, and a full one reports EAGAIN. */
    if (!(first_attempt && stream->fd_nonblocking) &&
        !wait_for_ready(stream, want_events, this_timeout))
      return -1;
    first_attempt = false;

    if (stream->tls) {
      ssize_t written = ctls_conn_write((ctls_conn_t *)stream->tls, buf, len);
      if (written >= 0) return written;
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        /* An EWOULDBLOCK from ctls_conn_write() doesn't always mean "wait
         * for writable": OpenSSL may need to READ before this call can make
         * progress, for example during a TLS 1.2 renegotiation (see the doc
         * comment of ctls_conn_wants_write()). Always going back to waiting
         * for POLLOUT would leave the connection stalled, because no
         * readiness event ever comes in the direction it needs. */
        want_events = ctls_conn_wants_write((ctls_conn_t *)stream->tls)
                          ? POLLOUT
                          : POLLIN;
        continue;
      }
      stream->last_errno = errno;
      return -1;
    }

    /* MSG_NOSIGNAL: writing to a peer that has closed or reset reports EPIPE
     * here and never raises SIGPIPE, so the process's disposition for that
     * signal doesn't matter to this module. MSG_DONTWAIT: POLLOUT means the
     * socket has some room, not room for len bytes, so a blocking send(2) on
     * a blocking fd would wait for the rest past the deadline. So the
     * attempt never blocks, and a short count goes back to the caller. macOS
     * ignores MSG_DONTWAIT on a send, and there ccol_send_nb() limits the
     * send instead (see csock.h). */
    ssize_t written = ccol_send_nb(stream->fd, buf, len, CCOL_MSG_NOSIGNAL);
    if (written >= 0) return written;
    /* A signal that interrupts send(2) before it has moved a byte isn't an
     * error; the loop waits again and retries, as chttp1_stream_writev2()
     * does. */
    if (errno == EINTR) continue;
    /* Same as the TLS branch above: a wait_for_ready() that confirms POLLOUT
     * doesn't guarantee that a later send(2) on a non-blocking fd can't
     * report EWOULDBLOCK or EAGAIN, and chttpserver.c always accepts with
     * accept4(..., SOCK_NONBLOCK). So we loop back and wait again instead of
     * reporting a transient non-error as a hard I/O failure. */
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
    /* Two parts that fit in one record go out as one record: one encryption,
     * one send(2), and a copy of at most 16 KiB, which costs far less than
     * encrypting the same bytes. */
    if (blen <= CHTTP1_TLS_COALESCE_MAX &&
        alen <= CHTTP1_TLS_COALESCE_MAX - blen) {
      char joined[CHTTP1_TLS_COALESCE_MAX];
      memcpy(joined, a, alen);
      memcpy(joined + alen, b, blen);
      return chttp1_stream_write(stream, joined, alen + blen, timeout_ms);
    }
    ssize_t n = chttp1_stream_write(stream, a, alen, timeout_ms);
    if (n < (ssize_t)alen) return n;
    /* The first part is out. The second one is tried in the same call and never
     * waited for; a full socket leaves it to the next call, which starts
     * exactly there. */
    ssize_t m = chttp1_stream_write(stream, b, blen, 0);
    stream->timed_out = false;
    stream->last_errno = 0;
    return m > 0 ? n + m : n;
  }

  struct iovec iov[2];
  int cnt = 0;
  if (alen) iov[cnt++] = (struct iovec){.iov_base = (void *)a, .iov_len = alen};
  if (blen) iov[cnt++] = (struct iovec){.iov_base = (void *)b, .iov_len = blen};
  /* sendmsg(2) with this iovec is the same single gather write as writev(2).
   * MSG_NOSIGNAL makes a write to a peer that's gone report EPIPE instead of
   * raising SIGPIPE, and MSG_DONTWAIT keeps the attempt from blocking on a
   * blocking fd, for the reason chttp1_stream_write() gives. macOS ignores
   * MSG_DONTWAIT on a send, so there ccol_send_room() limits how many bytes
   * one attempt hands over (see csock.h). */
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
    if (room == 0) continue; /* no room now, wait again */
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
  /* Guards the size computation below against overflow, the same
   * SIZE_MAX-relative guard the other growable and concatenated buffers of
   * this codebase use for this kind of computation, _merge_ref_path() and
   * _concat_len() in chttpclient.c for example.
   *
   * len and existing are two independent sizes: len is a chunk just read,
   * and existing is the carry-over this stream already held. So their sum
   * can get close to SIZE_MAX without either one alone being impossibly
   * large. That can't happen in practice, but in this project an overflow in
   * a size computation that a guard could remove is a real bug, however
   * large the input it would take to trigger. */
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
    /* stream's carry stays as it is, and chttp1_stream_release frees it as
     * usual. *len_out reports the lost bytes so the caller can tell this
     * failure from "nothing to take back". */
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
