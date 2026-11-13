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

// The default header count and total header byte caps, which keep a peer from
// growing the header block without bound. Trailers count against them too.
// They don't affect the struct layout, so they stay private.
#define CHTTP1_MAX_HEADER_COUNT 100
#define CHTTP1_MAX_TOTAL_HEADER_BYTES (64 * 1024)

// How many blank lines a request may send before its request line. RFC 7230
// section 3.5 has a server ignore a stray CRLF after a POST body, but an
// endless stream of them is still malformed input.
#define CHTTP1_MAX_LEADING_BLANK_LINES 25

// Bits of chttp1_parser.flags: The facts the header block established, which
// the framing rules and the accessors read. F_CHUNKED says the latest
// Transfer-Encoding line that names a coding ends in chunked;
// F_CHUNKED_APPLIED says chunked appeared anywhere in the merged list so far
// (it survives a later line that clears F_CHUNKED). F_CHUNK_TOO_LARGE goes
// with a rejection by max_chunk_size_override, F_OTHER_CODING marks a coding
// other than chunked, and F_HEADERS_DONE says the header block ended and the
// framing is known.
#define F_CHUNKED 0x01u
#define F_CONTENT_LENGTH 0x02u
#define F_TRANSFER_ENCODING 0x04u
#define F_CONNECTION_CLOSE 0x08u
#define F_CONNECTION_KEEP_ALIVE 0x10u
#define F_EXPECT_100_CONTINUE 0x20u
#define F_CHUNK_TOO_LARGE 0x40u
#define F_CHUNKED_APPLIED 0x80u
#define F_OTHER_CODING 0x100u
#define F_HEADERS_DONE 0x200u

// How many bytes of chunk framing (size lines, extensions, CRLFs) one message
// may carry beyond what its data earns, so a peer can't send megabytes of
// extensions around a few bytes of data; see parse_chunk_size_line().
#define CHTTP1_MAX_CHUNK_OVERHEAD_EXCESS (16u * 1024u)

// Small character helpers
//
// True for an ASCII decimal digit; the version, status and number parsers use
// it.
static bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

// True for optional white space (SP or HTAB), which the header, token and
// chunk-extension scanners skip.
static bool is_ows(unsigned char c) { return c == ' ' || c == '\t'; }

// Tells whether c is a tchar of RFC 7230 section 3.2.6. The parser checks
// methods and header names with it, and the two HTTP modules check caller
// header names, so they all agree on what a token is.
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

// Tells whether a header value must not hold byte c: Every control byte
// except HTAB, and DEL. It's the only check that catches a bare CR or a NUL,
// since accumulate_line() leaves those in the line as content.
static bool is_invalid_value_byte(unsigned char c) {
  return (c < 0x20 && c != '\t') || c == 0x7f;
}

// Tells whether a request-target must not hold byte c. It's stricter than a
// header value: HTAB and SP are refused too (RFC 7230 sections 3.1.1 and 5.3),
// or "GET /a<HTAB>b" would reach routing as one target while a front end that
// splits on white space reads another request.
static bool is_invalid_target_byte(unsigned char c) {
  return c <= 0x20 || c == 0x7f;
}

// Overflow-checked integer parsing
//
// Parses exactly len bytes of s as an unsigned decimal number into *out, for
// Content-Length. It refuses an empty span, a non-digit and a value above
// UINT64_MAX, so a huge length can't wrap.
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

// Parses exactly len bytes of s as a hex number into *out, for chunk sizes,
// with the same refusals as parse_uint64_decimal().
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

// Line accumulation
//
// The outcome of accumulate_line().
typedef enum {
  LINE_NEED_MORE,
  LINE_READY,
  LINE_TOO_LONG,
  LINE_BAD_EOL
} line_result;

// Gives the buffer of the current line: line_spill once the line spilled,
// else line_buf. Every line reader goes through it, so a spilled line reads
// the same as a short one.
static inline char *line_ptr(chttp1_parser *parser) {
  return parser->line_spill ? parser->line_spill : parser->line_buf;
}

// Moves the line that fills line_buf into a CHTTP1_MAX_SPILL_LINE_LEN heap
// buffer from line_spill_mp, so a response header line can grow past
// CHTTP1_MAX_LINE_LEN. It gives false when spilling isn't enabled, a spill
// buffer exists already, header_budget_left couldn't hold a longer line, or
// the allocation fails. On success it sets parser->line_spill.
static bool line_spill_grow(chttp1_parser *parser, size_t header_budget_left) {
  if (!parser->line_spill_enabled || parser->line_spill) return false;
  if (header_budget_left <= CHTTP1_MAX_LINE_LEN) return false;
  char *heap =
      (char *)ccol__mem_alloc(parser->line_spill_mp, CHTTP1_MAX_SPILL_LINE_LEN);
  if (!heap) return false;
  memcpy(heap, parser->line_buf, parser->line_len);
  parser->line_spill = heap;
  return true;
}

// Gives the usable size of the spill buffer for a line: spill_budget when it's
// smaller than the buffer. That makes a line fail as soon as it passes the
// header budget, before the buffer is full.
static inline size_t spill_cap(size_t spill_budget) {
  return spill_budget && spill_budget < CHTTP1_MAX_SPILL_LINE_LEN
             ? spill_budget
             : (size_t)CHTTP1_MAX_SPILL_LINE_LEN;
}

