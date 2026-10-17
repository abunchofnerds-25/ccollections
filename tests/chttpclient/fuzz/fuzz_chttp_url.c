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
 * All the other fuzz targets of the project drive a parser that reads bytes off
 * a socket. This one drives the layer above it, which turns text into a
 * connection target, and its input is just as much in an attacker's hands: a
 * Location header comes from whatever server the previous hop reached, and the
 * client resolves it against that hop's URL before opening the next connection.
 * So a defect here can be a server-side request forgery primitive and not only
 * a crash.
 *
 * The input is split at the first NUL byte into a base URL and a Location
 * reference, and libFuzzer discovers that split from the corpus.
 *
 * These entry points run on every input:
 *   _chttp_parse_url_for_tests             - the URL grammar
 *   _chttp_resolve_redirect_url_for_tests  - RFC 3986 reference resolution
 *   _chttp_redirect_transport_allowed_for_tests - the AF_UNIX screen
 *
 * On every input we also assert the one property of the transport screen that
 * matters for security: a base that is NOT a unix-socket target never resolves
 * to one that IS. Without that, any ordinary http or https server could point
 * the next hop at a local socket with a "Location: http+unix://..." reply.
 */

#include <common.h>
#include <internal/test_helpers/chttpclient.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool is_unix_url(const char *u) {
  return u && strncasecmp(u, "http+unix://", 12) == 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  /* Two NUL-terminated strings out of one buffer. Both copies are bounded, so
   * neither one depends on the input having a terminator of its own. */
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

  /* 1. The URL grammar alone. We take every out-parameter, so every
   *    allocation the parse makes is freed here and a leak shows up in the
   *    LeakSanitizer that libFuzzer runs with. */
  {
    bool is_https = false, is_ipv6 = false, is_unix = false;
    char *host = NULL, *pq = NULL, *origin = NULL, *auth = NULL, *sock = NULL;
    uint16_t port = 0;
    if (_chttp_parse_url_for_tests(base, &is_https, &is_ipv6, &host, &port, &pq,
                                   &origin, &auth, &is_unix,
                                   &sock) == ccol_success) {
      /* When the parse succeeds the path always starts with '/'.
       * _serialize_request writes it straight into the request line, and
       * anything else would put a malformed request on the wire. */
      if (pq && pq[0] != '/') abort();
      /* A unix target has a socket path and no host, a network target is the
       * other way round, and nothing may report both or neither. */
      if (is_unix && (!sock || host)) abort();
      if (!is_unix && (sock || !host)) abort();
      /* A network host is a uri-host of RFC 3986 SS3.2.2. It goes to the Host
       * header, SNI and the resolver as it is, so no byte outside that grammar
       * may survive the parse: no control byte, no space, no byte at or above
       * 0x80, no delimiter of the authority and none of the characters that
       * RFC 3986 never allows. */
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

  /* 3. The transport screen, with its security property asserted. The AF_UNIX
   * rule holds whatever the TLS-downgrade opt-in says, so we screen with both
   * settings of it. */
  for (int prevent_downgrade = 0; prevent_downgrade <= 1; prevent_downgrade++) {
    char *resolved = NULL;
    bool allowed = _chttp_redirect_transport_allowed_for_tests(
        base, location, prevent_downgrade != 0, &resolved);
    if (allowed && resolved && is_unix_url(resolved) && !is_unix_url(base)) {
      /* A base that isn't a unix-socket target resolved to one that is, and the
       * screen let it through. That's exactly the server-side request forgery
       * primitive the screen is there to stop. */
      abort();
    }
    free(resolved);
  }
  return 0;
}
