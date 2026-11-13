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

// Internal only. A small hand-written HTTP/1.1 parser: chttpclient.c uses it
// in response mode and chttpserver.c in request mode. No public header
// includes it.
//
// Both modes share everything after the first line (headers, chunks, trailers
// and the size caps). Request mode adds chttp1_parser_init_request(),
// on_request_line, CHTTP1_HEADERS_DIVERT_BODY and the request body rule of
// RFC 7230 section 3.3. The parser does no I/O. A request parser, and a
// response parser that never enabled line spilling, owns nothing, so it can
// live on the stack and needs no release: The line buffer is a fixed array in
// the struct, and its size is also the line length cap. Body bytes never pass
// through that buffer.
//
// Three caps hold back a peer that sends endless headers: The line length,
// the header count and the total header bytes. Trailers count against the
// same count and byte budget as the headers.
//
// Repeated Transfer-Encoding lines form one merged list (RFC 7230 section
// 3.2.2), and every framing rule applies to that list. Empty elements are
// ignored, so "chunked" followed by an empty Transfer-Encoding line is the
// list "chunked". Both modes refuse "chunked" more than once and a
// Content-Length beside any Transfer-Encoding. A list whose final coding isn't
// chunked fails a request (its length can't be known) and makes a response
// EOF-delimited, never keep-alive. A request chunk-size line follows RFC 9112
// section 7.1 (whitespace after the size only before ';'); response mode also
// accepts "5 \r\n", as Go and curl do.
//
// Nothing here is thread-safe. One thread owns a parser at a time; in request
// mode ownership may move once, at a CHTTP1_HEADERS_ONLY divert, from a reactor
// thread to a worker, and the caller sequences that handoff. chttp1_stream
// follows the same rule.

#include <common.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h>
#endif
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// Result codes
//
// The outcome of chttp1_parser_execute() and chttp1_parser_finish(). OK means
// the message isn't complete, keep feeding. PAUSED means it completed (see the
// contracts of chttp1_parser_execute()). USER means a callback reported an
// error. ERROR is every parse failure; parser->reason names it for logs and
// tests.
typedef enum {
  CHTTP1_OK = 0,
  CHTTP1_PAUSED,
  CHTTP1_USER,
  CHTTP1_ERROR,
  // Request mode only: on_headers_complete gave CHTTP1_HEADERS_DIVERT_BODY, so
  // the parse stopped right after the header block, before any body byte.
  // chttp1_parser_consumed() is the size of the header block in this call's
  // buffer; the rest belongs to the caller (for example as the leftover of
  // chttp1_stream_prepare()). Unlike PAUSED this isn't terminal: State already
  // sits at the body framing, and more chttp1_parser_execute() calls, from the
  // thread that owns the connection, continue the body.
  CHTTP1_HEADERS_ONLY
} chttp1_errno;

// The grammar of the first line. The init function sets it and nothing
// changes it later.
typedef enum {
  CHTTP1_PARSE_RESPONSE = 0,
  CHTTP1_PARSE_REQUEST
} chttp1_parser_type;

// What on_headers_complete returns. HAS_BODY (0) and NO_BODY (1) work in both
// modes, so a bare 0 or 1 means the same. DIVERT_BODY is for request mode only;
// a response parser answers it with CHTTP1_USER.
#define CHTTP1_HEADERS_HAS_BODY 0
#define CHTTP1_HEADERS_NO_BODY 1
#define CHTTP1_HEADERS_DIVERT_BODY 2

// Internal state
//
// The parser's states. They're here only so chttp1_parser is a complete type;
// callers use the accessors below instead. FIRST_LINE is the status line or
// the request line, as parser->type says, and DEAD means a hard error after
// which execute() does nothing.
typedef enum {
  CHTTP1_ST_FIRST_LINE = 0,
  CHTTP1_ST_HEADERS,
  CHTTP1_ST_BODY_CONTENT_LENGTH,
  CHTTP1_ST_BODY_CHUNK_SIZE,
  CHTTP1_ST_BODY_CHUNK_DATA,
  CHTTP1_ST_BODY_CHUNK_CRLF,
  CHTTP1_ST_BODY_CHUNK_TRAILERS,
  CHTTP1_ST_BODY_EOF,
  CHTTP1_ST_MESSAGE_DONE,
  CHTTP1_ST_DEAD
} chttp1__state;