// Appends input from *pp up to end to the current line, until CRLF, a full
// buffer or the end of input. Every line-based state of the parser uses it,
// so lines can span execute() calls. *pp always moves to show what was
// consumed: To end for NEED_MORE and TOO_LONG, past the line end for READY and
// BAD_EOL. On READY parser->line_len is the content without the CRLF.
// spill_budget is 0 for a line that must fit line_buf, or the remaining header
// budget for a response header or trailer line that may spill (it may set
// line_spill). Every LF not preceded by CR is BAD_EOL (the parser has no
// lenient mode); a bare CR stays content for the caller's control byte check.
// The buffer, capacity and length live in locals because a store through the
// char buffer could alias the parser fields.
static line_result accumulate_line(chttp1_parser *parser, const char **pp,
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

// Lets the line parsers tell malformed input from an error a callback
// reported.
typedef enum { PH_OK, PH_ERROR, PH_USER } ph_result;

// Status line
//
// Parses the response status line "HTTP/d.d SP 3DIGIT [SP reason]" in the
// current line. Any digit is accepted as the major and the minor, and any
// 3-digit status, since nonstandard codes exist. The reason and its space are
// optional, because servers send "HTTP/1.1 304". It sets http_major,
// http_minor and status_code, or parser->reason and false on failure.
static bool parse_status_line(chttp1_parser *parser) {
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

// Parses the request line "method SP target SP HTTP/d.d" in line_buf and
// fires on_request_line, for request mode. The method must be a token and the
// target free of CTL, DEL and white space; an unknown method or the form of
// the target is the caller's business. It sets http_major and http_minor and
// gives PH_OK, PH_ERROR (with parser->reason) or PH_USER when the callback
// fails.
static ph_result parse_request_line(chttp1_parser *parser) {
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

// Header and trailer lines
//
// True for a hex digit, either case; the Host check uses it for IPv6 literals
// and percent escapes.
static bool is_hexdig(unsigned char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// True for an unreserved or sub-delims byte of RFC 3986 sections 2.2 and 2.3,
// the characters a reg-name may hold; the Host check uses it.
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

// Tells whether a Host value (v, len) is uri-host [":" port] (RFC 9112
// section 3.2, RFC 3986 section 3.2.2). A value outside the grammar ("a/b",
// "u@h", white space) can name different authorities to different
// recipients, which smuggling and cache poisoning need, so the parser refuses
// it. An IPv4 address passes as a reg-name; inside brackets only the IPv6 or
// IPvFuture characters are checked. An empty host is refused.
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

// Compares a header name with a lowercase literal, ignoring ASCII case in any
// locale, so process_header_line() can find the fields it interprets.
static bool header_name_is(const char *name, size_t name_len,
                           const char *literal) {
  size_t lit_len = strlen(literal);
  return name_len == lit_len &&
         ccol_ascii_ncasecmp(name, literal, lit_len) == 0;
}

// Scans one Transfer-Encoding value (v, len, OWS already trimmed) and answers
// every question the framing rules ask about chunked on that line. It's one
// function on purpose: Two scanners with their own idea of a token boundary
// could disagree about one header, and their answers drive opposite
// decisions, which is the ambiguity request smuggling needs. A new question
// about the list goes here.
//
// Empty elements are ignored (RFC 7230 sections 7 and 4.1), so "chunked,"
// names "chunked". The outputs: chunked_count (saturating at 2; chunked twice
// is refused in both modes), nonfinal_chunked (chunked before another coding,
// refused for a request), ends_with_chunked (the last non-empty token),
// names_a_coding (a line with only empty elements leaves the framing alone)
// and names_other_coding. The caller merges these across lines.
static void transfer_encoding_scan(
    const char *v, size_t len, unsigned *chunked_count, bool *nonfinal_chunked,
    bool *ends_with_chunked, bool *names_a_coding, bool *names_other_coding) {
  unsigned count = 0;
  bool nonfinal = false;
  bool any_token = false;
  bool other = false;
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
    if (i < len) i++;
    if (tok_len == 0) continue;
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

// Reads the tokens of a Connection value and sets F_CONNECTION_CLOSE or
// F_CONNECTION_KEEP_ALIVE in parser->flags, which chttp1_should_keep_alive()
// reads. Upgrade isn't handled at all.
static void parse_connection_tokens(chttp1_parser *parser, const char *v,
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

// Gives the total header byte cap of parser: Its override, or the default.
// Header lines and the first line are both charged against it.
static size_t effective_max_total_header_bytes(const chttp1_parser *parser) {
  return parser->max_total_header_bytes_override
             ? parser->max_total_header_bytes_override
             : CHTTP1_MAX_TOTAL_HEADER_BYTES;
}

// Gives the spill_budget accumulate_line() gets for a header or trailer line:
// 0 when spilling is off, else what's left of the header byte budget, so a
// spilled line can't pass the cap.
static size_t header_spill_budget(const chttp1_parser *parser) {
  if (!parser->line_spill_enabled) return 0;
  size_t max = effective_max_total_header_bytes(parser);
  return max > parser->total_header_bytes ? max - parser->total_header_bytes
                                          : 0;
}

// Validates the current line as one "name: value" header or trailer, charges
// it to the header count and byte caps, and calls on_header or on_trailer.
// is_trailer picks the kind. A header line also updates the framing,
// Connection, Expect and Host state in parser->flags and host_count: A
// duplicate or bad Content-Length, chunked applied twice (on one line or
// across merged lines, F_CHUNKED_APPLIED), a request chunked that isn't last
// on its line, and Content-Length with chunked are refused. F_CHUNKED is
// derived again on every Transfer-Encoding line that names a coding, so
// chunked then identity fails and gzip then chunked passes; the final check
// waits for the end of the headers. A trailer touches none of that state, so
// it can't change a framing that's already acted on. It gives PH_OK, PH_ERROR
// (with parser->reason) or PH_USER.
static ph_result process_header_line(chttp1_parser *parser, bool is_trailer) {
  size_t contribution = parser->line_len + 2;
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
      parser->reason = "chunked can't be applied more than once";
      return PH_ERROR;
    }
    if (line_chunked_count > 0) parser->flags |= F_CHUNKED_APPLIED;
    if (parser->type == CHTTP1_PARSE_REQUEST && line_has_nonfinal_chunked) {
      parser->reason = "chunked must be the last Transfer-Encoding token";
      return PH_ERROR;
    }
    if (line_ends_with_chunked)
      parser->flags |= F_CHUNKED;
    else if (line_names_a_coding)
      parser->flags &= (uint16_t)~F_CHUNKED;
  } else if (parser->type == CHTTP1_PARSE_REQUEST &&
             header_name_is(name, name_len, "host")) {
    if (parser->host_count < 2) parser->host_count++;
    if (!host_value_is_valid(vstart, value_len)) {
      parser->reason = "Invalid Host header value";
      return PH_ERROR;
    }
  } else if (header_name_is(name, name_len, "connection")) {
    parse_connection_tokens(parser, vstart, value_len);
  } else if (header_name_is(name, name_len, "expect")) {
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

// Chunk size line
//
// Tells whether s[i..len) is a valid chunk-ext list (RFC 9112 section 7.1.1);
// an empty span is. The parser drops extensions but checks the grammar in
// full, because a recipient that skips everything after ';' accepts control
// bytes a stricter one refuses, and the two then frame the body
// differently.
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
      i = after_name;
      continue;
    }
    i++;
    while (i < len && is_ows((unsigned char)s[i])) i++;
    if (i >= len) return false;
    if (s[i] == '"') {
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

// Parses the current line as "1*HEXDIG chunk-ext" and stores the size in
// content_length, so the chunk data state knows how much to read. Response
// mode also accepts white space before the CRLF with no extension; request
// mode stays strict, since a looser server is half of a smuggling pair. A
// size above max_chunk_size_override sets F_CHUNK_TOO_LARGE. Each chunk adds
// its line and CRLFs to chunk_excess and earns back 16 bytes plus twice its
// data (Go's rule); passing CHTTP1_MAX_CHUNK_OVERHEAD_EXCESS fails the
// message, so extensions can't be sent for free. It gives false with
// parser->reason set on any refusal.
static bool parse_chunk_size_line(chttp1_parser *parser) {
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

// Terminal outcomes
//
// Ends the parse on malformed input: Stores reason, moves the parser to DEAD
// and gives CHTTP1_ERROR.
static chttp1_errno fail(chttp1_parser *parser, const char *reason) {
  parser->reason = reason;
  parser->state = CHTTP1_ST_DEAD;
  return CHTTP1_ERROR;
}

// Ends the parse like fail() when a helper already set parser->reason.
static chttp1_errno fail_already_set(chttp1_parser *parser) {
  parser->state = CHTTP1_ST_DEAD;
  return CHTTP1_ERROR;
}

// Ends the parse after a callback reported an error: Moves the parser to DEAD,
// sets a reason if none is set and gives CHTTP1_USER.
static chttp1_errno user_error(chttp1_parser *parser) {
  parser->state = CHTTP1_ST_DEAD;
  if (!parser->reason) parser->reason = "Callback reported an error";
  return CHTTP1_USER;
}

// Finishes a message: Fires on_message_complete, records in parser->consumed
// how much of data was used (p is the current position), moves to
// MESSAGE_DONE and gives CHTTP1_PAUSED, or CHTTP1_USER when the callback
// fails.
static chttp1_errno complete_message(chttp1_parser *parser, const char *p,
                                     const char *data) {
  if (parser->settings && parser->settings->on_message_complete) {
    int err = parser->settings->on_message_complete(parser);
    if (err != 0) return user_error(parser);
  }
  parser->consumed = (size_t)(p - data);
  parser->state = CHTTP1_ST_MESSAGE_DONE;
  return CHTTP1_PAUSED;
}

// Lifecycle
//
// Zeroes settings, so every callback is NULL until the caller sets one.
void chttp1_settings_init(chttp1_settings *settings) {
  memset(settings, 0, sizeof(*settings));
}

// The init clears only the members before line_buf (see the comment of
// line_buf in the header), so it must be the last member and the struct must
// end exactly where it ends.
_Static_assert(offsetof(chttp1_parser, line_buf) +
                       sizeof(((chttp1_parser *)0)->line_buf) ==
                   sizeof(chttp1_parser),
               "line_buf must be the last member of chttp1_parser");
_Static_assert(CHTTP1_MAX_LINE_LEN % _Alignof(max_align_t) == 0,
               "the size of line_buf must keep the end of chttp1_parser "
               "aligned");

// Prepares parser for a new message of the given type, the shared body of the
// two inits. It clears every member in front of line_buf, sets the first-line
// state and an unsafe finish state, and keeps settings.
static void parser_init_common(chttp1_parser *parser,
                               const chttp1_settings *settings,
                               chttp1_parser_type type) {
  memset(parser, 0, offsetof(chttp1_parser, line_buf));
  parser->type = type;
  parser->state = CHTTP1_ST_FIRST_LINE;
  parser->finish_state = CHTTP1_FINISH_UNSAFE;
  parser->settings = settings;
}

// Prepares parser for a new response, for chttpclient.
void chttp1_parser_init(chttp1_parser *parser,
                        const chttp1_settings *settings) {
  parser_init_common(parser, settings, CHTTP1_PARSE_RESPONSE);
}

// Prepares parser for a new request, for chttpserver.
void chttp1_parser_init_request(chttp1_parser *parser,
                                const chttp1_settings *settings) {
  parser_init_common(parser, settings, CHTTP1_PARSE_REQUEST);
}

// Lets a response parser move a long header or trailer line to a heap buffer
// from mp, because real responses can carry header lines longer than 8 KiB.
// It sets line_spill_enabled and line_spill_mp and gives false for NULL or a
// request parser.
bool chttp1_parser_enable_line_spill(chttp1_parser *parser,
                                     ccol_memmgmt_procs *mp) {
  if (!parser || parser->type != CHTTP1_PARSE_RESPONSE) return false;
  parser->line_spill_enabled = true;
  parser->line_spill_mp = mp;
  return true;
}

// Frees the spill buffer of parser, if any, so a spilling response parser
// doesn't leak, and clears line_spill so a second call does nothing.
void chttp1_parser_release(chttp1_parser *parser) {
  if (!parser || !parser->line_spill) return;
  ccol__mem_free(parser->line_spill_mp, parser->line_spill);
  parser->line_spill = NULL;
}

// Main execute loop
//
// Runs the state machine over len bytes of data, firing the callbacks; it's
// the one entry point that turns wire bytes into a message for both modules.
// It gives the outcome code (a dead or finished parser gives CHTTP1_ERROR at
// once) and updates every state field of parser. A request skips up to
// CHTTP1_MAX_LEADING_BLANK_LINES blank lines first, and the first line counts
// against the total header bytes (max_header_bytes covers the request line).
// At the end of the headers, before on_headers_complete so the caller never
// routes such a message, a request is refused for two Host lines, no Host in
// HTTP/1.1, Transfer-Encoding in HTTP/1.0 (RFC 9112 section 6.1) and a
// Transfer-Encoding that doesn't end in chunked (section 6.3). A 1xx, 204 or
// 304 response has no body. A response with a non-chunked Transfer-Encoding
// and a Content-Length is refused, since framing it by the length would leave
// the sender's extra bytes on a keep-alive connection. Then it picks the
// framing: chunked, Content-Length, no body for a request without framing, or
// EOF for a response, the only mode where finish_state is SAFE_WITH_CB.
chttp1_errno chttp1_parser_execute(chttp1_parser *parser, const char *data,
                                   size_t len) {
  const char *p = data;
  const char *end = data + len;

  if (parser->state == CHTTP1_ST_DEAD ||
      parser->state == CHTTP1_ST_MESSAGE_DONE)
    return CHTTP1_ERROR;

  while (p < end) {
    switch (parser->state) {
      case CHTTP1_ST_FIRST_LINE: {
        line_result lr = accumulate_line(parser, &p, end, 0);
        if (lr == LINE_NEED_MORE) break;
        if (lr == LINE_TOO_LONG)
          return fail(parser, parser->type == CHTTP1_PARSE_REQUEST
                                  ? "Request line too long"
                                  : "Status line too long");
        if (lr == LINE_BAD_EOL) return fail(parser, "Expected CRLF");
        if (parser->type == CHTTP1_PARSE_REQUEST && parser->line_len == 0) {
          if (++parser->leading_blank_lines > CHTTP1_MAX_LEADING_BLANK_LINES)
            return fail(parser, "Too many leading blank lines");
          parser->line_len = 0;
          break;
        }
        {
          size_t contribution = parser->line_len + 2;
          if (parser->total_header_bytes + contribution >
              effective_max_total_header_bytes(parser))
            return fail(parser, parser->type == CHTTP1_PARSE_REQUEST
                                    ? "Request line too large"
                                    : "Status line too large");
          parser->total_header_bytes += contribution;
        }
        if (parser->type == CHTTP1_PARSE_REQUEST) {
          ph_result plr = parse_request_line(parser);
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
        line_result lr =
            accumulate_line(parser, &p, end, header_spill_budget(parser));
        if (lr == LINE_NEED_MORE) break;
        if (lr == LINE_TOO_LONG) return fail(parser, "Header line too long");
        if (lr == LINE_BAD_EOL) return fail(parser, "Expected CRLF");

        if (parser->line_len == 0) {
          if (parser->type == CHTTP1_PARSE_REQUEST) {
            bool is_1_1_or_later =
                parser->http_major > 1 ||
                (parser->http_major == 1 && parser->http_minor >= 1);
            if (parser->host_count > 1)
              return fail(parser, "Duplicate Host header");
            if (parser->host_count == 0 && is_1_1_or_later)
              return fail(parser, "Missing Host header");
            if ((parser->flags & F_TRANSFER_ENCODING) && !is_1_1_or_later)
              return fail(parser, "Transfer-Encoding in an HTTP/1.0 request");
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
            no_body = true;
          }

          if (!no_body && (parser->flags & F_TRANSFER_ENCODING) &&
              !(parser->flags & F_CHUNKED) &&
              (parser->flags & F_CONTENT_LENGTH)) {
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
            parser->finish_state = CHTTP1_FINISH_UNSAFE;
            if (want_divert) {
              parser->consumed = (size_t)(p - data);
              return CHTTP1_HEADERS_ONLY;
            }
          } else if (parser->type == CHTTP1_PARSE_REQUEST) {
            parser->finish_state = CHTTP1_FINISH_SAFE;
            return complete_message(parser, p, data);
          } else {
            parser->state = CHTTP1_ST_BODY_EOF;
            parser->finish_state = CHTTP1_FINISH_SAFE_WITH_CB;
          }
        } else {
          ph_result ph = process_header_line(parser, false);
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
        line_result lr = accumulate_line(parser, &p, end, 0);
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
        line_result lr = accumulate_line(parser, &p, end, 0);
        if (lr == LINE_NEED_MORE) break;
        if (lr == LINE_TOO_LONG || lr == LINE_BAD_EOL || parser->line_len != 0)
          return fail(parser, "Expected CRLF after chunk data");
        parser->state = CHTTP1_ST_BODY_CHUNK_SIZE;
        break;
      }

      case CHTTP1_ST_BODY_CHUNK_TRAILERS: {
        line_result lr =
            accumulate_line(parser, &p, end, header_spill_budget(parser));
        if (lr == LINE_NEED_MORE) break;
        if (lr == LINE_TOO_LONG) return fail(parser, "Trailer line too long");
        if (lr == LINE_BAD_EOL) return fail(parser, "Expected CRLF");

        if (parser->line_len == 0) {
          parser->finish_state = CHTTP1_FINISH_SAFE;
          return complete_message(parser, p, data);
        }
        ph_result ph = process_header_line(parser, true);
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
        break;
      }

      case CHTTP1_ST_MESSAGE_DONE:
      case CHTTP1_ST_DEAD:
      default:
        return CHTTP1_ERROR;
    }
  }
  return CHTTP1_OK;
}

// Finish (EOF handling)
//
// Tells whether an EOF from the connection is a valid end of the message,
// because a response without framing ends only at EOF. It gives CHTTP1_PAUSED
// (after on_message_complete, moving to MESSAGE_DONE) for an EOF-delimited
// body, CHTTP1_OK at a clean boundary, CHTTP1_ERROR for a cut message, and
// CHTTP1_USER when the callback fails. A parser already at MESSAGE_DONE gives
// CHTTP1_OK, so a second call never fires on_message_complete again
// (finish_state stays SAFE_WITH_CB after an EOF-delimited message, since
// chttp1_should_keep_alive() reads it).
chttp1_errno chttp1_parser_finish(chttp1_parser *parser) {
  if (parser->state == CHTTP1_ST_DEAD) return CHTTP1_ERROR;
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

// Accessors
//
// Gives parser->consumed, the bytes of the last execute() buffer the message
// used, so the caller knows where its leftover starts.
size_t chttp1_parser_consumed(const chttp1_parser *parser) {
  return parser->consumed;
}

// Tells whether the parser reached MESSAGE_DONE, for a worker that continues a
// request across a divert.
bool chttp1_parser_message_complete(const chttp1_parser *parser) {
  return parser->state == CHTTP1_ST_MESSAGE_DONE;
}

// Sets F_CONNECTION_CLOSE, so chttp1_should_keep_alive() says no for a
// connection that lost bytes of a pipelined message.
void chttp1_parser_force_close(chttp1_parser *parser) {
  if (parser) parser->flags |= F_CONNECTION_CLOSE;
}

// Tells whether the connection may carry another message, so the caller can
// pool or close it. It reads the version, the Connection flags,
// Transfer-Encoding and finish_state. "1.1 or later" is major > 1 or 1.1+, so
// HTTP/2.0 doesn't fall into the 1.0 class. A close token wins first. An
// EOF-delimited body is the one case where finish_state stays SAFE_WITH_CB,
// so that alone rules out keep-alive.
bool chttp1_should_keep_alive(const chttp1_parser *parser) {
  bool is_1_1_or_later = parser->http_major > 1 ||
                         (parser->http_major == 1 && parser->http_minor >= 1);
  if (parser->flags & F_CONNECTION_CLOSE) return false;
  if (!is_1_1_or_later && !(parser->flags & F_CONNECTION_KEEP_ALIVE))
    return false;
  if (!is_1_1_or_later && (parser->flags & F_TRANSFER_ENCODING)) return false;

  bool needs_eof = (parser->finish_state == CHTTP1_FINISH_SAFE_WITH_CB);
  return !needs_eof;
}

// Tells whether the headers held "Expect: 100-continue" in an HTTP/1.1 or
// later message, so a server knows to answer before reading the body. RFC
// 9110 section 10.1.1 has a server ignore it from HTTP/1.0.
bool chttp1_expects_continue(const chttp1_parser *parser) {
  bool is_1_1_or_later = parser->http_major > 1 ||
                         (parser->http_major == 1 && parser->http_minor >= 1);
  return is_1_1_or_later && (parser->flags & F_EXPECT_100_CONTINUE) != 0;
}

// Tells whether F_OTHER_CODING is set, a coding this parser can't decode, so
// the caller refuses the message. False for NULL.
bool chttp1_has_other_transfer_coding(const chttp1_parser *parser) {
  return parser && (parser->flags & F_OTHER_CODING) != 0;
}

// Tells whether F_CONTENT_LENGTH is set, so the caller can look at the
// declared length. False for NULL.
bool chttp1_has_content_length(const chttp1_parser *parser) {
  return parser && (parser->flags & F_CONTENT_LENGTH) != 0;
}

// Gives content_length, which holds the declared length until the body parse
// starts, so a server can refuse an oversized body at once. 0 for NULL.
uint64_t chttp1_declared_content_length(const chttp1_parser *parser) {
  return parser ? parser->content_length : 0;
}

// Tells whether F_CHUNK_TOO_LARGE is set, so the caller answers a chunk over
// its limit as a body that's too large. False for NULL.
bool chttp1_chunk_size_limit_exceeded(const chttp1_parser *parser) {
  return parser && (parser->flags & F_CHUNK_TOO_LARGE) != 0;
}

// Tells whether the peer may still send bytes of this message, so a server
// knows to linger before it closes. Without F_HEADERS_DONE the framing isn't
// known, so the answer is true; after it, a chunked body or a Content-Length
// body with bytes left gives true. False for NULL and a complete message.
bool chttp1_request_bytes_may_remain(const chttp1_parser *parser) {
  if (!parser || parser->state == CHTTP1_ST_MESSAGE_DONE) return false;
  if (!(parser->flags & F_HEADERS_DONE)) return true;
  if (parser->flags & F_CHUNKED) return true;
  return (parser->flags & F_CONTENT_LENGTH) && parser->content_length > 0;
}

// Gives the body bytes the framing still guarantees, which a server uses as
// SO_RCVLOWAT: content_length inside a Content-Length body or a chunk's data,
// 0 in every other state and for NULL.
uint64_t chttp1_body_bytes_still_expected(const chttp1_parser *parser) {
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

// Worker-pull body and response streaming
//
// Prepares stream, the shared body of the two prepare functions: It zeroes
// the stream, records fd, tls_conn and mp, and copies leftover into a carry
// buffer from mp. On a failed copy it gives false and rolls every field back
// so the stream is all zeros, except fd, which goes to -1 so a caller that
// ignores the result can't touch fd 0.
static bool stream_prepare_common(chttp1_stream *stream, int fd, void *tls_conn,
                                  const char *leftover, size_t leftover_len,
                                  ccol_memmgmt_procs *mp) {
  memset(stream, 0, sizeof(*stream));
  stream->fd = fd;
  stream->tls = tls_conn;
  stream->mp = mp;
  stream->prepared = true;
  if (leftover_len == 0) return true;

  char *copy = (char *)ccol__mem_alloc(mp, leftover_len);
  if (!copy) {
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

// Prepares a plaintext stream on fd with a copy of leftover, for a worker that
// takes over a diverted connection.
bool chttp1_stream_prepare(chttp1_stream *stream, int fd, const char *leftover,
                           size_t leftover_len, ccol_memmgmt_procs *mp) {
  return stream_prepare_common(stream, fd, NULL, leftover, leftover_len, mp);
}

// Prepares a TLS stream on fd and tls_conn with a copy of leftover, for a
// worker that takes over a diverted TLS connection.
bool chttp1_stream_prepare_tls(chttp1_stream *stream, int fd, void *tls_conn,
                               const char *leftover, size_t leftover_len,
                               ccol_memmgmt_procs *mp) {
  return stream_prepare_common(stream, fd, tls_conn, leftover, leftover_len,
                               mp);
}

static void compute_deadline(struct timespec *deadline, int timeout_ms);
static long remaining_ms(const struct timespec *deadline);

// Waits in poll(2) until stream->fd is ready for events, so the read and write
// paths never block in the I/O call itself. It gives true when ready, false
// when timeout_ms passed (stream->timed_out set) or poll failed
// (stream->last_errno set). EINTR retries with what's left of the timeout,
// and a passed deadline still gets one zero poll.
static bool wait_for_ready(chttp1_stream *stream, short events,
                           int timeout_ms) {
  struct pollfd pfd = {.fd = stream->fd, .events = events, .revents = 0};
  struct timespec deadline;
  bool bounded = timeout_ms > 0;
  if (bounded) compute_deadline(&deadline, timeout_ms);
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
      long rem = remaining_ms(&deadline);
      wait_ms = rem > 0 ? (int)rem : 0;
    }
  }
}

// Gives the milliseconds left until deadline, rounded up (a wait must not end
// early), 0 when it passed, capped at INT_MAX. The retry loops use it so all
// their rounds share one timeout budget.
static long remaining_ms(const struct timespec *deadline) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  long long ns = (long long)(deadline->tv_sec - now.tv_sec) * 1000000000LL +
                 (deadline->tv_nsec - now.tv_nsec);
  if (ns <= 0) return 0;
  long long ms = (ns + 999999LL) / 1000000LL;
  return ms > (long long)INT_MAX ? (long)INT_MAX : (long)ms;
}

// Sets *deadline to CLOCK_MONOTONIC now plus timeout_ms, the start of every
// bounded read or write.
static void compute_deadline(struct timespec *deadline, int timeout_ms) {
  clock_gettime(CLOCK_MONOTONIC, deadline);
  deadline->tv_sec += timeout_ms / 1000;
  deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (deadline->tv_nsec >= 1000000000L) {
    deadline->tv_nsec -= 1000000000L;
    deadline->tv_sec += 1;
  }
}

// Reads up to buflen bytes into buf for a worker that pulls a body: From the
// carry-over first (freeing it once drained), then from the connection within
// timeout_ms. It gives the bytes read, 0 at EOF, or -1 with timed_out or
// last_errno set. A TLS stream reads before polling: OpenSSL may hold
// decrypted bytes poll can't see, and it's what makes a timeout of 0 still
// try once. A TLS read that would block waits for the direction
// ctls_conn_wants_write() names, since OpenSSL may need to write first. A
// plaintext stream with fd_nonblocking also reads first; otherwise it makes
// one poll with timeout_ms passed straight through (recomputing would turn a
// 0 timeout into a timeout without any check) and then one read, because a
// read on a blocking empty fd would ignore the timeout.
ssize_t chttp1_stream_read(chttp1_stream *stream, char *buf, size_t buflen,
                           int timeout_ms) {
  stream->timed_out = false;
  stream->last_errno = 0;

  if (stream->carry_pos < stream->carry_len) {
    size_t avail = stream->carry_len - stream->carry_pos;
    size_t take = avail < buflen ? avail : buflen;
    if (take > 0) memcpy(buf, stream->carry + stream->carry_pos, take);
    stream->carry_pos += take;
    if (stream->carry_pos == stream->carry_len) {
      ccol__mem_free(stream->mp, stream->carry);
      stream->carry = NULL;
      stream->carry_len = 0;
      stream->carry_pos = 0;
    }
    return (ssize_t)take;
  }

  bool has_deadline = timeout_ms >= 0;
  struct timespec deadline;
  if (has_deadline) compute_deadline(&deadline, timeout_ms);

  if (stream->tls) {
    for (;;) {
      ssize_t got = ctls_conn_read((ctls_conn *)stream->tls, buf, buflen);
      if (got >= 0) return got;
      if (!(errno == EWOULDBLOCK || errno == EAGAIN)) {
        stream->last_errno = errno;
        return -1;
      }

      int this_timeout = timeout_ms;
      if (has_deadline) {
        long rem = remaining_ms(&deadline);
        if (rem <= 0) {
          stream->timed_out = true;
          return -1;
        }
        this_timeout = (int)rem;
      }
      short want_events =
          ctls_conn_wants_write((ctls_conn *)stream->tls) ? POLLOUT : POLLIN;
      if (!wait_for_ready(stream, want_events, this_timeout)) return -1;
    }
  }

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
        long rem = remaining_ms(&deadline);
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

  ssize_t got;
  do {
    got = read(stream->fd, buf, buflen);
  } while (got < 0 && errno == EINTR);
  if (got < 0) stream->last_errno = errno;
  return got;
}

// Writes up to len bytes of buf for a worker that sends a response, within
// timeout_ms (0 with write_nonblocking). It gives the bytes written or -1 with
// timed_out or last_errno set. The deadline isn't checked on the first round,
// so a 0 timeout still tries once. A TLS write that would block waits for the
// direction OpenSSL needs. A plaintext send uses MSG_NOSIGNAL and never
// blocks (ccol_send_nb()), since POLLOUT means some room and not room for len
// bytes; EINTR and EAGAIN wait again.
ssize_t chttp1_stream_write(chttp1_stream *stream, const char *buf, size_t len,
                            int timeout_ms) {
  stream->timed_out = false;
  stream->last_errno = 0;
  if (stream->write_nonblocking) timeout_ms = 0;

  bool has_deadline = timeout_ms >= 0;
  struct timespec deadline;
  if (has_deadline) compute_deadline(&deadline, timeout_ms);

  short want_events = POLLOUT;
  bool first_attempt = true;
  for (;;) {
    int this_timeout = timeout_ms;
    if (has_deadline && !first_attempt) {
      long rem = remaining_ms(&deadline);
      if (rem <= 0) {
        stream->timed_out = true;
        return -1;
      }
      this_timeout = (int)rem;
    }
    if (!(first_attempt && stream->fd_nonblocking) &&
        !wait_for_ready(stream, want_events, this_timeout))
      return -1;
    first_attempt = false;

    if (stream->tls) {
      ssize_t written = ctls_conn_write((ctls_conn *)stream->tls, buf, len);
      if (written >= 0) return written;
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        want_events =
            ctls_conn_wants_write((ctls_conn *)stream->tls) ? POLLOUT : POLLIN;
        continue;
      }
      stream->last_errno = errno;
      return -1;
    }

    ssize_t written = ccol_send_nb(stream->fd, buf, len, CCOL_MSG_NOSIGNAL);
    if (written >= 0) return written;
    if (errno == EINTR) continue;
    if (errno == EWOULDBLOCK || errno == EAGAIN) continue;
    stream->last_errno = errno;
    return -1;
  }
}

// Writes a and then b as one write where it can, within timeout_ms, so the
// server sends a response head and body together. It gives the bytes written
// across both, or -1 like chttp1_stream_write(). TLS joins parts that fit one
// record into a stack buffer (one encryption and one send), else writes a and
// tries b with no wait. Plaintext loops over one sendmsg(2) with MSG_NOSIGNAL
// and MSG_DONTWAIT; where send ignores MSG_DONTWAIT (the Darwin emulation)
// ccol_send_room() caps each attempt.
ssize_t chttp1_stream_writev2(chttp1_stream *stream, const char *a, size_t alen,
                              const char *b, size_t blen, int timeout_ms) {
  stream->timed_out = false;
  stream->last_errno = 0;
  if (stream->write_nonblocking) timeout_ms = 0;
  if (alen == 0 && blen == 0) return 0;

  if (stream->tls) {
    if (alen == 0) return chttp1_stream_write(stream, b, blen, timeout_ms);
    if (blen == 0) return chttp1_stream_write(stream, a, alen, timeout_ms);
    if (blen <= CHTTP1_TLS_COALESCE_LIMIT &&
        alen <= CHTTP1_TLS_COALESCE_LIMIT - blen) {
      char joined[CHTTP1_TLS_COALESCE_LIMIT];
      memcpy(joined, a, alen);
      memcpy(joined + alen, b, blen);
      return chttp1_stream_write(stream, joined, alen + blen, timeout_ms);
    }
    ssize_t n = chttp1_stream_write(stream, a, alen, timeout_ms);
    if (n < (ssize_t)alen) return n;
    ssize_t m = chttp1_stream_write(stream, b, blen, 0);
    stream->timed_out = false;
    stream->last_errno = 0;
    return m > 0 ? n + m : n;
  }

  struct iovec iov[2];
  int cnt = 0;
  if (alen) iov[cnt++] = (struct iovec){.iov_base = (void *)a, .iov_len = alen};
  if (blen) iov[cnt++] = (struct iovec){.iov_base = (void *)b, .iov_len = blen};
  struct msghdr msg = {.msg_iov = iov, .msg_iovlen = (size_t)cnt};
  bool has_deadline = timeout_ms >= 0;
  struct timespec deadline;
  if (has_deadline) compute_deadline(&deadline, timeout_ms);
  bool first_attempt = true;
  for (;;) {
    int this_timeout = timeout_ms;
    if (has_deadline && !first_attempt) {
      long rem = remaining_ms(&deadline);
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
#if defined(CCOL__EMULATE_DARWIN_SOCK)
    struct iovec part[2];
    struct msghdr pmsg = msg;
    size_t room = ccol_send_room(stream->fd, alen + blen);
    if (room == 0) continue;
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

// Gives stream->timed_out, so a caller can tell a deadline from an error.
bool chttp1_stream_timed_out(const chttp1_stream *stream) {
  return stream->timed_out;
}

// Gives stream->last_errno, the errno of the last failed call.
int chttp1_stream_last_error(const chttp1_stream *stream) {
  return stream->last_errno;
}

// Puts a copy of buf in front of the unread carry-over, in one new buffer from
// stream->mp, so bytes of the next pipelined request survive the end of this
// one. It gives false, with the carry-over unchanged, when the sizes would
// overflow or the allocation fails.
bool chttp1_stream_push_back_leftover(chttp1_stream *stream, const char *buf,
                                      size_t len) {
  if (len == 0) return true;
  size_t existing = stream->carry_len - stream->carry_pos;
  if (len > SIZE_MAX - existing) return false;
  size_t total = len + existing;
  char *nc = (char *)ccol__mem_alloc(stream->mp, total);
  if (!nc) return false;
  memcpy(nc, buf, len);
  if (existing) memcpy(nc + len, stream->carry + stream->carry_pos, existing);
  ccol__mem_free(stream->mp, stream->carry);
  stream->carry = nc;
  stream->carry_len = total;
  stream->carry_pos = 0;
  return true;
}

// Hands the unread carry-over to the caller in a new buffer from stream->mp
// and empties it, so the server can parse the next pipelined request; *len_out
// gets its length. With nothing left it gives NULL and 0. On a failed
// allocation it gives NULL, keeps the carry-over for chttp1_stream_release()
// and reports the lost bytes in *len_out.
char *chttp1_stream_take_leftover(chttp1_stream *stream, size_t *len_out) {
  if (len_out) *len_out = 0;
  if (stream->carry_pos >= stream->carry_len) return NULL;
  size_t remaining = stream->carry_len - stream->carry_pos;
  char *out = (char *)ccol__mem_alloc(stream->mp, remaining);
  if (!out) {
    if (len_out) *len_out = remaining;
    return NULL;
  }
  memcpy(out, stream->carry + stream->carry_pos, remaining);
  ccol__mem_free(stream->mp, stream->carry);
  stream->carry = NULL;
  stream->carry_len = 0;
  stream->carry_pos = 0;
  if (len_out) *len_out = remaining;
  return out;
}

// Frees the carry-over and marks the stream released, when the worker is done
// with it. It never closes fd.
void chttp1_stream_release(chttp1_stream *stream) {
  if (stream->carry) {
    ccol__mem_free(stream->mp, stream->carry);
    stream->carry = NULL;
  }
  stream->carry_len = 0;
  stream->carry_pos = 0;
  stream->released = true;
}
