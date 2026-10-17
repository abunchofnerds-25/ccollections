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

/**
 * @file test_helpers/chashmap.h
 * @brief INTERNAL ONLY. The test-only hooks of chashmap.
 *
 * src/chashmap.c defines these hooks and the tests call them. They count the
 * work that probes, chains and rebuilds do, expose the hash functions and the
 * secret, and read state of the adaptive hashing that the public API doesn't
 * show, so a test can assert on work instead of on time.
 *
 * All of it exists only in a build that defines RUNNING_UNIT_TESTS. Every test
 * suite defines it and the shipped library never does, so none of this is
 * installed or exported.
 */

#include <chashmap.h>

#ifdef RUNNING_UNIT_TESTS
/* Process-wide counters of the slots that open-addressing probes look at,
 * and a reset to 0. A key set that the hash doesn't spread drives the count
 * up while every answer stays correct. */
unsigned long long chashmap_oa_probe_steps_for_tests(void);
void chashmap_reset_oa_probe_steps_for_tests(void);

/* The number of successful open-addressing rehashes across the process. A
 * rehash at an unchanged capacity counts too. */
unsigned long long chashmap_oa_rehashes_for_tests(void);

/* How many entries backward-shift deletion moved, and how many occupied
 * slots its walk looked at, across the process. */
unsigned long long chashmap_oa_shift_moves_for_tests(void);
unsigned long long chashmap_oa_shift_steps_for_tests(void);

/* How many chain nodes the searches of the separate-chaining backend
 * visited across the process, and a reset to 0. */
unsigned long long chashmap_sc_node_visits_for_tests(void);
void chashmap_reset_sc_node_visits_for_tests(void);

/* The fast attempts that growing a keyed map makes, across the process: how
 * many started, how many failed, the work of the failed ones, and the work,
 * count and capacity of the last one that failed. */
unsigned long long chashmap_retries_for_tests(void);
unsigned long long chashmap_retry_failures_for_tests(void);
unsigned long long chashmap_retry_failed_work_for_tests(void);
void chashmap_last_failed_retry_for_tests(size_t *work, size_t *count,
                                          size_t *capacity);

/* The net count of chashmap iterators that chashmap_begin_iter() allocated
 * and that nothing has freed yet, across every map in the process. */
long chashmap_iter_outstanding_count_for_tests(void);

/* The current size of the bucket array or the slot array, and the element
 * counts at which the next scale-up and the next scale-down start, so a test
 * can check the resize thresholds of both backends. */
size_t chmap_get_bucket_arr_size(chmap chm);
size_t chmap_get_elem_count_to_scale_up(chmap chm);
size_t chmap_get_elem_count_to_scale_down(chmap chm);

/* The SipHash state of the 128-bit key (k0, k1), the way a map keeps it. */
void chashmap_sip_state_for_tests(uint64_t k0, uint64_t k1, uint64_t sip_v[4]);

/* SipHash-1-3 of a buffer from an explicit state, which a test compares with
 * an independent byte-wise implementation of the algorithm. */
uint64_t chashmap_siphash13_for_tests(const uint64_t sip_v[4],
                                      const void *key_ptr, size_t key_size);

/* The byte hash of a map from an explicit state, which a test uses with a
 * state that anybody can know, to build keys that collide for it. */
size_t chashmap_byte_hash_for_tests(const uint64_t sip_v[4],
                                    const void *key_ptr, size_t key_size);

/* The secret a map hashes with: the four words of the SipHash state, then
 * int_seed, then byte_seed. An open-addressing map gives 0 for the SipHash
 * state and for byte_seed. */
void chashmap_hash_secret_for_tests(chmap chm, uint64_t out[6]);

/* The four key words that the fallback path derives when getrandom(2) can't
 * answer. */
void chashmap_fallback_hash_key_for_tests(uint64_t out[4]);

/* XXH64 of a buffer with an explicit seed, which a test compares with the
 * published vectors and with an independent byte-wise implementation. */
uint64_t chashmap_xxh64_for_tests(const void *key_ptr, size_t key_size,
                                  uint64_t seed);

/* Whether the map hashes in the keyed mode, and a switch to it through the
 * same function that the detection calls. The switch gives
 * ccol_not_enough_memory, leaving the map fast, when rebuilding an
 * open-addressing table can't allocate. */
bool chashmap_is_keyed_for_tests(chmap chm);
ccol_retval_t chashmap_switch_to_keyed_for_tests(chmap chm);

/* The probe_span of an open-addressing map, or 0 for the other backend. */
size_t chashmap_oa_probe_span_for_tests(chmap chm);

/* The cap on a single insert into a fast open-addressing table of capacity
 * slots. */
size_t chashmap_oa_insert_cap_for_tests(size_t capacity);

/* The window bound of an open-addressing table of capacity slots, and of a
 * separate-chaining map with bucket_count buckets, whose window started at
 * start_count entries. */
uint32_t chashmap_oa_window_bound_for_tests(size_t start_count,
                                            size_t capacity);
uint32_t chashmap_sc_window_bound_for_tests(size_t start_count,
                                            size_t bucket_count);

/* The bound and the running sum of the current insert window of a map. */
uint32_t chashmap_window_bound_of_for_tests(chmap chm);
uint32_t chashmap_insert_window_for_tests(chmap chm);

/* The length of the longest bucket chain of a separate-chaining map, which
 * is what the worst lookup costs, or 0 for a map of the other backend. */
size_t chashmap_sc_longest_chain_for_tests(chmap chm);

/* The hash that a separate-chaining map stored for the entry whose key bytes
 * equal key_ptr, or 0 when there's none. */
size_t chashmap_sc_stored_hash_for_tests(chmap chm, const void *key_ptr,
                                         size_t key_size);

/* The home slot of a key in an open-addressing map, or SIZE_MAX for a map of
 * the other backend, so a test can pick keys that form a cluster at a chosen
 * place, such as one that wraps past the last slot. */
size_t chashmap_oa_home_slot_for_tests(chmap chm, const void *key_ptr,
                                       size_t key_size);

/* Checks the structure of an open-addressing map: every occupied slot is
 * reachable from its home slot through occupied slots only, and exactly
 * count slots are occupied. Gives true for a map of the other backend. */
bool chashmap_oa_check_invariants_for_tests(chmap chm);
#endif /* RUNNING_UNIT_TESTS */
