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

/*
 * The libFuzzer target for the URL and redirect layer of chttpclient.c.
 *
 * Every other fuzz target of this project drives a parser that reads bytes
 * off a socket, while this one drives the layer ABOVE that parser, the one
 * that turns text into a connection target. Its input is just as
 * attacker-influenced: a Location header comes from whatever server the
 * previous hop reached, and the client resolves it against the URL of that
 * hop before it opens the next connection. A defect here is a
 * server-side-request-forgery primitive and not only a crash.
 *
 * The input is split on the first NUL byte into a base URL and a Location
 * reference. libFuzzer finds that split on its own from the corpus.
 *
 * Three entry points run on every input:
 *   _chttp_parse_url_for_tests             - the URL grammar
 *   _chttp_resolve_redirect_url_for_tests  - RFC 3986 reference resolution
 *   _chttp_redirect_transport_allowed_for_tests - the AF_UNIX screen
 *
 * The target also asserts the one property of the transport screen that
 * matters for security, on every input: a base that is NOT a unix-socket
 * target can never resolve to one that IS. That is what stops any ordinary
 * http or https server from pointing the next hop at a local socket with a
 * "Location: http+unix://..." reply.
 */

#include <common.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* The white-box hooks of chttpclient.c, which exist only under
 * RUNNING_UNIT_TESTS (the build of this target defines it). chttp_url_t is a
 * file-local type there, so each hook flattens its result. */
ccol_retval_t _chttp_parse_url_for_tests(
    const char *url, bool *is_https_out, bool *is_ipv6_out, char **host_out,
    uint16_t *port_out, char **path_and_query_out, char **origin_key_out,
    char **userinfo_authorization_out, bool *is_unix_out,
    char **unix_socket_path_out);
char *_chttp_resolve_redirect_url_for_tests(const char *base_url,
                                            const char *location);
bool _chttp_redirect_transport_allowed_for_tests(const char *base_url,
                                                 const char *location,
                                                 bool prevent_downgrade,
                                                 char **resolved_out);

static bool is_unix_url(const char *u) {
  return u && strncasecmp(u, "http+unix://", 12) == 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  /* Two NUL-terminated strings out of one buffer. Both are bounded copies,
   * so neither depends on the input carrying a terminator of its own. */
  char base[4096];
  char location[4096];
  size_t split = size;
  for (size_t i = 0; i < size; i++) {
    if (data[i] == 0) {
      split = i;
      break;
    }
  }
  size_t base_len = split < sizeof(base) - 1 ? split : sizeof(base) - 1;
  memcpy(base, data, base_len);
  base[base_len] = '\0';

  size_t loc_off = split < size ? split + 1 : size;
  size_t loc_len = size - loc_off;
  if (loc_len > sizeof(location) - 1) loc_len = sizeof(location) - 1;
  memcpy(location, data + loc_off, loc_len);
  location[loc_len] = '\0';

  /* 1. The URL grammar on its own. Every out-parameter is taken, so every
   *    allocation that the parse makes is freed here, and a leak shows up
   *    under the LeakSanitizer that libFuzzer runs with. */
  {
    bool is_https = false, is_ipv6 = false, is_unix = false;
    char *host = NULL, *pq = NULL, *origin = NULL, *auth = NULL, *sock = NULL;
    uint16_t port = 0;
    if (_chttp_parse_url_for_tests(base, &is_https, &is_ipv6, &host, &port, &pq,
                                   &origin, &auth, &is_unix,
                                   &sock) == ccol_success) {
      /* A parse that succeeds always gives a path that starts with '/'.
       * _serialize_request writes it straight into the request line, so
       * anything else would be a malformed request on the wire. */
      if (pq && pq[0] != '/') abort();
      /* A unix target has a socket path and no host; a network target is
       * the other way round. Nothing may report both or neither. */
      if (is_unix && (!sock || host)) abort();
      if (!is_unix && (sock || !host)) abort();
      /* A network host is a uri-host of RFC 3986 SS3.2.2. It reaches the
       * Host header, SNI and the resolver as it is, so no byte outside
       * that grammar may survive the parse: no control byte, no space, no
       * byte at or above 0x80, no delimiter of the authority, and none of
       * the characters that RFC 3986 never allows. */
      if (host) {
        if (!host[0]) abort();
        for (const unsigned char *c = (const unsigned char *)host; *c; c++) {
          if (*c <= 0x20 || *c >= 0x7f) abort();
          if (strchr("\"#/<>?@[\\]^`{|}", *c)) abort();
          if (!is_ipv6 && *c == ':') abort();
        }
      }
      free(host);
      free(pq);
      free(origin);
      free(auth);
      free(sock);
    }
  }

  /* 2. Reference resolution, which needs a base that parses. */
  free(_chttp_resolve_redirect_url_for_tests(base, location));

  /* 3. The transport screen, with its security property asserted. The
   * AF_UNIX rule holds whatever the TLS-downgrade opt-in says, so both
   * settings of that opt-in are screened. */
  for (int prevent_downgrade = 0; prevent_downgrade <= 1; prevent_downgrade++) {
    char *resolved = NULL;
    bool allowed = _chttp_redirect_transport_allowed_for_tests(
        base, location, prevent_downgrade != 0, &resolved);
    if (allowed && resolved && is_unix_url(resolved) && !is_unix_url(base)) {
      /* A base that is not a unix-socket target resolved to one that is,
       * and the screen let it through. That is the server-side request
       * forgery primitive that the screen exists to close. */
      abort();
    }
    free(resolved);
  }
  return 0;
}
