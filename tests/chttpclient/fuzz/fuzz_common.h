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
 * Shared driver for the request/response chttp1_parser libFuzzer harnesses
 * (fuzz_chttp1_request.c, fuzz_chttp1_response.c). Not a public header of
 * this library; lives only under this fuzz/ directory.
 *
 * Feeds a fuzz input to an already-initialized parser in randomly-sized
 * chunks, exercising the byte-boundary-fragmentation class this parser's
 * own tests_parser.c suite already treats as high-value (see that file's
 * assert_fragmented_matches_oneshot helper). The chunk boundaries are
 * derived deterministically from the input's own bytes via a small
 * splitmix64-style PRNG, never from real randomness or the clock, since
 * libFuzzer requires that feeding the same input twice always reaches the
 * same code path (a crash must be reproducible from its saved input file
 * alone).
 */

#ifndef FUZZ_COMMON_H
#define FUZZ_COMMON_H

#include <chttp1_parser.h>
#include <stddef.h>
#include <stdint.h>

static uint64_t fuzz_rng_state;

static uint64_t fuzz_rng_next(void) {
  uint64_t z = (fuzz_rng_state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

static void fuzz_rng_seed(const uint8_t *data, size_t size) {
  uint64_t seed = 0;
  size_t n = size < sizeof(seed) ? size : sizeof(seed);
  for (size_t i = 0; i < n; i++) seed = (seed << 8) | data[i];
  /* A zero seed would make every subsequent fuzz_rng_next() call return 0
   * forever (splitmix64's own degenerate fixed point); fall back to a
   * fixed nonzero seed for the (rare, short-input) case where the derived
   * seed is genuinely zero. */
  fuzz_rng_state = seed ? seed : 0x9E3779B97F4A7C15ULL;
}

/*
 * Feeds all of data[0..size) to parser across randomly-sized chunks.
 *
 * Correctly threads CHTTP1_HEADERS_ONLY's own contract (request mode only):
 * chttp1_parser_consumed() reports how much of the JUST-FED chunk was
 * actually used for the header block, and any remainder is real, valid body
 * bytes still needing to be fed, not something to be discarded the way
 * CHTTP1_PAUSED's own leftover-after-response-mode-pause bytes are.
 */
static void fuzz_feed_fragmented(chttp1_parser_t *parser, const uint8_t *data,
                                 size_t size) {
  fuzz_rng_seed(data, size);

  size_t pos = 0;
  while (pos < size) {
    size_t remaining = size - pos;
    size_t chunk = 1 + (size_t)(fuzz_rng_next() % remaining);
    size_t chunk_start = pos;
    chttp1_errno_t rv =
        chttp1_parser_execute(parser, (const char *)data + pos, chunk);

    if (rv == CHTTP1_OK) {
      pos = chunk_start + chunk;
      continue;
    }
    if (rv == CHTTP1_HEADERS_ONLY) {
      pos = chunk_start + chttp1_parser_consumed(parser);
      continue;
    }
    /* CHTTP1_PAUSED (message complete), CHTTP1_ERROR, or CHTTP1_USER: no
     * further feeding is valid on this parser instance regardless of
     * which one, per chttp1_parser_execute's own documented contract. */
    break;
  }
}

#endif /* FUZZ_COMMON_H */
