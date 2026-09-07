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

/* Shared seeded PRNG for randomized-operation invariant tests across
 * test suites (not part of the library itself; test-only). A fixed seed
 * (rather than one freshly drawn from the OS) is deliberate: on a test
 * failure, printing the seed via ccol_invariants_print_seed lets the exact
 * same operation sequence be reproduced by hardcoding that seed. */
typedef struct {
  uint64_t state;
} ccol_invariants_rng_t;

static inline void ccol_invariants_seed(ccol_invariants_rng_t *rng,
                                        uint64_t seed) {
  rng->state = seed;
}

/* splitmix64, chosen for being small, dependency-free, and good enough to
 * drive test operation sequences (not for anything cryptographic). */
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

/* A fixed default seed, used unless a test picks its own. Printed on every
 * run (not just on failure) so a seed is always visible in test output. */
#define CCOL_INVARIANTS_DEFAULT_SEED 0xC0FFEEULL

static inline void ccol_invariants_print_seed(const char *test_name,
                                              uint64_t seed) {
  fprintf(stderr, "[invariants] %s: seed = 0x%llX\n", test_name,
          (unsigned long long)seed);
}
