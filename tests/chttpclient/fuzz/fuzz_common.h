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
 * The driver that the two libFuzzer harnesses of chttp1_parser share,
 * fuzz_chttp1_request.c and fuzz_chttp1_response.c. It isn't a public header of
 * the library and it only lives under this fuzz/ directory.
 *
 * It feeds a fuzz input to a parser that is already initialized, in chunks of
 * random size, so the fuzzer also hits the bugs that only show up when a
 * message is cut at some byte boundary. tests_parser.c already cares about that
 * class of bug, see its assert_fragmented_matches_oneshot helper.
 *
 * The chunk boundaries are taken from the bytes of the input itself, through a
 * small splitmix64 style PRNG, and never from real randomness or the clock.
 * libFuzzer needs two runs of the same input to take the same code path, and a
 * crash must be reproducible from its saved input file alone.
 */

#ifndef FUZZ_COMMON_H
#define FUZZ_COMMON_H

#include <internal/chttp1_parser.h>
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
  /* splitmix64 has a degenerate fixed point at zero, where every later
   * fuzz_rng_next() call returns 0 for ever, so we fall back to a fixed seed
   * that isn't zero. The derived seed is really zero only for a rare, short
   * input. */
  fuzz_rng_state = seed ? seed : 0x9E3779B97F4A7C15ULL;
}

/*
 * Feeds all of data[0..size) to the parser, in chunks of random size.
 *
 * It follows the contract of CHTTP1_HEADERS_ONLY, which only request mode has:
 * chttp1_parser_consumed() tells how much of the chunk we JUST FED went into
 * the header block, and what is left over is real body bytes that we have to
 * feed instead of throwing them away. The leftover bytes after a response mode
 * pause (CHTTP1_PAUSED) are a different thing.
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
    /* We get here for CHTTP1_PAUSED (the message is complete), CHTTP1_ERROR or
     * CHTTP1_USER, and for all three the documented contract of
     * chttp1_parser_execute says the parser can't be fed any more. */
    break;
  }
}

#endif /* FUZZ_COMMON_H */