// What an EOF means at the current point: A clean boundary, the end of an
// EOF-delimited body, or a cut message. chttp1_parser_finish() reads it, so
// its caller needn't know the framing.
typedef enum {
  CHTTP1_FINISH_SAFE = 0,
  CHTTP1_FINISH_SAFE_WITH_CB,
  CHTTP1_FINISH_UNSAFE
} chttp1__finish_state;

// The size of the line buffer in the struct, and with it the longest status,
// header or chunk-size line (except a spilled response header line). It's here
// because it decides the struct layout; the header count and total byte caps
// are private to chttp1_parser.c.
#define CHTTP1_MAX_LINE_LEN 8192

// The capacity of the heap buffer a spilled response header or trailer line
// moves to, so a client accepts the long header lines real servers send. The
// total header byte cap bounds it too.
#define CHTTP1_MAX_SPILL_LINE_LEN (64 * 1024)

typedef struct chttp1_parser chttp1_parser;

// The callbacks. Each is checked for NULL, so set only the ones you need, and
// a callback that returns non-zero stops the parse with CHTTP1_USER (rules of
// on_headers_complete below). Strings point into the parser's line buffer and
// are valid only during the call.
typedef struct {
  // Once per complete header line of the header block, with the name as sent
  // (not lower-cased) and the value trimmed of SP and HTAB. Trailer fields
  // never come here, so a trailer can't replace a header.
  int (*on_header)(chttp1_parser *p, const char *name, size_t name_len,
                   const char *value, size_t value_len);

  // Once per trailer line after the last chunk (RFC 7230 section 4.1.2),
  // validated and charged like a header line. Trailers never change the framing
  // or what the accessors report. NULL (the default) drops each trailer after
  // validation.
  int (*on_trailer)(chttp1_parser *p, const char *name, size_t name_len,
                    const char *value, size_t value_len);

  // Request mode only. Once, after the request line is validated and before the
  // first header; http_major and http_minor are set. method is a token and
  // target any run without CTL, DEL or white space; the form of the target and
  // whether the method is known are left to the caller.
  int (*on_request_line)(chttp1_parser *p, const char *method,
                         size_t method_len, const char *target,
                         size_t target_len);

  // Once, after the blank line that ends the headers (status_code is set in
  // response mode). Return HAS_BODY normally, NO_BODY when the caller knows no
  // body follows (a HEAD response, whatever Content-Length says), or, in
  // request mode, DIVERT_BODY to stop with CHTTP1_HEADERS_ONLY. Any other value
  // stops with CHTTP1_USER, so a callback can report a real error. DIVERT_BODY
  // acts as HAS_BODY when there's nothing to divert: No framing at all, or
  // "Content-Length: 0". A chunked body is always diverted, since its emptiness
  // shows only on the first chunk-size line.
  int (*on_headers_complete)(chttp1_parser *p);

  // Zero or more times with body bytes, after the framing is taken off. at
  // points into the caller's buffer and is valid only during the call.
  int (*on_body)(chttp1_parser *p, const char *at, size_t len);

  // Once, when the whole message is parsed. execute() and finish() report the
  // completion with CHTTP1_PAUSED themselves.
  int (*on_message_complete)(chttp1_parser *p);
} chttp1_settings;

// The parser. It owns a resource only after a response parser with spilling
// enabled has spilled a line.
struct chttp1_parser {
  // For the caller: Set it after the init and read it in the callbacks.
  void *data;

  // Response mode only, set before on_headers_complete; 0 in request mode.
  int status_code;

  chttp1_parser_type type;

  // A static description of the last rejection, or NULL. Only logs and tests
  // read it.
  const char *reason;

  // Internal state; callers neither read nor write it. content_length is the
  // bytes left of a Content-Length body or of the current chunk. chunk_excess
  // is the chunk framing overhead beyond what the data earned so far.
  chttp1__state state;
  chttp1__finish_state finish_state;
  uint8_t http_major;
  uint8_t http_minor;
  uint16_t flags;
  uint64_t content_length;
  uint64_t chunk_excess;

