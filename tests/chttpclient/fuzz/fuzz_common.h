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

// The driver that the two libFuzzer harnesses of chttp1_parser share
// (fuzz_chttp1_request.c and fuzz_chttp1_response.c). It feeds a fuzz input to
// the parser in chunks of random size, so the fuzzer also finds the bugs that
// show up only when a message is cut at some byte boundary.
//
// The chunk sizes come from the input bytes through a small splitmix64 PRNG,
// never from the clock. libFuzzer needs two runs of one input to take the same
// path, and a crash must reproduce from its saved input file alone.

#ifndef FUZZ_COMMON_H
#define FUZZ_COMMON_H

#include <internal/chttp1_parser.h>
#include <stddef.h>
#include <stdint.h>

// The state of the chunk-size PRNG, seeded from each input.
static uint64_t fuzz_rng_state;

// Gives the next splitmix64 value and advances fuzz_rng_state; it picks the
// chunk sizes of fuzz_feed_fragmented().
static uint64_t fuzz_rng_next(void) {
  uint64_t z = (fuzz_rng_state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

// Seeds fuzz_rng_state from the first 8 bytes of data, so the chunk sizes are a
// function of the input alone and a crash reproduces from its saved file. A
// seed of zero is replaced by a fixed constant, because zero is a fixed point
// of splitmix64 and every later value would be 0.
static void fuzz_rng_seed(const uint8_t *data, size_t size) {
  uint64_t seed = 0;
  size_t n = size < sizeof(seed) ? size : sizeof(seed);
  for (size_t i = 0; i < n; i++) seed = (seed << 8) | data[i];
  fuzz_rng_state = seed ? seed : 0x9E3779B97F4A7C15ULL;
}

// Feeds all of data[0..size) to parser in chunks of random size, so the fuzzer
// reaches the bugs that depend on where a message is cut. Both harnesses call
// it; it reseeds fuzz_rng_state from the input. After CHTTP1_HEADERS_ONLY
// (request mode only) chttp1_parser_consumed() says how much of the chunk just
// fed went into the header block, and the rest is body that we feed again
// instead of dropping it. CHTTP1_PAUSED, CHTTP1_ERROR and CHTTP1_USER all end
// the feed, because the parser takes no more input after them.
static void fuzz_feed_fragmented(chttp1_parser *parser, const uint8_t *data,
                                 size_t size) {
  fuzz_rng_seed(data, size);

  size_t pos = 0;
  while (pos < size) {
    size_t remaining = size - pos;
    size_t chunk = 1 + (size_t)(fuzz_rng_next() % remaining);
    size_t chunk_start = pos;
    chttp1_errno rv =
        chttp1_parser_execute(parser, (const char *)data + pos, chunk);

    if (rv == CHTTP1_OK) {
      pos = chunk_start + chunk;
      continue;
    }
    if (rv == CHTTP1_HEADERS_ONLY) {
      pos = chunk_start + chttp1_parser_consumed(parser);
      continue;
    }
    break;
  }
}

#endif
