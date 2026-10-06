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

#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* A shared pseudo random number generator (PRNG) with a seed, for the invariant
 * tests that use a random sequence of operations. Several test suites use it.
 * It is not part of the library, and only the tests use it. The seed is fixed,
 * and does not come from the OS. This is deliberate: on a test failure,
 * ccol_invariants_print_seed prints the seed, and you can then hardcode that
 * seed to reproduce the exact same sequence of operations. */
typedef struct {
  uint64_t state;
} ccol_invariants_rng_t;

static inline void ccol_invariants_seed(ccol_invariants_rng_t *rng,
                                        uint64_t seed) {
  rng->state = seed;
}

/* splitmix64. It is small, it needs no dependency, and it is good enough to
 * drive a sequence of test operations. It is not for cryptography. */
static inline uint64_t ccol_invariants_next_u64(ccol_invariants_rng_t *rng) {
  uint64_t z = (rng->state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

/* Returns a value in [0, bound). bound must be > 0. */
static inline uint64_t ccol_invariants_next_bounded(ccol_invariants_rng_t *rng,
                                                    uint64_t bound) {
  return ccol_invariants_next_u64(rng) % bound;
}

/* The fixed default seed. A test uses it unless the test chooses its own
 * seed. Every run prints a seed, and not only a run that fails, so a seed is
 * always visible in the test output. */
#define CCOL_INVARIANTS_DEFAULT_SEED 0xC0FFEEULL

static inline void ccol_invariants_print_seed(const char *test_name,
                                              uint64_t seed) {
  fprintf(stderr, "[invariants] %s: seed = 0x%llX\n", test_name,
          (unsigned long long)seed);
}