  size_t header_count;
  size_t total_header_bytes;
  // Request mode: Blank lines accepted before the request line (RFC 7230
  // section 3.5).
  size_t leading_blank_lines;

  // Request mode: Host lines seen, saturating. Judged at the end of the header
  // block: More than one, or none in HTTP/1.1, is a 400 (RFC 7230 section 5.4),
  // because two Host lines let a front end and a back end disagree about the
  // authority.
  uint8_t host_count;

  // Response mode: Set by chttp1_parser_enable_line_spill().
  bool line_spill_enabled;

  // Overrides of the built-in header count and total byte caps, 0 for the
  // default. Set them before the first byte; chttpserver does, for
  // max_header_bytes.
  size_t max_header_count_override;
  size_t max_total_header_bytes_override;

  // A cap on the declared size of one chunk, 0 for none. A larger chunk-size
  // line fails at once (chttp1_chunk_size_limit_exceeded()). Without it a peer
  // can declare a huge chunk, send nothing and hold a reader until a timeout,
  // so every request parser with a body limit should set it.
  uint64_t max_chunk_size_override;

  // The length of the current line, and the heap buffer (with its procs) a
  // response parser moves a long line to.
  size_t line_len;
  char *line_spill;
  ccol_memmgmt_procs *line_spill_mp;

  // What chttp1_parser_consumed() reports after a pause.
  size_t consumed;

  const chttp1_settings *settings;

  // Accumulates the status, request, header, trailer and chunk-size lines,
  // across execute() calls. It must stay the last member: An init clears only
  // the members in front of it, which is safe because nothing reads a byte of
  // line_buf or line_spill not written since the reset. Every reader stops at
  // line_len, and accumulate_line() looks back only at bytes it counted. A new
  // reader must keep that. The alignment makes the struct end exactly at the
  // buffer on every target (armhf would pad after it otherwise).
  _Alignas(max_align_t) char line_buf[CHTTP1_MAX_LINE_LEN];
};

// Grammar helpers
//
// Tells whether c is a tchar (RFC 7230 section 3.2.6), a byte a header name or
// a method token may hold. The parser checks wire bytes with it, and
// chttp_request_set_header() and chttpsvr_resp_set_header() check a caller's
// header name, so all three agree on what a token is.
bool chttp1_is_tchar(unsigned char c);

// Lifecycle
//
// Zeroes settings, so every callback is NULL and a caller sets only the ones
// it needs before passing it to an init.
void chttp1_settings_init(chttp1_settings *settings);

// Prepares parser for a new response; chttpclient calls it for every message
// it reads. settings must outlive the parser. It can't fail. To reuse a
// parser, init it again (release a spilling parser first). The init clears
// the state and not the 8 KiB line buffer.
void chttp1_parser_init(chttp1_parser *parser, const chttp1_settings *settings);

// Prepares parser for a new request; chttpserver calls it for every request.
// Unlike chttp1_parser_init(), the parser reads a request line, fires
// on_request_line, gives a message without framing no body (never EOF
// framing) and accepts CHTTP1_HEADERS_DIVERT_BODY.
void chttp1_parser_init_request(chttp1_parser *parser,
                                const chttp1_settings *settings);

// Lets a response parser take header and trailer lines longer than
// CHTTP1_MAX_LINE_LEN, up to CHTTP1_MAX_SPILL_LINE_LEN and within the total
// header cap, because real servers send long Set-Cookie or CSP lines. Call it
// after the init, before the first byte. A line that fills the fixed buffer
// moves to a heap buffer from mp (NULL for the default) kept for the rest of
// the message; the status and chunk-size lines stay at CHTTP1_MAX_LINE_LEN
// until a header line spills. A failed allocation reports "line too long". It
// gives false for a request parser and a NULL parser. After a true return,
// call chttp1_parser_release() on every path that drops or reinitializes the
// parser.
bool chttp1_parser_enable_line_spill(chttp1_parser *parser,
                                     ccol_memmgmt_procs *mp);

