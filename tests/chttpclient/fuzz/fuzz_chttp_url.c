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

// The libFuzzer target for the URL and redirect layer of chttpclient.c. Its
// input is in an attacker's hands too: A Location header comes from whatever
// server the previous hop reached, so a defect here can be a server-side
// request forgery primitive and not only a crash.
//
// The input is split at the first NUL byte into a base URL and a Location
// reference. Every input goes through the URL grammar, reference resolution and
// the AF_UNIX transport screen, and we assert that a base that isn't a
// unix-socket target never resolves, through the screen, to one that is.

#include <common.h>
#include <internal/test_helpers/chttpclient.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Tells whether u is an http+unix:// URL, so the screen check knows which side
// of the AF_UNIX rule a URL is on.
static bool is_unix_url(const char *u) {
  return u && strncasecmp(u, "http+unix://", 12) == 0;
}

// The libFuzzer entry point. It splits the input into base and location (both
// copies bounded, so neither needs a terminator in the input) and runs three
// checks, aborting when a property breaks; it always gives 0.
// 1. The URL grammar. Every out-parameter is taken and freed, so LeakSanitizer
//    sees a leak. A parse that succeeds must give a path that starts with '/',
//    exactly one of a socket path and a host, and a host that matches RFC 3986
//    uri-host (no control byte, space, byte >= 0x80 or authority delimiter).
// 2. Reference resolution of location against base.
// 3. The transport screen, with both settings of the TLS-downgrade opt-in,
//    because the AF_UNIX rule holds whatever that flag says.
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
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

  {
    bool is_https = false, is_ipv6 = false, is_unix = false;
    char *host = NULL, *pq = NULL, *origin = NULL, *auth = NULL, *sock = NULL;
    uint16_t port = 0;
    if (chttp__parse_url_for_tests(base, &is_https, &is_ipv6, &host, &port, &pq,
                                   &origin, &auth, &is_unix,
                                   &sock) == ccol_success) {
      if (pq && pq[0] != '/') abort();
      if (is_unix && (!sock || host)) abort();
      if (!is_unix && (sock || !host)) abort();
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

  free(chttp__resolve_redirect_url_for_tests(base, location));

  for (int prevent_downgrade = 0; prevent_downgrade <= 1; prevent_downgrade++) {
    char *resolved = NULL;
    bool allowed = chttp__redirect_transport_allowed_for_tests(
        base, location, prevent_downgrade != 0, &resolved);
    if (allowed && resolved && is_unix_url(resolved) && !is_unix_url(base)) {
      abort();
    }
    free(resolved);
  }
  return 0;
}