// Frees the spill buffer of a parser, if it has one, so a spilling response
// parser doesn't leak. Safe on any initialized parser and more than once.
// Reinitialize before feeding the parser again.
void chttp1_parser_release(chttp1_parser *parser);

// Parsing
//
// Feeds len bytes of data to the parser and fires the callbacks; both modules
// call it once per read. CHTTP1_OK always means the whole buffer was consumed
// (partial lines and counts carry over between calls). After CHTTP1_PAUSED
// don't feed this parser again; the bytes from chttp1_parser_consumed() to
// len weren't consumed (a client drops them, a server keeps them for the next
// pipelined request through chttp1_stream_push_back_leftover()).
// CHTTP1_HEADERS_ONLY isn't terminal, and feeding more continues the body. It
// gives OK, PAUSED, HEADERS_ONLY, USER or ERROR (see parser->reason).
chttp1_errno chttp1_parser_execute(chttp1_parser *parser, const char *data,
                                   size_t len);

// Tells whether an EOF from the connection ends the message validly; call it
// when a read gives 0, because a response without framing ends only at EOF.
// For an EOF-delimited body it fires on_message_complete and gives
// CHTTP1_PAUSED. It gives CHTTP1_OK at a clean boundary, CHTTP1_ERROR when the
// EOF cut a message, and CHTTP1_USER (parser dead) when on_message_complete
// failed.
chttp1_errno chttp1_parser_finish(chttp1_parser *parser);

// Gives how many bytes of the last execute() buffer the message used, so the
// caller knows where the next message or the body starts. Valid only right
// after CHTTP1_PAUSED or CHTTP1_HEADERS_ONLY.
size_t chttp1_parser_consumed(const chttp1_parser *parser);

// Tells whether the whole message is parsed. A worker that continues a
// request across a divert asks this instead of reading parser->state.
bool chttp1_parser_message_complete(const chttp1_parser *parser);

// Tells whether the connection may carry another message (RFC 7230 section
// 6.3), which decides whether the caller pools or closes it. HTTP/1.1
// defaults to yes, HTTP/1.0 to no unless Connection: keep-alive. A close token
// wins for every version, on any line. An HTTP/1.0 message with
// Transfer-Encoding (RFC 9112 section 6.1), a body only the close delimits and
// a chttp1_parser_force_close() all give false. Valid once the message
// completed.
bool chttp1_should_keep_alive(const chttp1_parser *parser);

// Makes chttp1_should_keep_alive() false for this message, as a close token
// would. A caller that lost bytes of a pipelined next message calls it, so
// the connection closes instead of waiting for data already sent. The next
// init clears it; NULL is ignored.
void chttp1_parser_force_close(chttp1_parser *parser);

// Tells whether the headers hold exactly "Expect: 100-continue", so a server
// can send "100 Continue" (or a final status) before reading the body. Any
// other Expect value is the caller's to refuse, with 417 for example. Always
// false for HTTP/1.0 (RFC 9110 section 10.1.1). Valid once
// on_headers_complete fired. The parser sends nothing itself. A response
// parser sees a 100 as a complete bodiless message, and the client parses the
// final response with a new parser.
bool chttp1_expects_continue(const chttp1_parser *parser);

// Tells whether the merged Transfer-Encoding list names a coding other than
// chunked. Only chunked is decoded, so a request with "gzip, chunked" would
// deliver gzip bytes; a server that doesn't decode them answers 501 (RFC 9112
// section 6.1), and the client fails such a response. Valid once
// on_headers_complete fired.
bool chttp1_has_other_transfer_coding(const chttp1_parser *parser);

// Tells whether the headers set up Content-Length framing, so the caller can
// check the declared size before it diverts or reads the body. Valid once
// on_headers_complete fired; such a message carries no Transfer-Encoding,
// since every combination of the two is refused earlier.
bool chttp1_has_content_length(const chttp1_parser *parser);

// Gives the declared Content-Length. chttpserver refuses an oversized
// declaration with it at headers-complete, so a peer that never sends the
// body can't hold a reader until a timeout. Valid only while
// chttp1_has_content_length() is true and before the body parse starts (the
// field counts down, and counts the current chunk for a chunked body). 0 for
// NULL.
uint64_t chttp1_declared_content_length(const chttp1_parser *parser);

// Tells whether the last CHTTP1_ERROR was a chunk-size line above
// max_chunk_size_override, so the caller can answer it as a body that's too
// large (413) instead of a generic failure. False in every other case.
bool chttp1_chunk_size_limit_exceeded(const chttp1_parser *parser);

// Gives the body bytes the framing still guarantees: The rest of a
// Content-Length body, the rest of the current chunk's data, and 0 on a
// chunk-size line, a chunk CRLF, in the trailers and in every other state. A
// server uses it as SO_RCVLOWAT, since a peer that follows its framing sends
// that much before it waits for an answer.
uint64_t chttp1_body_bytes_still_expected(const chttp1_parser *parser);

// Tells whether the peer may still send bytes of this message. chttpserver
// lingers before a close when it's true, because closing on unread input
// makes the kernel send a reset that can destroy the response. False once the
// message is complete and for a header block without body framing (even when
// refused at on_headers_complete); true for an unfinished body (paused,
// diverted or failed inside it) and before or after an error in the head,
// where the framing is unknown. False for NULL.
bool chttp1_request_bytes_may_remain(const chttp1_parser *parser);

// Worker-pull body and response streaming
//
// A reactor-free helper that lets a worker thread read and write a connection
// with an absolute timeout per call. It first delivers the carry-over bytes a
// reactor thread read along with the headers. The usual flow: Parse the head
// until CHTTP1_HEADERS_ONLY, hand the fd and the unconsumed bytes to a worker,
// which prepares a stream with them and feeds chttp1_stream_read() output to
// the same parser until CHTTP1_PAUSED. chttp1_stream_write() serves the
// response phase.
typedef struct chttp1_stream {
  int fd;

  // An opaque ctls_conn, or NULL for plain text. With it, reads and writes go
  // through ctls and fd serves only the poll(2) waits.
  void *tls;

  // A heap copy of the leftover bytes, drained through carry_pos before fd is
  // read. For TLS they're already decrypted.
  char *carry;
  size_t carry_len;
  size_t carry_pos;

  // The allocator of every stream buffer, the carry-over and what
  // chttp1_stream_take_leftover() hands out included (NULL for malloc), so a
  // server's custom allocator also covers diverted bodies.
  ccol_memmgmt_procs *mp;

  // errno of the last failed call, and whether that call failed by its deadline
  // (then last_errno means nothing).
  int last_errno;
  bool timed_out;
  bool prepared;
  bool released;
  // When true, writes never wait: A full socket counts as a timeout. For a
  // caller that waits for writability on its own reactor. Cleared by prepare;
  // no effect on reads.
  bool write_nonblocking;
  // The caller promises fd is non-blocking, so I/O is tried before any poll.
  // Cleared by prepare, because a plaintext read on a blocking fd must poll
  // first or it ignores the timeout. Plaintext writes use MSG_DONTWAIT either
  // way.
  bool fd_nonblocking;
} chttp1_stream;

// Prepares stream for a worker that reads a request body and writes the
// response on fd, a connected stream socket. leftover (NULL when leftover_len
// is 0) holds bytes the reactor already read off fd; they're copied and
// delivered first. Writes use MSG_NOSIGNAL, so a gone peer gives EPIPE or
// ECONNRESET and never SIGPIPE; a non-socket fails with ENOTSOCK. Call it
// before another thread can touch fd. mp is kept until
// chttp1_stream_release() and also frees what chttp1_stream_take_leftover()
// returns. It gives false only when the copy can't be allocated; the stream is
// then all zeros.
bool chttp1_stream_prepare(chttp1_stream *stream, int fd, const char *leftover,
                           size_t leftover_len, ccol_memmgmt_procs *mp);

// Prepares stream like chttp1_stream_prepare(), for a TLS connection whose
// handshake is done. I/O goes through ctls_conn_read() and ctls_conn_write();
// fd is only polled, since ctls has no poll of its own. leftover is already
// decrypted. The caller keeps ownership of tls_conn and destroys it after
// chttp1_stream_release().
bool chttp1_stream_prepare_tls(chttp1_stream *stream, int fd, void *tls_conn,
                               const char *leftover, size_t leftover_len,
                               ccol_memmgmt_procs *mp);

// Reads up to buflen bytes into buf for a worker that pulls a body, waiting at
// most timeout_ms (< 0 forever, 0 not at all). Carry-over comes first, without
// touching fd. Then a TLS stream, or a plaintext one with fd_nonblocking,
// tries a read before polling (OpenSSL may already hold decrypted bytes); a
// plaintext stream on a possibly blocking fd polls first. After EAGAIN it
// polls and retries within the same total timeout, so TLS records that yield
// no application bytes are retried quietly. It gives the bytes read, 0 on a
// clean EOF or close_notify, or -1 on an error or a timeout
// (chttp1_stream_timed_out() tells which).
ssize_t chttp1_stream_read(chttp1_stream *stream, char *buf, size_t buflen,
                           int timeout_ms);

// Tells whether carry-over bytes remain, which the next read returns without
// touching fd, so a caller knows it needn't wait for readiness.
static inline bool chttp1_stream_has_carry(const chttp1_stream *stream) {
  return stream->carry_pos < stream->carry_len;
}

// Writes up to len bytes of buf (encrypted for TLS) for a worker that sends a
// response, polling for at most timeout_ms while the socket is full, so a slow
// reader can't hold a worker without bound. It gives the bytes written, which
// may be fewer than len, or -1 on an error or a timeout.
ssize_t chttp1_stream_write(chttp1_stream *stream, const char *buf, size_t len,
                            int timeout_ms);

// The largest head plus body chttp1_stream_writev2() copies into one TLS
// record (the plaintext limit of a record), so a response head and a small
// body cost one encryption and one send.
#define CHTTP1_TLS_COALESCE_LIMIT 16384

// Writes a and then b as one write where it can; either may be empty. The
// server sends a response head and body with it in one system call or one TLS
// record. With fd_nonblocking it writes first and polls only after a full
// socket, otherwise it polls first. Plaintext uses one sendmsg(2) with
// MSG_NOSIGNAL. TLS copies both into one record when they fit
// CHTTP1_TLS_COALESCE_LIMIT, otherwise it writes a and then tries b at once. A
// TLS write that would block is retried by the next call with the unsent
// rest. It gives the bytes written across a and b, or -1 on an error or a
// timeout.
ssize_t chttp1_stream_writev2(chttp1_stream *stream, const char *a, size_t alen,
                              const char *b, size_t blen, int timeout_ms);

// Tells whether the last read or write gave -1 because its deadline passed,
// so the caller can tell a slow peer from a broken connection.
bool chttp1_stream_timed_out(const chttp1_stream *stream);

// Gives the errno of the last failed read or write, for the caller's error
// handling; 0 after a success, a timeout or a read from the carry-over.
int chttp1_stream_last_error(const chttp1_stream *stream);

// Puts a copy of buf in front of the carry-over. A message can end before the
// bytes just read do; those trailing bytes are the next pipelined request, and
// this keeps them for chttp1_stream_take_leftover(). It gives false only when
// the copy can't be allocated, and the carry-over stays as it was.
bool chttp1_stream_push_back_leftover(chttp1_stream *stream, const char *buf,
                                      size_t len);

// Hands the unread carry-over to the caller, so the server can parse the next
// pipelined request from it. Call it once, right before
// chttp1_stream_release(), or that request is lost. The buffer comes from the
// stream's mp; free it with ccol__mem_free() and that mp. NULL with *len_out
// 0 means nothing was left; NULL with *len_out above 0 means the copy failed,
// those bytes are gone and the caller must close the connection. The
// carry-over is empty afterwards.
char *chttp1_stream_take_leftover(chttp1_stream *stream, size_t *len_out);

// Frees the stream's carry-over when the worker is done with it. It never
// closes fd. Call it once; it's safe on a zeroed stream and a second call
// does nothing.
void chttp1_stream_release(chttp1_stream *stream);
