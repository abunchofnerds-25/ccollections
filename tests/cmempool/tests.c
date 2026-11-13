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

#include <cmempool.h>
#include <errno.h>
#include <fcntl.h>
#include <internal/test_helpers/cmempool.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#if defined(TEST_NO_LD_WRAP)
#include <dlfcn.h>
#endif
#include <tau/tau.h>
#include <test_coverage.h>
#include <test_sanitizer.h>
#include <test_signals.h>
#include <time.h>
#include <unistd.h>

TAU_MAIN()

// Resets the signal state this binary inherited, before any test runs. The
// forked children report through their wait status, which an inherited signal
// state can break (see test_reset_inherited_signals()).
__attribute__((constructor)) static void _reset_inherited_signals(void) {
  test_reset_inherited_signals();
}

// Destroy *mp and *rmp; the cleanup functions of SCOPED_MEMPOOL and
// SCOPED_R_MEMPOOL. A failing REQUIRE returns at once, and without them the
// pool would leak on exactly those runs. A test that destroys its pool itself
// leaves NULL behind, so the cleanup then does nothing.
static void mp_scoped_release(cmempool **mp) {
  if (*mp) cmempool_destroy(*mp);
}
static void rmp_scoped_release(crmempool **rmp) {
  if (*rmp) crmempool_destroy(*rmp);
}

// Declare a pool handle, or a ranged pool handle, that is destroyed on every
// way out of the scope, so a failing test doesn't leak it.
#define SCOPED_MEMPOOL(name) \
  cmempool *name __attribute__((cleanup(mp_scoped_release)))
#define SCOPED_R_MEMPOOL(name) \
  crmempool *name __attribute__((cleanup(rmp_scoped_release)))

TEST(cmempools, create_fails) {
  char *err;
  cmempool *mp = cmempool_create(0, sizeof(int), false, false, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);

  mp = cmempool_create(16, 0, false, false, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);

  ccol_memmgmt_procs m_procs = {
      .malloc = malloc, .calloc = calloc, .realloc = realloc, .free = NULL};
  mp = cmempool_create_mp(16, 0, false, false, &m_procs, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// The sizes are valid, so only the procs check can refuse the call.
TEST(cmempools, create_rejects_incomplete_procs_with_valid_size_params) {
  ccol_memmgmt_procs m_procs = {
      .malloc = malloc, .calloc = calloc, .realloc = realloc, .free = NULL};
  char *err = NULL;
  cmempool *mp =
      cmempool_create_mp(16, sizeof(int), false, false, &m_procs, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(cmempools,
     create_from_preallocated_rejects_incomplete_procs_with_valid_params) {
  _Alignas(cmempool__entry_align) uint8_t buf[256];
  ccol_memmgmt_procs m_procs = {
      .malloc = NULL, .calloc = calloc, .realloc = realloc, .free = free};
  char *err = NULL;
  SCOPED_MEMPOOL(mp) = cmempool_create_from_preallocated_buffer_mp(
      buf, sizeof(buf), 16, false, false, &m_procs, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(cmempools, create_rejects_elem_size_whose_stride_overflows) {
  char *err = NULL;
  cmempool *mp = cmempool_create(2, SIZE_MAX - 8, false, true, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);

  err = NULL;
  mp = cmempool_create(1, SIZE_MAX, false, true, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// The wrapped product is tiny (17 * count is 2^64 + 16), so a build without
// the check gets a 16-byte block and dies in the layout loop instead of
// returning NULL.
TEST(cmempools, create_rejects_a_count_and_size_whose_block_overflows) {
#if SIZE_MAX > 0xFFFFFFFFu
  const size_t wrapping_count = (size_t)1085102592571150096ULL;
#else
  const size_t wrapping_count = (size_t)252645136u;
#endif
  char *err = NULL;
  cmempool *mp = cmempool_create(wrapping_count, 16, false, true, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);

  err = NULL;
  SCOPED_MEMPOOL(ok) = cmempool_create(64, 16, false, true, &err);
  bool ok_built = (ok != NULL);
  if (ok_built) cmempool_destroy(ok);
  REQUIRE_TRUE(ok_built);
}

TEST(cmempools,
     create_from_preallocated_rejects_elem_size_whose_stride_overflows) {
  _Alignas(cmempool__entry_align) uint8_t buf[256];
  memset(buf, 0, sizeof(buf));
  char *err = NULL;
  SCOPED_MEMPOOL(mp) = cmempool_create_from_preallocated_buffer(
      buf, sizeof(buf), SIZE_MAX - 8, false, true, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(cmempools, pool_entries_beyond_first_are_properly_aligned) {
  SCOPED_MEMPOOL(mp) = cmempool_create(8, 9, false, true, NULL);
  REQUIRE_NE((void *)mp, NULL);

  void *ptrs[8];
  size_t obtained = 0;
  size_t misaligned_entries = 0;
  for (size_t i = 0; i < 8; ++i) {
    ptrs[i] = cmempool_alloc_entry(mp);
    if (!ptrs[i]) continue;
    ++obtained;
    if ((uintptr_t)ptrs[i] % _Alignof(max_align_t) != 0) ++misaligned_entries;
  }

  for (size_t i = 0; i < 8; ++i) {
    if (ptrs[i]) cmempool_free_entry(mp, ptrs[i]);
  }

  cmempool_destroy(mp);

  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_EQ(obtained, (size_t)8);
  REQUIRE_EQ(misaligned_entries, (size_t)0);
}

// A rejected value can't go into the buffer macro itself (the binary wouldn't
// compile), so this and the next tests call the guard directly.
TEST(cmempools, buffer_params_fit_rejects_zero_elem_count) {
  REQUIRE_FALSE(cmempool__buffer_params_fit(0, 64));

  REQUIRE_TRUE(cmempool__buffer_params_fit(100, 1));
  REQUIRE_TRUE(cmempool__buffer_params_fit(1, 1));
  REQUIRE_TRUE(cmempool__buffer_params_fit(32768, 256));
  REQUIRE_TRUE(cmempool__buffer_params_fit(50, 9));
}

TEST(cmempools, buffer_params_fit_rejects_elem_size_near_size_max) {
  for (size_t back_off = 0; back_off < 8; ++back_off) {
    size_t elem_size = SIZE_MAX - back_off;
    REQUIRE_FALSE(cmempool__buffer_params_fit(1, elem_size));
    REQUIRE_FALSE(cmempool__buffer_params_fit(100, elem_size));
  }

#if CMEMPOOL_COMPACT_LAYOUT
  const size_t largest = SIZE_MAX - (cmempool__entry_align - 1);
#else
#if SIZE_MAX > 0xFFFFFFFFu
#define CCOL_TEST_LARGEST_STRIDE_EXP 40
#else
#define CCOL_TEST_LARGEST_STRIDE_EXP 31
#endif
  const size_t largest = (size_t)1 << CCOL_TEST_LARGEST_STRIDE_EXP;
#undef CCOL_TEST_LARGEST_STRIDE_EXP
#endif
  REQUIRE_EQ(cmempool__stride(largest), largest);
  REQUIRE_TRUE(cmempool__buffer_params_fit(1, largest));
  REQUIRE_FALSE(cmempool__buffer_params_fit(1, largest + 1));

  REQUIRE_FALSE(cmempool__buffer_params_fit(SIZE_MAX / 2, 64));
}

// Takes every entry, frees it and takes it again, so every position goes
// through the index arithmetic twice. A stride rounded too weakly misaligns
// every second entry, and a reciprocal off by one in the compact layout makes
// two entries share a status byte, which the second pass catches as a double
// free.
TEST(cmempools, every_entry_position_round_trips_for_an_awkward_elem_size) {
  enum { kCount = 500, kElemSize = 96 };
  cmempool *mp = cmempool_create(kCount, kElemSize, false, true, NULL);
  REQUIRE_NE((void *)mp, NULL);

  const size_t stride = cmempool__stride(kElemSize);

  void *ptrs[kCount] = {0};
  size_t obtained = 0;
  size_t wrongly_spaced = 0;
  for (size_t pass = 0; pass < 2; ++pass) {
    for (size_t i = 0; i < kCount; ++i) {
      ptrs[i] = cmempool_alloc_entry(mp);
      if (ptrs[i]) ++obtained;
    }
    for (size_t i = 0; i + 1 < kCount; ++i) {
      for (size_t j = i + 1; j < kCount; ++j) {
        if (ptrs[j] && ptrs[i] && (uintptr_t)ptrs[j] < (uintptr_t)ptrs[i]) {
          void *t = ptrs[i];
          ptrs[i] = ptrs[j];
          ptrs[j] = t;
        }
      }
    }
    for (size_t i = 0; i + 1 < kCount; ++i) {
      if (!ptrs[i] || !ptrs[i + 1]) continue;
      if ((uintptr_t)ptrs[i + 1] - (uintptr_t)ptrs[i] != stride) {
        ++wrongly_spaced;
      }
    }
    for (size_t i = 0; i < kCount; ++i) {
      if (ptrs[i]) cmempool_free_entry(mp, ptrs[i]);
      ptrs[i] = NULL;
    }
  }

  cmempool_destroy(mp);

  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_EQ(obtained, (size_t)(2 * kCount));
  REQUIRE_EQ(wrongly_spaced, (size_t)0);
}

// An address partway into an entry must be refused, not rounded down, or two
// addresses would name one entry. The compact layout checks this by
// multiplying back, so it gets its own test.
TEST(cmempools, freeing_an_interior_pointer_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    SCOPED_MEMPOOL(mp) = cmempool_create(8, 96, false, true, NULL);
    void *entry = cmempool_alloc_entry(mp);
    if (!entry) test_child_exit(2);
    void *interior = (uint8_t *)entry + 8;
    cmempool_free_entry(mp, interior);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// A cached pool must report strictly more bytes than its capacity accounts
// for, and a pool without a cache exactly that much.
TEST(cmempools, allocated_bytes_reports_the_reserve_that_capacity_hides) {
  const size_t elem_size = 64;
  const size_t stride = cmempool__stride(elem_size);
  const size_t count = 1024;

  const size_t exactly_capacity = count * stride + count;

  cmempool *cached = cmempool_create(count, elem_size, false, false, NULL);
  cmempool *uncached = cmempool_create(count, elem_size, false, true, NULL);
  const bool both_created = (cached != NULL && uncached != NULL);

  size_t cached_bytes = 0, uncached_bytes = 0;
  size_t cached_capacity = 0, uncached_capacity = 0;
  if (both_created) {
    cached_bytes = cmempool_allocated_bytes(cached);
    uncached_bytes = cmempool_allocated_bytes(uncached);
    cached_capacity = cmempool_total_capacity(cached);
    uncached_capacity = cmempool_total_capacity(uncached);
  }

  if (cached) cmempool_destroy(cached);
  if (uncached) cmempool_destroy(uncached);

  REQUIRE_TRUE(both_created);
  REQUIRE_EQ(cached_capacity, count);
  REQUIRE_EQ(uncached_capacity, count);
  REQUIRE_GT(cached_bytes, exactly_capacity);
  REQUIRE_EQ(uncached_bytes, exactly_capacity);
}

TEST(r_mempools, allocated_bytes_sums_every_tier) {
  crmempool *rmp =
      crmempool_create(4, 8, 6, crmempool_fallback_disabled, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  size_t at_least = 0;
  for (size_t sz = 16; sz <= 256; sz *= 2) {
    at_least += crmempool_total_capacity(rmp, sz) * sz;
  }
  const size_t reported = crmempool_allocated_bytes(rmp);

  crmempool_destroy(rmp);

  REQUIRE_GT(reported, at_least);
  REQUIRE_GT(at_least, (size_t)0);
}

TEST(cmempools, every_entry_is_aligned_for_any_object_type) {
  size_t misaligned = 0;
  size_t checked = 0;
  bool all_created = true;

  for (size_t elem_size = 8; elem_size <= 72; elem_size += 8) {
    cmempool *mp = cmempool_create(8, elem_size, false, true, NULL);
    if (!mp) {
      all_created = false;
      continue;
    }
    void *entries[8] = {0};
    for (size_t i = 0; i < 8; ++i) {
      entries[i] = cmempool_alloc_entry(mp);
      if (entries[i]) {
        ++checked;
        if ((uintptr_t)entries[i] % _Alignof(max_align_t) != 0) ++misaligned;
      }
    }
    for (size_t i = 0; i < 8; ++i) {
      if (entries[i]) cmempool_free_entry(mp, entries[i]);
    }
    cmempool_destroy(mp);
  }

  REQUIRE_TRUE(all_created);
  REQUIRE_EQ(checked, (size_t)72);
  REQUIRE_EQ(misaligned, (size_t)0);
}

// Heap pools, preallocated buffers, ranged segments and fallback entries can
// each lose the alignment on their own, so every path is checked.
TEST(cmempools, every_creation_path_yields_aligned_entries) {
  size_t misaligned = 0;
  size_t checked = 0;
  bool all_created = true;

  CMEMPOOL_DECLARE_PREALLOCATED_BUFFER(pbuf, 8, 9);
  SCOPED_MEMPOOL(pmp) = cmempool_create_from_preallocated_buffer(
      pbuf, sizeof(pbuf), 9, false, true, NULL);
  if (!pmp) {
    all_created = false;
  } else {
    void *e[8] = {0};
    for (size_t i = 0; i < 8; ++i) {
      e[i] = cmempool_alloc_entry(pmp);
      if (e[i]) {
        ++checked;
        if ((uintptr_t)e[i] % _Alignof(max_align_t) != 0) ++misaligned;
      }
    }
    for (size_t i = 0; i < 8; ++i) {
      if (e[i]) cmempool_free_entry(pmp, e[i]);
    }
    cmempool_destroy(pmp);
  }

  SCOPED_MEMPOOL(fmp) = cmempool_create(2, 9, true, true, NULL);
  size_t fallback_entries = 0;
  if (!fmp) {
    all_created = false;
  } else {
    void *f[6] = {0};
    for (size_t i = 0; i < 6; ++i) {
      f[i] = cmempool_alloc_entry(fmp);
      if (f[i]) {
        ++checked;
        if ((uintptr_t)f[i] % _Alignof(max_align_t) != 0) ++misaligned;
      }
    }
    fallback_entries = cmempool_dynamic_allocs_count(fmp);
    for (size_t i = 0; i < 6; ++i) {
      if (f[i]) cmempool_free_entry(fmp, f[i]);
    }
    cmempool_destroy(fmp);
  }

  crmempool *rmp =
      crmempool_create(4, 9, 5, crmempool_fallback_disabled, true, NULL);
  if (!rmp) {
    all_created = false;
  } else {
    for (size_t sz = 16; sz <= 512; sz *= 2) {
      void *e = crmempool_alloc_entry(rmp, sz);
      if (e) {
        ++checked;
        if ((uintptr_t)e % _Alignof(max_align_t) != 0) ++misaligned;
        crmempool_free_entry(rmp, e);
      }
    }
    crmempool_destroy(rmp);
  }

  static CRMEMPOOL_DECLARE_PREALLOCATED_BUFFER(prbuf, 4, 16, 12);
  SCOPED_R_MEMPOOL(prmp) = crmempool_create_from_preallocated_buffer(
      prbuf, sizeof(prbuf), 4, 16, 12, crmempool_fallback_disabled, true, NULL);
  size_t tiers_checked = 0;
  if (!prmp) {
    all_created = false;
  } else {
    for (size_t sz = 16; sz <= ((size_t)1 << 16); sz *= 2) {
      void *e = crmempool_alloc_entry(prmp, sz);
      if (e) {
        ++checked;
        ++tiers_checked;
        if ((uintptr_t)e % _Alignof(max_align_t) != 0) ++misaligned;
        crmempool_free_entry(prmp, e);
      }
    }
    crmempool_destroy(prmp);
  }

  REQUIRE_TRUE(all_created);
  REQUIRE_GT(checked, (size_t)10);
  REQUIRE_GT(fallback_entries, (size_t)0);
  REQUIRE_EQ(tiers_checked, (size_t)13);
  REQUIRE_EQ(misaligned, (size_t)0);
}

TEST(cmempools, create_succeeds) {
  char *err;
  cmempool *mp = cmempool_create(256, sizeof(int), false, false, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)mp, NULL);

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);

  mp = cmempool_create(256, sizeof(int), false, true, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)mp, NULL);

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);

  ccol_memmgmt_procs m_procs = {
      .malloc = malloc, .calloc = calloc, .realloc = realloc, .free = free};
  mp = cmempool_create_mp(256, sizeof(int), false, false, &m_procs, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)mp, NULL);

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);

  mp = cmempool_create_mp(256, sizeof(int), false, true, &m_procs, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)mp, NULL);

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, allocations_and_deallocations_fallback_disabled) {
  cmempool *mp = cmempool_create(256, sizeof(int), false, false, NULL);
  REQUIRE_NE((void *)mp, NULL);

  REQUIRE_EQ(cmempool_used_count(mp), 0);
  REQUIRE_EQ(cmempool_total_capacity(mp), 256);

  int *ptrs[256] = {0};

  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = cmempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(cmempool_used_count(mp), i + 1);
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  int *tmp_ptr = cmempool_alloc_entry(mp);
  REQUIRE_EQ((void *)tmp_ptr, NULL);

  for (size_t i = 0; i < 256; ++i) {
    cmempool_free_entry(mp, ptrs[i]);
    REQUIRE_EQ(cmempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = cmempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(cmempool_used_count(mp), i + 1);
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  tmp_ptr = cmempool_alloc_entry(mp);
  REQUIRE_EQ((void *)tmp_ptr, NULL);

  for (size_t i = 0; i < 256; ++i) {
    cmempool_free_entry(mp, ptrs[i]);
    REQUIRE_EQ(cmempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, allocations_and_deallocations_fallback_disabled_no_locks) {
  cmempool *mp = cmempool_create(256, sizeof(int), false, true, NULL);
  REQUIRE_NE((void *)mp, NULL);

  REQUIRE_EQ(cmempool_used_count(mp), 0);
  REQUIRE_EQ(cmempool_total_capacity(mp), 256);

  int *ptrs[256] = {0};

  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = cmempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(cmempool_used_count(mp), i + 1);
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  int *tmp_ptr = cmempool_alloc_entry(mp);
  REQUIRE_EQ((void *)tmp_ptr, NULL);

  for (size_t i = 0; i < 256; ++i) {
    cmempool_free_entry(mp, ptrs[i]);
    REQUIRE_EQ(cmempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = cmempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(cmempool_used_count(mp), i + 1);
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  tmp_ptr = cmempool_alloc_entry(mp);
  REQUIRE_EQ((void *)tmp_ptr, NULL);

  for (size_t i = 0; i < 256; ++i) {
    cmempool_free_entry(mp, ptrs[i]);
    REQUIRE_EQ(cmempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, allocations_and_deallocations_fallback_enabled) {
  cmempool *mp = cmempool_create(256, sizeof(int), true, false, NULL);
  REQUIRE_NE((void *)mp, NULL);

  REQUIRE_EQ(cmempool_used_count(mp), 0);
  REQUIRE_EQ(cmempool_total_capacity(mp), 256);

  int *ptrs[256] = {0};

  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = cmempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(cmempool_used_count(mp), i + 1);
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  int *tmp_ptr = cmempool_alloc_entry(mp);
  REQUIRE_NE((void *)tmp_ptr, NULL);
  cmempool_free_entry(mp, tmp_ptr);

  for (size_t i = 0; i < 256; ++i) {
    cmempool_free_entry(mp, ptrs[i]);
    REQUIRE_EQ(cmempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = cmempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(cmempool_used_count(mp), i + 1);
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  tmp_ptr = cmempool_alloc_entry(mp);
  REQUIRE_NE((void *)tmp_ptr, NULL);
  cmempool_free_entry(mp, tmp_ptr);

  for (size_t i = 0; i < 256; ++i) {
    cmempool_free_entry(mp, ptrs[i]);
    REQUIRE_EQ(cmempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools,
     allocations_and_deallocations_fallback_enabled_custom_mem_procs) {
  ccol_memmgmt_procs m_procs = {
      .malloc = malloc, .calloc = calloc, .realloc = realloc, .free = free};
  char *err;

  cmempool *mp =
      cmempool_create_mp(256, sizeof(int), true, false, &m_procs, &err);
  REQUIRE_NE((void *)mp, NULL);

  REQUIRE_EQ(cmempool_used_count(mp), 0);
  REQUIRE_EQ(cmempool_total_capacity(mp), 256);

  int *ptrs[256] = {0};

  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = cmempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(cmempool_used_count(mp), i + 1);
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  int *tmp_ptr = cmempool_alloc_entry(mp);
  REQUIRE_NE((void *)tmp_ptr, NULL);
  cmempool_free_entry(mp, tmp_ptr);

  for (size_t i = 0; i < 256; ++i) {
    cmempool_free_entry(mp, ptrs[i]);
    REQUIRE_EQ(cmempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = cmempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(cmempool_used_count(mp), i + 1);
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  tmp_ptr = cmempool_alloc_entry(mp);
  REQUIRE_NE((void *)tmp_ptr, NULL);
  cmempool_free_entry(mp, tmp_ptr);

  for (size_t i = 0; i < 256; ++i) {
    cmempool_free_entry(mp, ptrs[i]);
    REQUIRE_EQ(cmempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, allocations_and_deallocations_fallback_enabled_no_locks) {
  cmempool *mp = cmempool_create(256, sizeof(int), true, true, NULL);
  REQUIRE_NE((void *)mp, NULL);

  REQUIRE_EQ(cmempool_used_count(mp), 0);
  REQUIRE_EQ(cmempool_total_capacity(mp), 256);

  int *ptrs[256] = {0};

  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = cmempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(cmempool_used_count(mp), i + 1);
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  int *tmp_ptr = cmempool_alloc_entry(mp);
  REQUIRE_NE((void *)tmp_ptr, NULL);
  cmempool_free_entry(mp, tmp_ptr);

  for (size_t i = 0; i < 256; ++i) {
    cmempool_free_entry(mp, ptrs[i]);
    REQUIRE_EQ(cmempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = cmempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(cmempool_used_count(mp), i + 1);
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  tmp_ptr = cmempool_alloc_entry(mp);
  REQUIRE_NE((void *)tmp_ptr, NULL);
  cmempool_free_entry(mp, tmp_ptr);

  for (size_t i = 0; i < 256; ++i) {
    cmempool_free_entry(mp, ptrs[i]);
    REQUIRE_EQ(cmempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, c_allocations_and_deallocations_fallback_disabled) {
  cmempool *mp = cmempool_create(256, sizeof(int), false, false, NULL);
  REQUIRE_NE((void *)mp, NULL);

  REQUIRE_EQ(cmempool_used_count(mp), 0);
  REQUIRE_EQ(cmempool_total_capacity(mp), 256);

  int *ptrs[256] = {0};

  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = cmempool_calloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(cmempool_used_count(mp), i + 1);
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  int *tmp_ptr = cmempool_calloc_entry(mp);
  REQUIRE_EQ((void *)tmp_ptr, NULL);

  for (size_t i = 0; i < 256; ++i) {
    cmempool_free_entry(mp, ptrs[i]);
    REQUIRE_EQ(cmempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = cmempool_calloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(cmempool_used_count(mp), i + 1);
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  tmp_ptr = cmempool_calloc_entry(mp);
  REQUIRE_EQ((void *)tmp_ptr, NULL);

  for (size_t i = 0; i < 256; ++i) {
    cmempool_free_entry(mp, ptrs[i]);
    REQUIRE_EQ(cmempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, c_allocations_and_deallocations_fallback_enabled) {
  cmempool *mp = cmempool_create(256, sizeof(int), true, false, NULL);
  REQUIRE_NE((void *)mp, NULL);

  REQUIRE_EQ(cmempool_used_count(mp), 0);
  REQUIRE_EQ(cmempool_total_capacity(mp), 256);

  int *ptrs[256] = {0};

  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = cmempool_calloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(cmempool_used_count(mp), i + 1);
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  int *tmp_ptr = cmempool_calloc_entry(mp);
  REQUIRE_NE((void *)tmp_ptr, NULL);
  cmempool_free_entry(mp, tmp_ptr);

  for (size_t i = 0; i < 256; ++i) {
    cmempool_free_entry(mp, ptrs[i]);
    REQUIRE_EQ(cmempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = cmempool_calloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(cmempool_used_count(mp), i + 1);
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  tmp_ptr = cmempool_calloc_entry(mp);
  REQUIRE_NE((void *)tmp_ptr, NULL);
  cmempool_free_entry(mp, tmp_ptr);

  for (size_t i = 0; i < 256; ++i) {
    cmempool_free_entry(mp, ptrs[i]);
    REQUIRE_EQ(cmempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(cmempool_total_capacity(mp), 256);
  }

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, calloc_entry_zeroes_memory) {
  const size_t elem_size = sizeof(long long);
  cmempool *mp = cmempool_create(4, elem_size, false, false, NULL);
  REQUIRE_NE((void *)mp, NULL);

  long long *slot = cmempool_alloc_entry(mp);
  REQUIRE_NE((void *)slot, NULL);
  memset(slot, 0xFF, elem_size);
  cmempool_free_entry(mp, slot);

  slot = cmempool_calloc_entry(mp);
  REQUIRE_NE((void *)slot, NULL);

  const char zeroes[sizeof(long long)] = {0};
  REQUIRE_EQ(memcmp(slot, zeroes, elem_size), 0);

  cmempool_free_entry(mp, slot);
  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, free_null_is_noop) {
  cmempool *mp = cmempool_create(4, sizeof(int), false, true, NULL);
  REQUIRE_NE((void *)mp, NULL);

  void *p = NULL;
  cmempool_free_entry(mp, p);
  bool p_still_null = (p == NULL);

  cmempool_destroy(mp);

  REQUIRE_TRUE(p_still_null);
}

TEST(cmempools, free_with_null_pool_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    cmempool *mp = cmempool_create(4, sizeof(int), false, true, NULL);
    void *e = mp ? cmempool_alloc_entry(mp) : NULL;
    cmempool_free_entry((cmempool *)NULL, e);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// Calls cmempool__free_entry() directly, because the macro clears its
// argument and a second macro call would be a no-op.
TEST(cmempools, double_free_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    cmempool *mp = cmempool_create(4, sizeof(int), false, true, NULL);
    void *p = cmempool_alloc_entry(mp);
    void *p_saved = p;
    cmempool_free_entry(mp, p);
    cmempool__free_entry(mp, p_saved);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

TEST(cmempools, free_entry_with_corrupted_status_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    cmempool *mp = cmempool_create(4, sizeof(int), false, true, NULL);
    void *p = cmempool_alloc_entry(mp);
    cmempool__corrupt_entry_status_for_tests(mp, p, 0xAB);
    cmempool__free_entry(mp, p);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// An allocator whose free leaves the block untouched (and never releases it),
// so a double free of a dynamic entry isn't hidden by glibc poisoning the
// freed block. The double-free test with a second live dynamic entry needs it:
// a count-only check passes there, so the pool must forget the address on the
// first free.
static void *no_clobber_malloc(size_t s) { return malloc(s); }
static void *no_clobber_calloc(size_t n, size_t s) { return calloc(n, s); }
static void *no_clobber_realloc(void *p, size_t s) { return realloc(p, s); }
static void no_clobber_free(void *p) { (void)p; }

TEST(cmempools, double_free_of_dynamic_entry_with_another_still_live_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    ccol_memmgmt_procs procs = {.malloc = no_clobber_malloc,
                                .calloc = no_clobber_calloc,
                                .realloc = no_clobber_realloc,
                                .free = no_clobber_free};
    cmempool *mp = cmempool_create_mp(2, sizeof(int), true, true, &procs, NULL);
    cmempool_alloc_entry(mp);
    cmempool_alloc_entry(mp);
    void *d1 = cmempool_alloc_entry(mp);
    void *d2 = cmempool_alloc_entry(mp);
    (void)d2;
    void *d1_saved = d1;
    cmempool_free_entry(mp, d1);
    cmempool__free_entry(mp, d1_saved);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// The guard allocator: Every block starts right after a page the process can't
// read, so a read in front of a pool's first entry dies with SIGSEGV in any
// build. Only single-threaded forked children use it, so it takes no lock.
// MP_GUARD_MAX_BLOCKS is how many blocks it tracks.
#define MP_GUARD_MAX_BLOCKS 32
// One tracked block of the guard allocator: User address, mapping, size.
static struct {
  void *user;
  void *base;
  size_t len;
  size_t size;
} mp_guard_blocks[MP_GUARD_MAX_BLOCKS];

// Guard allocator functions: malloc maps a guard page plus the block, calloc
// relies on mmap memory being zero, free unmaps a tracked block, realloc
// copies into a new block.
static void *mp_guard_malloc(size_t n) {
  size_t pg = (size_t)sysconf(_SC_PAGESIZE);
  size_t body = ((n ? n : 1) + pg - 1) / pg * pg;
  for (size_t i = 0; i < MP_GUARD_MAX_BLOCKS; ++i) {
    if (mp_guard_blocks[i].user) continue;
    void *base = mmap(NULL, pg + body, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return NULL;
    if (mprotect(base, pg, PROT_NONE) != 0) {
      munmap(base, pg + body);
      return NULL;
    }
    mp_guard_blocks[i].base = base;
    mp_guard_blocks[i].len = pg + body;
    mp_guard_blocks[i].size = n;
    mp_guard_blocks[i].user = (char *)base + pg;
    return mp_guard_blocks[i].user;
  }
  return NULL;
}

static void *mp_guard_calloc(size_t count, size_t size) {
  if (size && count > SIZE_MAX / size) return NULL;
  return mp_guard_malloc(count * size);
}

static void mp_guard_free(void *p) {
  for (size_t i = 0; p && i < MP_GUARD_MAX_BLOCKS; ++i) {
    if (mp_guard_blocks[i].user == p) {
      munmap(mp_guard_blocks[i].base, mp_guard_blocks[i].len);
      mp_guard_blocks[i].user = NULL;
      return;
    }
  }
}

static void *mp_guard_realloc(void *p, size_t n) {
  void *q = mp_guard_malloc(n);
  if (!q || !p) return q;
  for (size_t i = 0; i < MP_GUARD_MAX_BLOCKS; ++i) {
    if (mp_guard_blocks[i].user == p) {
      memcpy(q, p, mp_guard_blocks[i].size < n ? mp_guard_blocks[i].size : n);
      break;
    }
  }
  mp_guard_free(p);
  return q;
}

// Builds a small single-threaded pool on the guard allocator, stores it in
// *out_pool and gives its lowest entry, the first byte of its buffer, so a
// foreign-pointer test has an address with nothing readable in front of it.
static void *mp_guard_first_entry(cmempool **out_pool) {
  static ccol_memmgmt_procs procs = {.malloc = mp_guard_malloc,
                                     .calloc = mp_guard_calloc,
                                     .realloc = mp_guard_realloc,
                                     .free = mp_guard_free};
  enum { N = 4 };
  cmempool *other =
      cmempool_create_mp(N, sizeof(int), false, true, &procs, NULL);
  if (!other) return NULL;
  void *lowest = NULL;
  for (int i = 0; i < N; ++i) {
    void *e = cmempool_alloc_entry(other);
    if (e && (!lowest || (uintptr_t)e < (uintptr_t)lowest)) lowest = e;
  }
  *out_pool = other;
  return lowest;
}

// Sends a forked child's stdout and stderr to /dev/null, so an expected abort
// doesn't clutter the test output.
static void mp_silence_child_output(void) {
  int dn = open("/dev/null", O_WRONLY);
  if (dn >= 0) {
    dup2(dn, STDOUT_FILENO);
    dup2(dn, STDERR_FILENO);
    close(dn);
  }
}

// The foreign entry has an unreadable page in front of it, so reading any
// header there gives SIGSEGV instead of the assert's SIGABRT; the pool holds
// a live dynamic entry, so the registry has to answer.
TEST(cmempools, free_of_another_pools_entry_asserts_without_reading_it) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    mp_silence_child_output();
    cmempool *other = NULL;
    void *foreign = mp_guard_first_entry(&other);
    cmempool *mp = cmempool_create(2, sizeof(int), true, false, NULL);
    if (!foreign || !mp) test_child_exit(2);
    cmempool_alloc_entry(mp);
    cmempool_alloc_entry(mp);
    void *dyn = cmempool_alloc_entry(mp);
    if (!dyn || cmempool_dynamic_allocs_count(mp) != 1) test_child_exit(3);
    cmempool__free_entry(mp, foreign);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// Forked-child body of the ranged foreign-pointer test: Holds a live dynamic
// entry under policy, then frees (or reallocs, with through_realloc) an entry
// of another pool, which must abort without reading through it.
static void mp_ranged_foreign_child(crmempool_fallback_policy policy,
                                    bool through_realloc) {
  test_child_no_core();
  mp_silence_child_output();
  cmempool *other = NULL;
  void *foreign = mp_guard_first_entry(&other);
  crmempool *rmp = crmempool_create(4, 5, 1, policy, false, NULL);
  if (!foreign || !rmp) test_child_exit(2);
  void *e = NULL;
  for (int i = 0; i < 4; ++i) e = crmempool_alloc_entry(rmp, 16);
  if (!e) test_child_exit(3);
  if (through_realloc) {
    crmempool_realloc_entry(rmp, foreign, 16);
  } else {
    crmempool__free_entry(rmp, foreign);
  }
  test_child_exit(0);
}

TEST(r_mempools, free_of_another_pools_entry_asserts_without_reading_it) {
  crmempool_fallback_policy policies[] = {
      crmempool_fallback_at_first_exhaustion,
      crmempool_fallback_at_last_exhaustion};
  int statuses[4] = {0};
  pid_t pids[4] = {-1, -1, -1, -1};
  for (int i = 0; i < 4; ++i) {
    pids[i] = fork();
    if (pids[i] == 0) mp_ranged_foreign_child(policies[i / 2], i % 2 == 1);
    if (pids[i] > 0) waitpid(pids[i], &statuses[i], 0);
  }
  for (int i = 0; i < 4; ++i) {
    REQUIRE_GT(pids[i], 0);
    REQUIRE_TRUE(WIFSIGNALED(statuses[i]));
    REQUIRE_EQ(WTERMSIG(statuses[i]), SIGABRT);
  }
}

// Frees many dynamic entries in an order unrelated to the allocation order, so
// every growth of the registry and every shape of removal runs.
TEST(cmempools, many_dynamic_entries_free_in_any_order) {
  enum { N = 2000 };
  cmempool *mp = cmempool_create(2, sizeof(int), true, false, NULL);
  REQUIRE_NE((void *)mp, NULL);
  void *owned[2] = {cmempool_alloc_entry(mp), cmempool_alloc_entry(mp)};
  void **dyn = calloc(N, sizeof(void *));
  size_t made = 0;
  if (dyn) {
    for (; made < N; ++made) {
      dyn[made] = cmempool_alloc_entry(mp);
      if (!dyn[made]) break;
    }
  }
  size_t count_full = cmempool_dynamic_allocs_count(mp);
  size_t count_half = 0;
  for (size_t k = 0; k < made; ++k) {
    size_t i = (k * 7919u) % made;
    cmempool_free_entry(mp, dyn[i]);
    if (k + 1 == made / 2) count_half = cmempool_dynamic_allocs_count(mp);
  }
  size_t count_empty = cmempool_dynamic_allocs_count(mp);
  void *again = cmempool_alloc_entry(mp);
  bool again_ok = (again != NULL);
  size_t count_again = cmempool_dynamic_allocs_count(mp);
  cmempool_free_entry(mp, again);
  cmempool_free_entry(mp, owned[0]);
  cmempool_free_entry(mp, owned[1]);
  free(dyn);
  cmempool_destroy(mp);

  REQUIRE_EQ(made, (size_t)N);
  REQUIRE_EQ(count_full, (size_t)N);
  REQUIRE_EQ(count_half, (size_t)(N - N / 2));
  REQUIRE_EQ(count_empty, (size_t)0);
  REQUIRE_TRUE(again_ok);
  REQUIRE_EQ(count_again, (size_t)1);
}

// Every other test frees its dynamic entries, so only this one notices a
// weakened leak assert in the destroy.
TEST(cmempools, destroy_with_leaked_dynamic_entry_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    cmempool *mp = cmempool_create(2, sizeof(int), true, true, NULL);
    cmempool_alloc_entry(mp);
    cmempool_alloc_entry(mp);
    void *leaked = cmempool_alloc_entry(mp);
    (void)leaked;
    cmempool_destroy(mp);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

TEST(cmempools, dynamic_allocs_count_zero_without_fallback) {
  cmempool *mp = cmempool_create(4, sizeof(int), false, false, NULL);
  REQUIRE_NE((void *)mp, NULL);
  REQUIRE_EQ(cmempool_dynamic_allocs_count(mp), 0);

  int *ptrs[4];
  for (size_t i = 0; i < 4; ++i) {
    ptrs[i] = cmempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    REQUIRE_EQ(cmempool_dynamic_allocs_count(mp), 0);
  }

  REQUIRE_EQ((void *)cmempool_alloc_entry(mp), NULL);
  REQUIRE_EQ(cmempool_dynamic_allocs_count(mp), 0);

  for (size_t i = 0; i < 4; ++i) {
    cmempool_free_entry(mp, ptrs[i]);
  }
  REQUIRE_EQ(cmempool_dynamic_allocs_count(mp), 0);

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, dynamic_allocs_count_tracks_correctly_with_fallback) {
  cmempool *mp = cmempool_create(4, sizeof(int), true, false, NULL);
  REQUIRE_NE((void *)mp, NULL);

  int *ptrs[4];
  for (size_t i = 0; i < 4; ++i) {
    ptrs[i] = cmempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    REQUIRE_EQ(cmempool_dynamic_allocs_count(mp), 0);
  }

  int *fallback1 = cmempool_alloc_entry(mp);
  REQUIRE_NE((void *)fallback1, NULL);
  REQUIRE_EQ(cmempool_dynamic_allocs_count(mp), 1);

  int *fallback2 = cmempool_alloc_entry(mp);
  REQUIRE_NE((void *)fallback2, NULL);
  REQUIRE_EQ(cmempool_dynamic_allocs_count(mp), 2);

  cmempool_free_entry(mp, fallback1);
  REQUIRE_EQ(cmempool_dynamic_allocs_count(mp), 1);

  cmempool_free_entry(mp, fallback2);
  REQUIRE_EQ(cmempool_dynamic_allocs_count(mp), 0);

  for (size_t i = 0; i < 4; ++i) {
    cmempool_free_entry(mp, ptrs[i]);
  }
  REQUIRE_EQ(cmempool_dynamic_allocs_count(mp), 0);
  REQUIRE_EQ(cmempool_used_count(mp), 0);

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, c_allocations_and_deallocations_fallback_enabled_no_locks) {
  cmempool *mp = cmempool_create(4, sizeof(int), true, true, NULL);
  REQUIRE_NE((void *)mp, NULL);

  REQUIRE_EQ(cmempool_used_count(mp), 0);
  REQUIRE_EQ(cmempool_total_capacity(mp), 4);

  int *ptrs[4];
  for (size_t i = 0; i < 4; ++i) {
    ptrs[i] = cmempool_calloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    REQUIRE_EQ(*ptrs[i], 0);
    *ptrs[i] = (int)i;
    REQUIRE_EQ(cmempool_used_count(mp), i + 1);
  }

  int *fallback = cmempool_calloc_entry(mp);
  REQUIRE_NE((void *)fallback, NULL);
  REQUIRE_EQ(*fallback, 0);
  REQUIRE_EQ(cmempool_dynamic_allocs_count(mp), 1);
  cmempool_free_entry(mp, fallback);
  REQUIRE_EQ(cmempool_dynamic_allocs_count(mp), 0);

  for (size_t i = 0; i < 4; ++i) {
    cmempool_free_entry(mp, ptrs[i]);
    REQUIRE_EQ(cmempool_used_count(mp), 4 - (i + 1));
  }

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, create_from_preallocated_fails_null_buffer) {
  char *err;
  SCOPED_MEMPOOL(mp) = cmempool_create_from_preallocated_buffer(
      NULL, 256, 16, false, false, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(cmempools, create_from_preallocated_small_elem_size_is_bumped_to_min) {
  _Alignas(cmempool__entry_align) uint8_t buf[256];
  char *err = NULL;
  SCOPED_MEMPOOL(mp) = cmempool_create_from_preallocated_buffer(
      buf, sizeof(buf), sizeof(uintptr_t) - 1, false, false, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)mp, NULL);

  void *p = cmempool_alloc_entry(mp);
  REQUIRE_NE(p, NULL);
  memset(p, 0xAB, sizeof(uintptr_t));
  cmempool_free_entry(mp, p);

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, create_from_preallocated_fails_elem_size_zero) {
  _Alignas(cmempool__entry_align) uint8_t buf[256];
  char *err = NULL;
  SCOPED_MEMPOOL(mp) = cmempool_create_from_preallocated_buffer(
      buf, sizeof(buf), 0, false, false, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(cmempools, create_from_preallocated_fails_elem_count_zero) {
  _Alignas(cmempool__entry_align) uint8_t buf[64];
  char *err;
  SCOPED_MEMPOOL(mp) = cmempool_create_from_preallocated_buffer(
      buf, sizeof(buf), 64, false, false, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(cmempools, create_from_preallocated_fails_misaligned_buffer) {
  _Alignas(cmempool__entry_align) uint8_t buf[257];
  char *err = NULL;
  SCOPED_MEMPOOL(mp) = cmempool_create_from_preallocated_buffer(
      buf + 1, sizeof(buf) - 1, 16, false, false, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// Buffers declared for elem_size 1, so the macro must round up to
// sizeof(uintptr_t) the way the constructor does, or the pool gets fewer
// elements than declared (or none at all for the single-element buffer).
CMEMPOOL_DECLARE_PREALLOCATED_BUFFER(preallocated_mp_small_elem_buffer, 100, 1);

TEST(cmempools,
     declare_preallocated_buffer_small_elem_size_yields_exact_elem_count) {
  SCOPED_MEMPOOL(mp) = cmempool_create_from_preallocated_buffer(
      preallocated_mp_small_elem_buffer,
      sizeof(preallocated_mp_small_elem_buffer), 1, false, false, NULL);
  REQUIRE_NE((void *)mp, NULL);

  REQUIRE_EQ(cmempool_total_capacity(mp), 100);

  void *ptrs[100];
  for (size_t i = 0; i < 100; ++i) {
    ptrs[i] = cmempool_alloc_entry(mp);
    REQUIRE_NE(ptrs[i], NULL);
  }
  REQUIRE_EQ((void *)cmempool_alloc_entry(mp), NULL);

  for (size_t i = 0; i < 100; ++i) {
    cmempool_free_entry(mp, ptrs[i]);
  }

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

CMEMPOOL_DECLARE_PREALLOCATED_BUFFER(preallocated_mp_single_small_elem_buffer,
                                     1, 1);

TEST(cmempools, declare_preallocated_buffer_single_small_elem_size_succeeds) {
  SCOPED_MEMPOOL(mp) = cmempool_create_from_preallocated_buffer(
      preallocated_mp_single_small_elem_buffer,
      sizeof(preallocated_mp_single_small_elem_buffer), 1, false, false, NULL);
  REQUIRE_NE((void *)mp, NULL);
  REQUIRE_EQ(cmempool_total_capacity(mp), 1);

  void *p = cmempool_alloc_entry(mp);
  REQUIRE_NE(p, NULL);
  REQUIRE_EQ((void *)cmempool_alloc_entry(mp), NULL);
  cmempool_free_entry(mp, p);

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

// A buffer for elem_size 9, which is neither a small size nor aligned, so only
// the macro's stride rounding keeps it in step with the constructor.
CMEMPOOL_DECLARE_PREALLOCATED_BUFFER(preallocated_mp_odd_elem_buffer, 50, 9);

TEST(
    cmempools,
    declare_preallocated_buffer_odd_elem_size_yields_exact_elem_count_and_alignment) {
  SCOPED_MEMPOOL(mp) = cmempool_create_from_preallocated_buffer(
      preallocated_mp_odd_elem_buffer, sizeof(preallocated_mp_odd_elem_buffer),
      9, false, false, NULL);
  REQUIRE_NE((void *)mp, NULL);

  REQUIRE_EQ(cmempool_total_capacity(mp), 50);

  void *ptrs[50];
  for (size_t i = 0; i < 50; ++i) {
    ptrs[i] = cmempool_alloc_entry(mp);
    REQUIRE_NE(ptrs[i], NULL);
    REQUIRE_EQ((uintptr_t)ptrs[i] % _Alignof(max_align_t), 0);
  }
  REQUIRE_EQ((void *)cmempool_alloc_entry(mp), NULL);

  for (size_t i = 0; i < 50; ++i) {
    cmempool_free_entry(mp, ptrs[i]);
  }

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

// The buffer and the entry array of the preallocated pool tests: 32768
// entries of 256 bytes.
CMEMPOOL_DECLARE_PREALLOCATED_BUFFER(preallocated_mp_buffer, 32768, 256);
char *preallocated_ptrs[32768] = {0};

TEST(cmempools, preallocated_buffer_without_fallback) {
  SCOPED_MEMPOOL(mp) = cmempool_create_from_preallocated_buffer(
      preallocated_mp_buffer, sizeof(preallocated_mp_buffer), 256, false, false,
      NULL);
  REQUIRE_NE((void *)mp, NULL);

  size_t capacity = cmempool_total_capacity(mp);
  REQUIRE_EQ(capacity, 32768);

  for (size_t i = 0; i < capacity; ++i) {
    preallocated_ptrs[i] = cmempool_alloc_entry(mp);
    REQUIRE_NE((void *)preallocated_ptrs[i], NULL);
  }

  REQUIRE_EQ(cmempool_alloc_entry(mp), NULL);

  for (size_t i = 0; i < capacity; ++i) {
    cmempool_free_entry(mp, preallocated_ptrs[i]);
    REQUIRE_EQ((void *)preallocated_ptrs[i], NULL);
  }

  for (size_t i = 0; i < capacity; ++i) {
    preallocated_ptrs[i] = cmempool_calloc_entry(mp);
    REQUIRE_NE((void *)preallocated_ptrs[i], NULL);
  }

  REQUIRE_EQ(cmempool_calloc_entry(mp), NULL);

  for (size_t i = 0; i < capacity; ++i) {
    cmempool_free_entry(mp, preallocated_ptrs[i]);
    REQUIRE_EQ((void *)preallocated_ptrs[i], NULL);
  }

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, preallocated_buffer_without_fallback_no_locks) {
  SCOPED_MEMPOOL(mp) = cmempool_create_from_preallocated_buffer(
      preallocated_mp_buffer, sizeof(preallocated_mp_buffer), 256, false, true,
      NULL);
  REQUIRE_NE((void *)mp, NULL);

  size_t capacity = cmempool_total_capacity(mp);
  REQUIRE_EQ(capacity, 32768);

  for (size_t i = 0; i < capacity; ++i) {
    preallocated_ptrs[i] = cmempool_alloc_entry(mp);
    REQUIRE_NE((void *)preallocated_ptrs[i], NULL);
  }

  REQUIRE_EQ(cmempool_alloc_entry(mp), NULL);

  for (size_t i = 0; i < capacity; ++i) {
    cmempool_free_entry(mp, preallocated_ptrs[i]);
    REQUIRE_EQ((void *)preallocated_ptrs[i], NULL);
  }

  for (size_t i = 0; i < capacity; ++i) {
    preallocated_ptrs[i] = cmempool_calloc_entry(mp);
    REQUIRE_NE((void *)preallocated_ptrs[i], NULL);
  }

  REQUIRE_EQ(cmempool_calloc_entry(mp), NULL);

  for (size_t i = 0; i < capacity; ++i) {
    cmempool_free_entry(mp, preallocated_ptrs[i]);
    REQUIRE_EQ((void *)preallocated_ptrs[i], NULL);
  }

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, preallocated_buffer_with_fallback) {
  SCOPED_MEMPOOL(mp) = cmempool_create_from_preallocated_buffer(
      preallocated_mp_buffer, sizeof(preallocated_mp_buffer), 256, true, false,
      NULL);
  REQUIRE_NE((void *)mp, NULL);

  size_t capacity = cmempool_total_capacity(mp);
  REQUIRE_EQ(capacity, 32768);

  for (size_t i = 0; i < capacity; ++i) {
    preallocated_ptrs[i] = cmempool_alloc_entry(mp);
    REQUIRE_NE((void *)preallocated_ptrs[i], NULL);
  }

  void *tmp = cmempool_alloc_entry(mp);
  REQUIRE_NE(tmp, NULL);
  cmempool_free_entry(mp, tmp);

  for (size_t i = 0; i < capacity; ++i) {
    cmempool_free_entry(mp, preallocated_ptrs[i]);
    REQUIRE_EQ((void *)preallocated_ptrs[i], NULL);
  }

  for (size_t i = 0; i < capacity; ++i) {
    preallocated_ptrs[i] = cmempool_calloc_entry(mp);
    REQUIRE_NE((void *)preallocated_ptrs[i], NULL);
  }

  tmp = cmempool_calloc_entry(mp);
  REQUIRE_NE(tmp, NULL);
  cmempool_free_entry(mp, tmp);

  for (size_t i = 0; i < capacity; ++i) {
    cmempool_free_entry(mp, preallocated_ptrs[i]);
    REQUIRE_EQ((void *)preallocated_ptrs[i], NULL);
  }

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, preallocated_buffer_with_fallback_no_locks) {
  SCOPED_MEMPOOL(mp) = cmempool_create_from_preallocated_buffer(
      preallocated_mp_buffer, sizeof(preallocated_mp_buffer), 256, true, true,
      NULL);
  REQUIRE_NE((void *)mp, NULL);

  size_t capacity = cmempool_total_capacity(mp);
  REQUIRE_EQ(capacity, 32768);

  for (size_t i = 0; i < capacity; ++i) {
    preallocated_ptrs[i] = cmempool_alloc_entry(mp);
    REQUIRE_NE((void *)preallocated_ptrs[i], NULL);
  }

  void *tmp = cmempool_alloc_entry(mp);
  REQUIRE_NE(tmp, NULL);
  cmempool_free_entry(mp, tmp);

  for (size_t i = 0; i < capacity; ++i) {
    cmempool_free_entry(mp, preallocated_ptrs[i]);
    REQUIRE_EQ((void *)preallocated_ptrs[i], NULL);
  }

  for (size_t i = 0; i < capacity; ++i) {
    preallocated_ptrs[i] = cmempool_calloc_entry(mp);
    REQUIRE_NE((void *)preallocated_ptrs[i], NULL);
  }

  tmp = cmempool_calloc_entry(mp);
  REQUIRE_NE(tmp, NULL);
  cmempool_free_entry(mp, tmp);

  for (size_t i = 0; i < capacity; ++i) {
    cmempool_free_entry(mp, preallocated_ptrs[i]);
    REQUIRE_EQ((void *)preallocated_ptrs[i], NULL);
  }

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(r_mempools, create_fails) {
  char *err;

  SCOPED_R_MEMPOOL(rmp) =
      crmempool_create(4, 17, 0, crmempool_fallback_disabled, false, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  rmp = crmempool_create(4, 0, 17, crmempool_fallback_disabled, false, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  rmp = crmempool_create(0, 17, 17, crmempool_fallback_disabled, false, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  rmp = crmempool_create(4, 3, 17, crmempool_fallback_disabled, false, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  rmp = crmempool_create(4, 4, 17, crmempool_fallback_disabled, false, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  rmp = crmempool_create(4, 7, 2, crmempool_fallback_disabled, false, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  rmp = crmempool_create(4, 6, 17, -1, false, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  rmp = crmempool_create(4, 6, 17, crmempool__fallback_end_place_holder, false,
                         &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  rmp = crmempool_create(4, 6, 17, crmempool__fallback_end_place_holder + 1,
                         false, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// Valid sizes with incomplete procs, for both constructors, so only the procs
// check can refuse.
TEST(r_mempools, create_rejects_incomplete_procs_with_valid_size_params) {
  ccol_memmgmt_procs m_procs = {
      .malloc = malloc, .calloc = NULL, .realloc = realloc, .free = free};
  char *err = NULL;
  SCOPED_R_MEMPOOL(rmp) = crmempool_create_mp(
      4, 6, 7, crmempool_fallback_disabled, false, &m_procs, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(r_mempools,
     create_from_preallocated_rejects_incomplete_procs_with_valid_params) {
  static CRMEMPOOL_DECLARE_PREALLOCATED_BUFFER(buf, 4, 6, 7);
  ccol_memmgmt_procs m_procs = {
      .malloc = malloc, .calloc = calloc, .realloc = NULL, .free = free};
  char *err = NULL;
  SCOPED_R_MEMPOOL(rmp) = crmempool_create_from_preallocated_buffer_mp(
      buf, sizeof(buf), 4, 6, 7, crmempool_fallback_disabled, true, &m_procs,
      &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(r_mempools, create_succeeds_with_minimum_sc) {
  char *err = NULL;
  crmempool *rmp =
      crmempool_create(4, 6, 2, crmempool_fallback_disabled, false, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  REQUIRE_EQ(crmempool_total_capacity(rmp, 16), 4);
  REQUIRE_EQ(crmempool_total_capacity(rmp, 32), 2);
  REQUIRE_EQ(crmempool_total_capacity(rmp, 64), 1);

  void *p16 = crmempool_alloc_entry(rmp, 16);
  REQUIRE_NE(p16, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 1);

  void *p32 = crmempool_alloc_entry(rmp, 32);
  REQUIRE_NE(p32, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 1);

  void *p64 = crmempool_alloc_entry(rmp, 64);
  REQUIRE_NE(p64, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 64), 1);

  REQUIRE_EQ((void *)crmempool_alloc_entry(rmp, 64), NULL);

  crmempool_free_entry(rmp, p16);
  crmempool_free_entry(rmp, p32);
  crmempool_free_entry(rmp, p64);

  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 64), 0);

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, create_succeeds) {
  char *err;
  SCOPED_R_MEMPOOL(rmp) =
      crmempool_create(4, 17, 17, crmempool_fallback_disabled, false, &err);
  REQUIRE_NE((void *)rmp, NULL);

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);

  rmp = crmempool_create(4, 17, 17, crmempool_fallback_disabled, true, &err);
  REQUIRE_NE((void *)rmp, NULL);

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, simple_allocations) {
  crmempool *rmp =
      crmempool_create(4, 6, 7, crmempool_fallback_disabled, false, NULL);
  char *ptr = NULL;

  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);
  ptr = crmempool_alloc_entry(rmp, 1);
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 1);
  crmempool_free_entry(rmp, ptr);

  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);
  ptr = crmempool_alloc_entry(rmp, 16);
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 1);
  crmempool_free_entry(rmp, ptr);

  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  ptr = crmempool_alloc_entry(rmp, 17);
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 1);
  crmempool_free_entry(rmp, ptr);

  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  ptr = crmempool_alloc_entry(rmp, 32);
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 1);
  crmempool_free_entry(rmp, ptr);

  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 64), 0);
  ptr = crmempool_alloc_entry(rmp, 33);
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 64), 1);
  crmempool_free_entry(rmp, ptr);

  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 64), 0);
  ptr = crmempool_alloc_entry(rmp, 63);
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 64), 1);
  crmempool_free_entry(rmp, ptr);

  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 64), 0);
  ptr = crmempool_alloc_entry(rmp, 64);
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 64), 1);
  crmempool_free_entry(rmp, ptr);

  REQUIRE_EQ((void *)crmempool_alloc_entry(rmp, 0), NULL);
  REQUIRE_EQ((void *)crmempool_alloc_entry(rmp, 65), NULL);

  crmempool_destroy(rmp);
}

// Checks the tier formula at every size boundary of a 7-tier pool, through the
// capacity each tier reports: It must change exactly at each power of two,
// never a byte early or late.
TEST(r_mempools, pool_index_boundaries_exhaustive) {
  SCOPED_R_MEMPOOL(rmp) =
      crmempool_create(4, 10, 6, crmempool_fallback_disabled, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  const size_t tier_sizes[] = {16, 32, 64, 128, 256, 512, 1024};
  const size_t tier_caps[] = {64, 32, 16, 8, 4, 2, 1};
  const size_t num_tiers = sizeof(tier_sizes) / sizeof(tier_sizes[0]);

  for (size_t t = 0; t < num_tiers; ++t) {
    REQUIRE_EQ(crmempool_total_capacity(rmp, tier_sizes[t]), tier_caps[t]);
  }

  size_t prev_boundary = 0;
  for (size_t t = 0; t < num_tiers; ++t) {
    for (size_t size = prev_boundary + 1; size <= tier_sizes[t]; ++size) {
      REQUIRE_EQ(crmempool_total_capacity(rmp, size), tier_caps[t]);
    }
    prev_boundary = tier_sizes[t];
  }

  for (size_t t = 0; t < num_tiers; ++t) {
    size_t just_over_prev = (t == 0) ? 1 : tier_sizes[t - 1] + 1;
    void *p = crmempool_alloc_entry(rmp, just_over_prev);
    REQUIRE_NE(p, NULL);
    REQUIRE_EQ(crmempool_used_count(rmp, tier_sizes[t]), 1);
    crmempool_free_entry(rmp, p);
    REQUIRE_EQ(crmempool_used_count(rmp, tier_sizes[t]), 0);

    p = crmempool_alloc_entry(rmp, tier_sizes[t]);
    REQUIRE_NE(p, NULL);
    REQUIRE_EQ(crmempool_used_count(rmp, tier_sizes[t]), 1);
    crmempool_free_entry(rmp, p);
    REQUIRE_EQ(crmempool_used_count(rmp, tier_sizes[t]), 0);
  }

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

// An allocator that refuses every request above 4096 bytes. A
// reverse-size lookup table for the configuration below would be 32 KiB, while
// every real allocation of the constructor is far smaller, so the test proves
// the constructor builds no such table.
static void *no_huge_alloc_malloc(size_t size) {
  return size > 4096 ? NULL : malloc(size);
}
static void *no_huge_alloc_calloc(size_t count, size_t size) {
  return (count != 0 && size > 4096 / count) ? NULL : calloc(count, size);
}
static void *no_huge_alloc_realloc(void *ptr, size_t size) {
  return size > 4096 ? NULL : realloc(ptr, size);
}
static void no_huge_alloc_free(void *ptr) { free(ptr); }

TEST(preallocated_r_mempools, create_needs_no_exponential_heap_allocation) {
  static CRMEMPOOL_DECLARE_PREALLOCATED_BUFFER(buf, 4, 16, 12);

  ccol_memmgmt_procs m_procs = {.malloc = no_huge_alloc_malloc,
                                .calloc = no_huge_alloc_calloc,
                                .realloc = no_huge_alloc_realloc,
                                .free = no_huge_alloc_free};
  char *err = NULL;
  SCOPED_R_MEMPOOL(rmp) = crmempool_create_from_preallocated_buffer_mp(
      buf, sizeof(buf), 4, 16, 12, crmempool_fallback_disabled, false, &m_procs,
      &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *p = crmempool_alloc_entry(rmp, 16);
  REQUIRE_NE(p, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 1);
  crmempool_free_entry(rmp, p);

  p = crmempool_alloc_entry(rmp, 1 << 16);
  REQUIRE_NE(p, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 1 << 16), 1);
  crmempool_free_entry(rmp, p);

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

// The true buffer size of these parameters is above SIZE_MAX. An unchecked
// sum wraps and fails later with the generic "buffer sizes differ", so the
// test asserts the specific overflow message, which only the guard gives.
TEST(preallocated_r_mempools,
     create_rejects_configuration_whose_true_buffer_size_overflows) {
#if SIZE_MAX > 0xFFFFFFFFu
  _Alignas(cmempool__entry_align) uint8_t buf[256];
  char *err = NULL;
  SCOPED_R_MEMPOOL(rmp) = crmempool_create_from_preallocated_buffer(
      buf, sizeof(buf), 4, 63, 62, crmempool_fallback_disabled, true, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_NE(strstr(err, "overflow"), NULL);
#else
  fprintf(stderr,
          "[SKIP] create_rejects_configuration_whose_true_buffer_size_"
          "overflows: size_t is only 32 bits on this platform, so "
          "largest_size_power_of_two=63 is rejected by an earlier, "
          "unrelated width guard before this test's own overflow scenario "
          "is ever reached\n");
#endif
}

TEST(r_mempools, simple_reallocations) {
  crmempool *rmp =
      crmempool_create(4, 6, 7, crmempool_fallback_disabled, false, NULL);
  char *ptr = NULL;

  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  ptr = crmempool_realloc_entry(rmp, ptr, 3);
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 1);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);

  for (int i = 0; i < 3; ++i) {
    ptr[i] = i;
  }

  char *orig = ptr;
  ptr = crmempool_realloc_entry(rmp, ptr, 6);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 1);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  REQUIRE_EQ((void *)orig, (void *)ptr);
  for (int i = 0; i < 6; ++i) {
    if (i < 3) {
      REQUIRE_EQ(ptr[i], i);
    } else {
      ptr[i] = i;
    }
  }

  ptr = crmempool_realloc_entry(rmp, ptr, 20);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 1);
  REQUIRE_NE((void *)orig, (void *)ptr);
  for (int i = 0; i < 20; ++i) {
    if (i < 6) {
      REQUIRE_EQ(ptr[i], i);
    } else {
      ptr[i] = i;
    }
  }

  orig = ptr;
  ptr = crmempool_realloc_entry(rmp, ptr, 5);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 1);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  REQUIRE_NE((void *)orig, (void *)ptr);
  for (int i = 0; i < 5; ++i) {
    REQUIRE_EQ(ptr[i], i);
  }

  crmempool_free_entry(rmp, ptr);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);

  crmempool_destroy(rmp);
}

TEST(r_mempools, simple_c_allocations) {
  crmempool *rmp =
      crmempool_create(4, 6, 7, crmempool_fallback_disabled, false, NULL);
  char *ptr = NULL;

  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);
  ptr = crmempool_calloc_entry(rmp, 1);
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 1);
  crmempool_free_entry(rmp, ptr);

  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);
  ptr = crmempool_calloc_entry(rmp, 16);
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 1);
  crmempool_free_entry(rmp, ptr);

  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  ptr = crmempool_calloc_entry(rmp, 17);
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 1);
  crmempool_free_entry(rmp, ptr);

  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  ptr = crmempool_calloc_entry(rmp, 32);
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 1);
  crmempool_free_entry(rmp, ptr);

  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 64), 0);
  ptr = crmempool_calloc_entry(rmp, 33);
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 64), 1);
  crmempool_free_entry(rmp, ptr);

  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 64), 0);
  ptr = crmempool_calloc_entry(rmp, 63);
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 64), 1);
  crmempool_free_entry(rmp, ptr);

  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 64), 0);
  ptr = crmempool_calloc_entry(rmp, 64);
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 64), 1);
  crmempool_free_entry(rmp, ptr);

  REQUIRE_EQ((void *)crmempool_calloc_entry(rmp, 0), NULL);
  REQUIRE_EQ((void *)crmempool_calloc_entry(rmp, 65), NULL);

  crmempool_destroy(rmp);
}

TEST(r_mempools, exhaust_all_fallback_disabled) {
  crmempool *rmp =
      crmempool_create(4, 6, 7, crmempool_fallback_disabled, false, NULL);

  size_t ptrs_len = 224;

  void *ptrs[ptrs_len];

  for (size_t i = 0; i < ptrs_len; ++i) {
    ptrs[i] = crmempool_alloc_entry(rmp, 8);
    REQUIRE_NE((void *)ptrs[i], NULL);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size),
               crmempool_total_capacity(rmp, size));
  }

  void *tmp_ptr = crmempool_alloc_entry(rmp, 8);
  REQUIRE_EQ((void *)tmp_ptr, NULL);

  for (size_t i = 0; i < ptrs_len; ++i) {
    crmempool_free_entry(rmp, ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size), 0);
  }

  crmempool_destroy(rmp);
}

TEST(r_mempools, c_exhaust_all_fallback_disabled) {
  crmempool *rmp =
      crmempool_create(4, 6, 7, crmempool_fallback_disabled, false, NULL);

  size_t ptrs_len = 224;

  void *ptrs[ptrs_len];

  for (size_t i = 0; i < ptrs_len; ++i) {
    ptrs[i] = crmempool_calloc_entry(rmp, 8);
    REQUIRE_NE((void *)ptrs[i], NULL);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size),
               crmempool_total_capacity(rmp, size));
  }

  void *tmp_ptr = crmempool_calloc_entry(rmp, 8);
  REQUIRE_EQ((void *)tmp_ptr, NULL);

  for (size_t i = 0; i < ptrs_len; ++i) {
    crmempool_free_entry(rmp, ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size), 0);
  }

  crmempool_destroy(rmp);
}

TEST(r_mempools, exhaust_last_subpool_returns_null_fallback_disabled) {
  crmempool *rmp =
      crmempool_create(4, 6, 7, crmempool_fallback_disabled, false, NULL);

  size_t capacity = 32;
  void *ptrs[32];

  for (size_t i = 0; i < capacity; ++i) {
    ptrs[i] = crmempool_alloc_entry(rmp, 64);
    REQUIRE_NE((void *)ptrs[i], NULL);
  }

  REQUIRE_EQ(crmempool_used_count(rmp, 64), crmempool_total_capacity(rmp, 64));
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);

  void *tmp = crmempool_alloc_entry(rmp, 64);
  REQUIRE_EQ((void *)tmp, NULL);

  for (size_t i = 0; i < capacity; ++i) {
    crmempool_free_entry(rmp, ptrs[i]);
  }

  REQUIRE_EQ(crmempool_used_count(rmp, 64), 0);

  crmempool_destroy(rmp);
}

TEST(r_mempools, try_exhausting_with_fallback_at_first_exhaustion) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 7, crmempool_fallback_at_first_exhaustion, false, NULL);

  size_t ptrs_len = 224;

  void *ptrs[ptrs_len];
  uint ptr_iterator = 0;

  for (size_t i = 0; i < 128; ++i) {
    ptrs[ptr_iterator++] = crmempool_alloc_entry(rmp, 16);
  }

  for (size_t i = 0; i < 64; ++i) {
    ptrs[ptr_iterator++] = crmempool_alloc_entry(rmp, 32);
  }

  for (size_t i = 0; i < 32; ++i) {
    ptrs[ptr_iterator++] = crmempool_alloc_entry(rmp, 64);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size),
               crmempool_total_capacity(rmp, size));
  }

  void *tmp_ptr_16 = crmempool_alloc_entry(rmp, 16);
  REQUIRE_NE((void *)tmp_ptr_16, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);

  void *tmp_ptr_32 = crmempool_alloc_entry(rmp, 32);
  REQUIRE_NE((void *)tmp_ptr_32, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 1);

  void *tmp_ptr_64 = crmempool_alloc_entry(rmp, 64);
  REQUIRE_NE((void *)tmp_ptr_64, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 1);

  for (size_t i = 0; i < ptrs_len; ++i) {
    crmempool_free_entry(rmp, ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size), 0);
  }

  crmempool_free_entry(rmp, tmp_ptr_16);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);

  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 1);
  crmempool_free_entry(rmp, tmp_ptr_32);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 0);

  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 1);
  crmempool_free_entry(rmp, tmp_ptr_64);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 0);

  crmempool_destroy(rmp);
}

TEST(r_mempools, c_try_exhausting_with_fallback_at_first_exhaustion) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 7, crmempool_fallback_at_first_exhaustion, false, NULL);

  size_t ptrs_len = 224;

  void *ptrs[ptrs_len];
  uint ptr_iterator = 0;

  for (size_t i = 0; i < 128; ++i) {
    ptrs[ptr_iterator++] = crmempool_calloc_entry(rmp, 16);
  }

  for (size_t i = 0; i < 64; ++i) {
    ptrs[ptr_iterator++] = crmempool_calloc_entry(rmp, 32);
  }

  for (size_t i = 0; i < 32; ++i) {
    ptrs[ptr_iterator++] = crmempool_calloc_entry(rmp, 64);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size),
               crmempool_total_capacity(rmp, size));
  }

  void *tmp_ptr_16 = crmempool_calloc_entry(rmp, 16);
  REQUIRE_NE((void *)tmp_ptr_16, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);

  void *tmp_ptr_32 = crmempool_calloc_entry(rmp, 32);
  REQUIRE_NE((void *)tmp_ptr_32, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 1);

  void *tmp_ptr_64 = crmempool_calloc_entry(rmp, 64);
  REQUIRE_NE((void *)tmp_ptr_64, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 1);

  for (size_t i = 0; i < ptrs_len; ++i) {
    crmempool_free_entry(rmp, ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size), 0);
  }

  crmempool_free_entry(rmp, tmp_ptr_16);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);

  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 1);
  crmempool_free_entry(rmp, tmp_ptr_32);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 0);

  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 1);
  crmempool_free_entry(rmp, tmp_ptr_64);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 0);

  crmempool_destroy(rmp);
}

TEST(r_mempools, c_try_exhausting_with_fallback_at_first_exhaustion_no_locks) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 7, crmempool_fallback_at_first_exhaustion, true, NULL);

  size_t ptrs_len = 224;

  void *ptrs[ptrs_len];
  uint ptr_iterator = 0;

  for (size_t i = 0; i < 128; ++i) {
    ptrs[ptr_iterator++] = crmempool_calloc_entry(rmp, 16);
  }

  for (size_t i = 0; i < 64; ++i) {
    ptrs[ptr_iterator++] = crmempool_calloc_entry(rmp, 32);
  }

  for (size_t i = 0; i < 32; ++i) {
    ptrs[ptr_iterator++] = crmempool_calloc_entry(rmp, 64);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size),
               crmempool_total_capacity(rmp, size));
  }

  void *tmp_ptr_16 = crmempool_calloc_entry(rmp, 16);
  REQUIRE_NE((void *)tmp_ptr_16, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);

  void *tmp_ptr_32 = crmempool_calloc_entry(rmp, 32);
  REQUIRE_NE((void *)tmp_ptr_32, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 1);

  void *tmp_ptr_64 = crmempool_calloc_entry(rmp, 64);
  REQUIRE_NE((void *)tmp_ptr_64, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 1);

  for (size_t i = 0; i < ptrs_len; ++i) {
    crmempool_free_entry(rmp, ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size), 0);
  }

  crmempool_free_entry(rmp, tmp_ptr_16);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);

  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 1);
  crmempool_free_entry(rmp, tmp_ptr_32);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 0);

  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 1);
  crmempool_free_entry(rmp, tmp_ptr_64);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 0);

  crmempool_destroy(rmp);
}

TEST(r_mempools, try_exhausting_with_fallback_at_last_exhaustion) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 7, crmempool_fallback_at_last_exhaustion, false, NULL);

  size_t ptrs_len = 224;

  void *ptrs[ptrs_len];
  uint ptr_iterator = 0;

  for (size_t i = 0; i < 128; ++i) {
    ptrs[ptr_iterator++] = crmempool_alloc_entry(rmp, 16);
  }

  for (size_t i = 0; i < 64; ++i) {
    ptrs[ptr_iterator++] = crmempool_alloc_entry(rmp, 32);
  }

  for (size_t i = 0; i < 32; ++i) {
    ptrs[ptr_iterator++] = crmempool_alloc_entry(rmp, 64);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size),
               crmempool_total_capacity(rmp, size));
  }

  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 0);
  void *tmp_ptr_16 = crmempool_alloc_entry(rmp, 16);
  REQUIRE_NE((void *)tmp_ptr_16, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 1);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 1);

  void *tmp_ptr_32 = crmempool_alloc_entry(rmp, 32);
  REQUIRE_NE((void *)tmp_ptr_32, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 2);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 2);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 2);

  void *tmp_ptr_64 = crmempool_alloc_entry(rmp, 64);
  REQUIRE_NE((void *)tmp_ptr_64, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 3);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 3);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 3);

  for (size_t i = 0; i < ptrs_len; ++i) {
    crmempool_free_entry(rmp, ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size), 0);
  }

  crmempool_free_entry(rmp, tmp_ptr_16);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 2);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 2);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 2);
  crmempool_free_entry(rmp, tmp_ptr_32);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 1);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 1);
  crmempool_free_entry(rmp, tmp_ptr_64);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 0);

  crmempool_destroy(rmp);
}

TEST(r_mempools, c_try_exhausting_with_fallback_at_last_exhaustion) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 7, crmempool_fallback_at_last_exhaustion, false, NULL);

  size_t ptrs_len = 224;

  void *ptrs[ptrs_len];
  uint ptr_iterator = 0;

  for (size_t i = 0; i < 128; ++i) {
    ptrs[ptr_iterator++] = crmempool_calloc_entry(rmp, 16);
  }

  for (size_t i = 0; i < 64; ++i) {
    ptrs[ptr_iterator++] = crmempool_calloc_entry(rmp, 32);
  }

  for (size_t i = 0; i < 32; ++i) {
    ptrs[ptr_iterator++] = crmempool_calloc_entry(rmp, 64);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size),
               crmempool_total_capacity(rmp, size));
  }

  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 0);
  void *tmp_ptr_16 = crmempool_calloc_entry(rmp, 16);
  REQUIRE_NE((void *)tmp_ptr_16, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 1);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 1);

  void *tmp_ptr_32 = crmempool_calloc_entry(rmp, 32);
  REQUIRE_NE((void *)tmp_ptr_32, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 2);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 2);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 2);

  void *tmp_ptr_64 = crmempool_calloc_entry(rmp, 64);
  REQUIRE_NE((void *)tmp_ptr_64, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 3);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 3);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 3);

  for (size_t i = 0; i < ptrs_len; ++i) {
    crmempool_free_entry(rmp, ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size), 0);
  }

  crmempool_free_entry(rmp, tmp_ptr_16);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 2);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 2);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 2);
  crmempool_free_entry(rmp, tmp_ptr_32);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 1);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 1);
  crmempool_free_entry(rmp, tmp_ptr_64);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 0);

  crmempool_destroy(rmp);
}

TEST(r_mempools, c_try_exhausting_with_fallback_at_last_exhaustion_no_locks) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 7, crmempool_fallback_at_last_exhaustion, true, NULL);

  size_t ptrs_len = 224;

  void *ptrs[ptrs_len];
  uint ptr_iterator = 0;

  for (size_t i = 0; i < 128; ++i) {
    ptrs[ptr_iterator++] = crmempool_calloc_entry(rmp, 16);
  }

  for (size_t i = 0; i < 64; ++i) {
    ptrs[ptr_iterator++] = crmempool_calloc_entry(rmp, 32);
  }

  for (size_t i = 0; i < 32; ++i) {
    ptrs[ptr_iterator++] = crmempool_calloc_entry(rmp, 64);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size),
               crmempool_total_capacity(rmp, size));
  }

  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 0);
  void *tmp_ptr_16 = crmempool_calloc_entry(rmp, 16);
  REQUIRE_NE((void *)tmp_ptr_16, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 1);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 1);

  void *tmp_ptr_32 = crmempool_calloc_entry(rmp, 32);
  REQUIRE_NE((void *)tmp_ptr_32, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 2);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 2);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 2);

  void *tmp_ptr_64 = crmempool_calloc_entry(rmp, 64);
  REQUIRE_NE((void *)tmp_ptr_64, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 3);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 3);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 3);

  for (size_t i = 0; i < ptrs_len; ++i) {
    crmempool_free_entry(rmp, ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size), 0);
  }

  crmempool_free_entry(rmp, tmp_ptr_16);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 2);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 2);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 2);
  crmempool_free_entry(rmp, tmp_ptr_32);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 1);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 1);
  crmempool_free_entry(rmp, tmp_ptr_64);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 0);

  crmempool_destroy(rmp);
}

TEST(r_mempools, create_fails_power_exceeds_size_t_width) {
  char *err;

  SCOPED_R_MEMPOOL(rmp) =
      crmempool_create(64, 65, 65, crmempool_fallback_disabled, false, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  rmp = crmempool_create(4, 64, 64, crmempool_fallback_disabled, false, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  rmp = crmempool_create(4, 6, 64, crmempool_fallback_disabled, false, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// Procs whose calloc always fails. The configuration below makes one calloc
// whose count * size overflows, which glibc answers with NULL but ASan aborts
// on, so the stub gives the same answer in both builds.
static void *max_size_test_malloc(size_t size) { return malloc(size); }
static void *max_size_test_calloc(size_t count, size_t size) {
  (void)count;
  (void)size;
  return NULL;
}
static void *max_size_test_realloc(void *ptr, size_t size) {
  return realloc(ptr, size);
}
static void max_size_test_free(void *ptr) { free(ptr); }

// 2^63 is a valid largest size and must pass validation (a limit of
// SIZE_MAX / 2 would refuse it); the call then fails at the allocation.
TEST(r_mempools, largest_size_power_of_two_at_true_maximum_passes_validation) {
  ccol_memmgmt_procs m_procs = {.malloc = max_size_test_malloc,
                                .calloc = max_size_test_calloc,
                                .realloc = max_size_test_realloc,
                                .free = max_size_test_free};
  char *err = NULL;
  SCOPED_R_MEMPOOL(rmp) = crmempool_create_mp(
      4, 63, 59, crmempool_fallback_disabled, true, &m_procs, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ(strstr(err, "beyond limits"), NULL);
}

// The count power at which an unreduced product in the ranged buffer formula
// would overflow while the real size still fits; it depends on the width of
// size_t.
#if SIZE_MAX > 0xFFFFFFFFu
#define CCOL_TEST_PREMATURE_OVERFLOW_SC 56
#else
#define CCOL_TEST_PREMATURE_OVERFLOW_SC 24
#endif
// Compares the macro with a sum over the tiers, a different algorithm with no
// large intermediate product.
TEST(r_mempools, calculate_preallocated_buffer_size_no_premature_overflow) {
  const uint8_t ss = 4, ls = 11, sc = CCOL_TEST_PREMATURE_OVERFLOW_SC;

  size_t expected = 0;
  size_t esize = (size_t)1 << ss;
  size_t ecount = (size_t)1 << sc;
  for (uint8_t i = 0; i < (uint8_t)(ls - ss + 1); ++i) {
    expected += ecount * esize + ecount;
    esize *= 2;
    ecount /= 2;
  }

  REQUIRE_EQ(CRMEMPOOL__CALCULATE_PREALLOCATED_BUFFER_SIZE(ss, ls, sc),
             expected);

  size_t unreduced_second_term =
      (2 * ((size_t)1 << sc) * (((size_t)1 << (ls - ss + 1)) - 1)) /
      ((size_t)1 << (ls - ss + 1));
  size_t unreduced_result =
      ((size_t)(ls - ss + 1)) * ((size_t)1 << sc) * ((size_t)1 << ss) +
      unreduced_second_term;
  REQUIRE_NE(unreduced_result, expected);
}
#undef CCOL_TEST_PREMATURE_OVERFLOW_SC

TEST(r_mempools, realloc_first_exhaustion_entry_grows) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 7, crmempool_fallback_at_first_exhaustion, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *fill[128];
  for (size_t i = 0; i < 128; ++i) {
    fill[i] = crmempool_alloc_entry(rmp, 16);
    REQUIRE_NE(fill[i], NULL);
  }
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 128);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);

  char *entry = crmempool_alloc_entry(rmp, 16);
  REQUIRE_NE((void *)entry, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);

  for (int i = 0; i < 16; ++i) {
    entry[i] = (char)(i + 1);
  }

  char *grown = crmempool_realloc_entry(rmp, entry, 32);
  REQUIRE_NE((void *)grown, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 1);

  for (int i = 0; i < 16; ++i) {
    REQUIRE_EQ(grown[i], (char)(i + 1));
  }

  crmempool_free_entry(rmp, grown);
  for (size_t i = 0; i < 128; ++i) {
    crmempool_free_entry(rmp, fill[i]);
  }

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, realloc_last_exhaustion_pseudo_pool_entry_shrinks) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 7, crmempool_fallback_at_last_exhaustion, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *fill[224];
  size_t iter = 0;
  for (size_t i = 0; i < 128; ++i)
    fill[iter++] = crmempool_alloc_entry(rmp, 16);
  for (size_t i = 0; i < 64; ++i) fill[iter++] = crmempool_alloc_entry(rmp, 32);
  for (size_t i = 0; i < 32; ++i) fill[iter++] = crmempool_alloc_entry(rmp, 64);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 0);

  char *entry = crmempool_alloc_entry(rmp, 32);
  REQUIRE_NE((void *)entry, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 1);
  for (int i = 0; i < 32; ++i) entry[i] = (char)(i + 1);

  char *shrunk = crmempool_realloc_entry(rmp, entry, 16);
  REQUIRE_NE((void *)shrunk, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);
  for (int i = 0; i < 16; ++i) {
    REQUIRE_EQ(shrunk[i], (char)(i + 1));
  }

  crmempool_free_entry(rmp, shrunk);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);

  for (size_t i = 0; i < 224; ++i) {
    crmempool_free_entry(rmp, fill[i]);
  }

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, realloc_last_exhaustion_pseudo_pool_entry_grows) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 7, crmempool_fallback_at_last_exhaustion, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *fill[224];
  size_t iter = 0;
  for (size_t i = 0; i < 128; ++i)
    fill[iter++] = crmempool_alloc_entry(rmp, 16);
  for (size_t i = 0; i < 64; ++i) fill[iter++] = crmempool_alloc_entry(rmp, 32);
  for (size_t i = 0; i < 32; ++i) fill[iter++] = crmempool_alloc_entry(rmp, 64);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);

  char *entry = crmempool_alloc_entry(rmp, 16);
  REQUIRE_NE((void *)entry, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);
  for (int i = 0; i < 16; ++i) entry[i] = (char)(i + 1);

  char *grown = crmempool_realloc_entry(rmp, entry, 32);
  REQUIRE_NE((void *)grown, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 1);
  for (int i = 0; i < 16; ++i) {
    REQUIRE_EQ(grown[i], (char)(i + 1));
  }

  crmempool_free_entry(rmp, grown);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 0);

  for (size_t i = 0; i < 224; ++i) {
    crmempool_free_entry(rmp, fill[i]);
  }

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools,
     realloc_last_exhaustion_pseudo_pool_entry_same_size_is_a_noop) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 7, crmempool_fallback_at_last_exhaustion, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *fill[224];
  size_t iter = 0;
  for (size_t i = 0; i < 128; ++i)
    fill[iter++] = crmempool_alloc_entry(rmp, 16);
  for (size_t i = 0; i < 64; ++i) fill[iter++] = crmempool_alloc_entry(rmp, 32);
  for (size_t i = 0; i < 32; ++i) fill[iter++] = crmempool_alloc_entry(rmp, 64);

  char *entry = crmempool_alloc_entry(rmp, 20);
  REQUIRE_NE((void *)entry, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 20), 1);
  for (int i = 0; i < 20; ++i) {
    entry[i] = (char)(i + 1);
  }

  char *same = crmempool_realloc_entry(rmp, entry, 20);
  REQUIRE_EQ((void *)same, (void *)entry);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 20), 1);
  for (int i = 0; i < 20; ++i) {
    REQUIRE_EQ(same[i], (char)(i + 1));
  }

  char *moved = crmempool_realloc_entry(rmp, same, 10);
  REQUIRE_NE((void *)moved, (void *)same);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 10), 1);
  for (int i = 0; i < 10; ++i) {
    REQUIRE_EQ(moved[i], (char)(i + 1));
  }

  crmempool_free_entry(rmp, moved);
  for (size_t i = 0; i < 224; ++i) {
    crmempool_free_entry(rmp, fill[i]);
  }

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(
    r_mempools,
    realloc_last_exhaustion_pseudo_pool_entry_below_word_size_same_size_is_a_noop) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 7, crmempool_fallback_at_last_exhaustion, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *fill[224];
  size_t iter = 0;
  for (size_t i = 0; i < 128; ++i)
    fill[iter++] = crmempool_alloc_entry(rmp, 16);
  for (size_t i = 0; i < 64; ++i) fill[iter++] = crmempool_alloc_entry(rmp, 32);
  for (size_t i = 0; i < 32; ++i) fill[iter++] = crmempool_alloc_entry(rmp, 64);

  char *entry = crmempool_alloc_entry(rmp, 3);
  REQUIRE_NE((void *)entry, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 3), 1);
  for (int i = 0; i < 3; ++i) {
    entry[i] = (char)(i + 1);
  }

  char *same = crmempool_realloc_entry(rmp, entry, 3);
  REQUIRE_EQ((void *)same, (void *)entry);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 3), 1);
  for (int i = 0; i < 3; ++i) {
    REQUIRE_EQ(same[i], (char)(i + 1));
  }

  char *moved = crmempool_realloc_entry(rmp, same, 5);
  REQUIRE_NE((void *)moved, (void *)same);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 5), 1);
  for (int i = 0; i < 3; ++i) {
    REQUIRE_EQ(moved[i], (char)(i + 1));
  }

  crmempool_free_entry(rmp, moved);
  for (size_t i = 0; i < 224; ++i) {
    crmempool_free_entry(rmp, fill[i]);
  }

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

// The counting allocator of the next tests: A realloc that needs no move must
// reach the allocator zero times, which they assert instead of timing
// anything.
static atomic_size_t mp_rc_allocs;
static atomic_size_t mp_rc_frees;
static void *mp_rc_malloc(size_t n) {
  atomic_fetch_add(&mp_rc_allocs, 1);
  return malloc(n);
}
static void *mp_rc_calloc(size_t a, size_t b) {
  atomic_fetch_add(&mp_rc_allocs, 1);
  return calloc(a, b);
}
static void *mp_rc_realloc(void *p, size_t n) {
  atomic_fetch_add(&mp_rc_allocs, 1);
  return realloc(p, n);
}
static void mp_rc_free(void *p) {
  if (p) atomic_fetch_add(&mp_rc_frees, 1);
  free(p);
}

TEST(r_mempools,
     realloc_of_a_tier_backed_dynamic_entry_within_its_tier_is_noop) {
  atomic_store(&mp_rc_allocs, 0);
  atomic_store(&mp_rc_frees, 0);
  ccol_memmgmt_procs procs = {.malloc = mp_rc_malloc,
                              .free = mp_rc_free,
                              .calloc = mp_rc_calloc,
                              .realloc = mp_rc_realloc};
  SCOPED_R_MEMPOOL(rmp) = crmempool_create_mp(
      4, 6, 3, crmempool_fallback_at_first_exhaustion, true, &procs, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *fill[4];
  bool filled = true;
  for (size_t i = 0; i < 4; ++i) {
    fill[i] = crmempool_alloc_entry(rmp, 32);
    if (!fill[i]) filled = false;
  }

  char *dyn = (char *)crmempool_alloc_entry(rmp, 20);
  bool is_dynamic =
      (dyn != NULL) && (crmempool_dynamic_allocs_count(rmp, 20) == 1);
  if (dyn) {
    for (int i = 0; i < 20; ++i) {
      dyn[i] = (char)(i + 1);
    }
  }

  size_t allocs_before = atomic_load(&mp_rc_allocs);
  size_t frees_before = atomic_load(&mp_rc_frees);
  char *same = dyn ? (char *)crmempool_realloc_entry(rmp, dyn, 20) : NULL;
  char *still = same ? (char *)crmempool_realloc_entry(rmp, same, 32) : NULL;
  char *owned = still ? still : (same ? same : dyn);
  size_t allocs_after = atomic_load(&mp_rc_allocs);
  size_t frees_after = atomic_load(&mp_rc_frees);

  bool contents_kept = (still != NULL);
  for (int i = 0; still && i < 20; ++i) {
    if (still[i] != (char)(i + 1)) contents_kept = false;
  }

  crmempool_free_entry(rmp, fill[0]);
  char *pool_owned = (char *)crmempool_alloc_entry(rmp, 20);
  size_t pool_allocs_before = atomic_load(&mp_rc_allocs);
  char *pool_same =
      pool_owned ? (char *)crmempool_realloc_entry(rmp, pool_owned, 20) : NULL;
  bool pool_is_noop = (pool_same == pool_owned) && (pool_owned != NULL) &&
                      (atomic_load(&mp_rc_allocs) == pool_allocs_before);

  bool first_is_noop = (same == dyn);
  bool second_is_noop = (still == dyn);

  if (pool_same) crmempool_free_entry(rmp, pool_same);
  if (owned) crmempool_free_entry(rmp, owned);
  for (size_t i = 1; i < 4; ++i) {
    if (fill[i]) crmempool_free_entry(rmp, fill[i]);
  }
  crmempool_destroy(rmp);

  REQUIRE_TRUE(filled);
  REQUIRE_TRUE(is_dynamic);
  REQUIRE_TRUE(first_is_noop);
  REQUIRE_TRUE(second_is_noop);
  REQUIRE_EQ(allocs_after - allocs_before, 0);
  REQUIRE_EQ(frees_after - frees_before, 0);
  REQUIRE_TRUE(contents_kept);
  REQUIRE_TRUE(pool_is_noop);
}

// An allocator that refuses everything while mp_rd_deny is set, so a realloc
// that needs no allocation can be held to that.
static atomic_bool mp_rd_deny;
static void *mp_rd_malloc(size_t n) {
  return atomic_load(&mp_rd_deny) ? NULL : malloc(n);
}
static void *mp_rd_calloc(size_t a, size_t b) {
  return atomic_load(&mp_rd_deny) ? NULL : calloc(a, b);
}
static void *mp_rd_realloc(void *p, size_t n) {
  return atomic_load(&mp_rd_deny) ? NULL : realloc(p, n);
}
static void mp_rd_free(void *p) { free(p); }

TEST(r_mempools, realloc_that_already_fits_survives_an_allocation_failure) {
  ccol_memmgmt_procs procs = {.malloc = mp_rd_malloc,
                              .free = mp_rd_free,
                              .calloc = mp_rd_calloc,
                              .realloc = mp_rd_realloc};
  atomic_store(&mp_rd_deny, false);
  SCOPED_R_MEMPOOL(rmp) = crmempool_create_mp(
      4, 6, 3, crmempool_fallback_at_first_exhaustion, true, &procs, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *f16[8];
  void *f32[4];
  void *f64[2];
  bool filled = true;
  for (size_t i = 0; i < 8; ++i) {
    f16[i] = crmempool_alloc_entry(rmp, 16);
    if (!f16[i]) filled = false;
  }
  for (size_t i = 0; i < 4; ++i) {
    f32[i] = crmempool_alloc_entry(rmp, 32);
    if (!f32[i]) filled = false;
  }
  for (size_t i = 0; i < 2; ++i) {
    f64[i] = crmempool_alloc_entry(rmp, 64);
    if (!f64[i]) filled = false;
  }

  char *dyn = (char *)crmempool_alloc_entry(rmp, 32);
  bool is_dynamic =
      (dyn != NULL) && (crmempool_dynamic_allocs_count(rmp, 32) == 1);
  if (dyn) {
    for (int i = 0; i < 32; ++i) {
      dyn[i] = (char)(i + 1);
    }
  }

  atomic_store(&mp_rd_deny, true);
  char *shrunk = dyn ? (char *)crmempool_realloc_entry(rmp, dyn, 8) : NULL;
  char *owned = shrunk ? shrunk : dyn;
  char *pool_shrunk = (char *)crmempool_realloc_entry(rmp, f64[0], 8);
  atomic_store(&mp_rd_deny, false);

  bool contents_kept = (shrunk != NULL);
  for (int i = 0; shrunk && i < 32; ++i) {
    if (shrunk[i] != (char)(i + 1)) contents_kept = false;
  }
  bool still_countable = (crmempool_dynamic_allocs_count(rmp, 32) == 1);
  bool dynamic_kept_its_buffer = (shrunk == dyn);
  bool pool_kept_its_buffer = (pool_shrunk == f64[0]);

  if (owned) crmempool_free_entry(rmp, owned);
  for (size_t i = 0; i < 8; ++i) {
    if (f16[i]) crmempool_free_entry(rmp, f16[i]);
  }
  for (size_t i = 0; i < 4; ++i) {
    if (f32[i]) crmempool_free_entry(rmp, f32[i]);
  }
  if (pool_shrunk) crmempool_free_entry(rmp, pool_shrunk);
  if (f64[1]) crmempool_free_entry(rmp, f64[1]);
  crmempool_destroy(rmp);

  REQUIRE_TRUE(filled);
  REQUIRE_TRUE(is_dynamic);
  REQUIRE_TRUE(dynamic_kept_its_buffer);
  REQUIRE_TRUE(contents_kept);
  REQUIRE_TRUE(still_countable);
  REQUIRE_TRUE(pool_kept_its_buffer);
}

TEST(r_mempools, custom_allocator_propagated_to_pseudo_pool) {
  ccol_memmgmt_procs m_procs = {
      .malloc = malloc, .calloc = calloc, .realloc = realloc, .free = free};

  SCOPED_R_MEMPOOL(rmp) = crmempool_create_mp(
      4, 6, 7, crmempool_fallback_at_last_exhaustion, false, &m_procs, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *fill[224];
  size_t iter = 0;
  for (size_t i = 0; i < 128; ++i)
    fill[iter++] = crmempool_alloc_entry(rmp, 16);
  for (size_t i = 0; i < 64; ++i) fill[iter++] = crmempool_alloc_entry(rmp, 32);
  for (size_t i = 0; i < 32; ++i) fill[iter++] = crmempool_alloc_entry(rmp, 64);

  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);

  void *entry = crmempool_alloc_entry(rmp, 16);
  REQUIRE_NE(entry, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);

  crmempool_free_entry(rmp, entry);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);

  for (size_t i = 0; i < 224; ++i) {
    crmempool_free_entry(rmp, fill[i]);
  }

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, custom_allocator_with_fallback_at_first_exhaustion) {
  ccol_memmgmt_procs m_procs = {
      .malloc = malloc, .calloc = calloc, .realloc = realloc, .free = free};

  SCOPED_R_MEMPOOL(rmp) = crmempool_create_mp(
      4, 6, 7, crmempool_fallback_at_first_exhaustion, false, &m_procs, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *fill[128];
  for (size_t i = 0; i < 128; ++i) {
    fill[i] = crmempool_alloc_entry(rmp, 16);
    REQUIRE_NE(fill[i], NULL);
  }
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 128);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);

  void *dyn = crmempool_alloc_entry(rmp, 16);
  REQUIRE_NE(dyn, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);

  crmempool_free_entry(rmp, dyn);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);

  for (size_t i = 0; i < 128; ++i) {
    crmempool_free_entry(rmp, fill[i]);
  }
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(preallocated_r_mempools, exhaust_all_fallback_disabled) {
  static CRMEMPOOL_DECLARE_PREALLOCATED_BUFFER(preallocated_rmp_buffer, 4, 6,
                                               7);

  SCOPED_R_MEMPOOL(rmp) = crmempool_create_from_preallocated_buffer(
      preallocated_rmp_buffer, sizeof(preallocated_rmp_buffer), 4, 6, 7,
      crmempool_fallback_disabled, false, NULL);

  size_t ptrs_len = 224;

  void *ptrs[ptrs_len];

  for (size_t i = 0; i < ptrs_len; ++i) {
    ptrs[i] = crmempool_alloc_entry(rmp, 8);
    REQUIRE_NE((void *)ptrs[i], NULL);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size),
               crmempool_total_capacity(rmp, size));
  }

  void *tmp_ptr = crmempool_alloc_entry(rmp, 8);
  REQUIRE_EQ((void *)tmp_ptr, NULL);

  for (size_t i = 0; i < ptrs_len; ++i) {
    crmempool_free_entry(rmp, ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size), 0);
  }

  crmempool_destroy(rmp);
}

TEST(preallocated_r_mempools, exhaust_all_fallback_disabled_no_locks) {
  static CRMEMPOOL_DECLARE_PREALLOCATED_BUFFER(preallocated_rmp_buffer, 4, 6,
                                               7);

  SCOPED_R_MEMPOOL(rmp) = crmempool_create_from_preallocated_buffer(
      preallocated_rmp_buffer, sizeof(preallocated_rmp_buffer), 4, 6, 7,
      crmempool_fallback_disabled, true, NULL);

  size_t ptrs_len = 224;

  void *ptrs[ptrs_len];

  for (size_t i = 0; i < ptrs_len; ++i) {
    ptrs[i] = crmempool_alloc_entry(rmp, 8);
    REQUIRE_NE((void *)ptrs[i], NULL);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size),
               crmempool_total_capacity(rmp, size));
  }

  void *tmp_ptr = crmempool_alloc_entry(rmp, 8);
  REQUIRE_EQ((void *)tmp_ptr, NULL);

  for (size_t i = 0; i < ptrs_len; ++i) {
    crmempool_free_entry(rmp, ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size), 0);
  }

  crmempool_destroy(rmp);
}

TEST(r_mempools, calloc_entry_zeroes_memory) {
  crmempool *rmp =
      crmempool_create(4, 6, 7, crmempool_fallback_disabled, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  char *p = crmempool_alloc_entry(rmp, 16);
  REQUIRE_NE((void *)p, NULL);
  memset(p, 0xFF, 16);
  crmempool_free_entry(rmp, p);

  p = crmempool_calloc_entry(rmp, 16);
  REQUIRE_NE((void *)p, NULL);
  for (size_t i = 0; i < 16; ++i) {
    REQUIRE_EQ(p[i], 0);
  }
  crmempool_free_entry(rmp, p);

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, realloc_null_addr_acts_like_alloc) {
  crmempool *rmp =
      crmempool_create(4, 6, 7, crmempool_fallback_disabled, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);
  void *p = crmempool_realloc_entry(rmp, NULL, 8);
  REQUIRE_NE(p, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 1);

  crmempool_free_entry(rmp, p);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, realloc_returns_null_for_invalid_size) {
  crmempool *rmp =
      crmempool_create(4, 6, 7, crmempool_fallback_disabled, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  REQUIRE_EQ((void *)crmempool_realloc_entry(rmp, NULL, 0), NULL);
  REQUIRE_EQ((void *)crmempool_realloc_entry(rmp, NULL, 65), NULL);

  void *p = crmempool_alloc_entry(rmp, 16);
  REQUIRE_NE(p, NULL);
  crmempool_free_entry(rmp, p);

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools,
     realloc_invalid_size_with_non_null_addr_leaves_addr_untouched) {
  crmempool *rmp =
      crmempool_create(4, 6, 7, crmempool_fallback_disabled, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  char *addr = crmempool_alloc_entry(rmp, 16);
  REQUIRE_NE((void *)addr, NULL);
  addr[0] = 0x5a;

  REQUIRE_EQ((void *)crmempool_realloc_entry(rmp, addr, 0), NULL);
  REQUIRE_EQ(addr[0], (char)0x5a);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 1);

  REQUIRE_EQ((void *)crmempool_realloc_entry(rmp, addr, 65), NULL);
  REQUIRE_EQ(addr[0], (char)0x5a);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 1);

  crmempool_free_entry(rmp, addr);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools,
     realloc_returns_null_when_full_no_fallback_preserves_original) {
  crmempool *rmp =
      crmempool_create(4, 5, 4, crmempool_fallback_disabled, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  size_t cap16 = crmempool_total_capacity(rmp, 16);
  size_t cap32 = crmempool_total_capacity(rmp, 32);
  REQUIRE_EQ(cap16, 16);
  REQUIRE_EQ(cap32, 8);

  void *fill16[16];
  void *fill32[8];

  for (size_t i = 0; i < cap16; ++i) {
    fill16[i] = crmempool_alloc_entry(rmp, 16);
    REQUIRE_NE(fill16[i], NULL);
  }
  for (size_t i = 0; i < cap32; ++i) {
    fill32[i] = crmempool_alloc_entry(rmp, 32);
    REQUIRE_NE(fill32[i], NULL);
  }

  void *orig = fill16[0];
  void *result = crmempool_realloc_entry(rmp, fill16[0], 32);
  REQUIRE_EQ(result, NULL);
  REQUIRE_EQ(fill16[0], orig);

  for (size_t i = 0; i < cap16; ++i) crmempool_free_entry(rmp, fill16[i]);
  for (size_t i = 0; i < cap32; ++i) crmempool_free_entry(rmp, fill32[i]);

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

// The neighbouring full-pool test only grows, so this shrink is what shows
// that an entry that already fits is kept when nothing can be allocated.
TEST(r_mempools, realloc_shrink_with_no_room_keeps_the_entry_it_already_fits) {
  crmempool *rmp =
      crmempool_create(4, 5, 4, crmempool_fallback_disabled, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  size_t cap16 = crmempool_total_capacity(rmp, 16);
  size_t cap32 = crmempool_total_capacity(rmp, 32);

  enum { FILL16 = 16, FILL32 = 8 };
  void *fill16[FILL16];
  void *fill32[FILL32];
  bool alloc_ok = (cap16 == FILL16 && cap32 == FILL32);
  if (cap16 > FILL16) cap16 = FILL16;
  if (cap32 > FILL32) cap32 = FILL32;
  for (size_t i = 0; i < cap16; ++i) {
    fill16[i] = crmempool_alloc_entry(rmp, 16);
    if (!fill16[i]) alloc_ok = false;
  }
  for (size_t i = 0; i < cap32; ++i) {
    fill32[i] = crmempool_alloc_entry(rmp, 32);
    if (!fill32[i]) alloc_ok = false;
  }

  void *orig = alloc_ok ? fill32[0] : NULL;
  void *result = orig ? crmempool_realloc_entry(rmp, orig, 16) : NULL;
  if (result) fill32[0] = result;

  bool kept = (orig != NULL && result == orig);
  if (kept) memset(result, 0x5a, 16);

  void *grow = alloc_ok ? crmempool_realloc_entry(rmp, fill16[0], 32) : NULL;
  bool grow_refused = (alloc_ok && grow == NULL);
  if (grow) fill16[0] = grow;

  for (size_t i = 0; i < cap16; ++i) crmempool_free_entry(rmp, fill16[i]);
  for (size_t i = 0; i < cap32; ++i) crmempool_free_entry(rmp, fill32[i]);
  crmempool_destroy(rmp);

  REQUIRE_TRUE(alloc_ok);
  REQUIRE_TRUE(kept);
  REQUIRE_TRUE(grow_refused);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, realloc_foreign_pointer_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    SCOPED_R_MEMPOOL(rmp) =
        crmempool_create(4, 6, 7, crmempool_fallback_disabled, false, NULL);
    int junk[8] = {0};
    void *foreign_ptr = &junk[2];
    crmempool_realloc_entry(rmp, foreign_ptr, 16);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

TEST(r_mempools, realloc_pointer_from_a_different_r_mempool_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    SCOPED_R_MEMPOOL(rmp1) =
        crmempool_create(4, 6, 7, crmempool_fallback_disabled, true, NULL);
    SCOPED_R_MEMPOOL(rmp2) =
        crmempool_create(4, 6, 7, crmempool_fallback_disabled, true, NULL);
    void *from_rmp2 = crmempool_alloc_entry(rmp2, 16);
    crmempool_realloc_entry(rmp1, from_rmp2, 16);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

TEST(r_mempools, realloc_already_freed_pointer_same_tier_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    SCOPED_R_MEMPOOL(rmp) =
        crmempool_create(4, 6, 7, crmempool_fallback_disabled, true, NULL);
    void *a = crmempool_alloc_entry(rmp, 16);
    crmempool_alloc_entry(rmp, 16);
    void *a_saved = a;
    crmempool_free_entry(rmp, a);
    crmempool_realloc_entry(rmp, a_saved, 8);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// The stack block is zeroed, so reading a header in front of the address
// would show up as a NULL pool pointer instead of random stack bytes.
TEST(r_mempools, realloc_address_outside_every_tier_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    SCOPED_R_MEMPOOL(rmp) =
        crmempool_create(4, 6, 7, crmempool_fallback_disabled, true, NULL);
    void *real = crmempool_alloc_entry(rmp, 16);
    if (!real) test_child_exit(2);

    _Alignas(cmempool__entry_align) uint8_t foreign[256];
    memset(foreign, 0, sizeof(foreign));

    crmempool_realloc_entry(rmp, foreign + 128, 16);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

TEST(r_mempools, realloc_pool_owned_entry_with_corrupted_status_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    SCOPED_R_MEMPOOL(rmp) = crmempool_create(
        4, 6, 2, crmempool_fallback_at_first_exhaustion, true, NULL);
    void *pool_owned = crmempool_alloc_entry(rmp, 16);
    if (!pool_owned) test_child_exit(2);

    crmempool__corrupt_entry_status_for_tests(rmp, pool_owned, 0xAB);

    crmempool_realloc_entry(rmp, pool_owned, 16);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

TEST(r_mempools, realloc_escalated_to_pseudo_pool_does_not_overflow_buffer) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 7, crmempool_fallback_at_last_exhaustion, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *pool1_fill[64];
  for (int i = 0; i < 64; ++i) pool1_fill[i] = crmempool_alloc_entry(rmp, 32);

  void *pool2_fill[32];
  for (int i = 0; i < 32; ++i) pool2_fill[i] = crmempool_alloc_entry(rmp, 64);
  char *old_entry = (char *)pool2_fill[0];
  REQUIRE_EQ(crmempool_used_count(rmp, 32), crmempool_total_capacity(rmp, 32));
  REQUIRE_EQ(crmempool_used_count(rmp, 64), crmempool_total_capacity(rmp, 64));

  char *shrunk = crmempool_realloc_entry(rmp, old_entry, 17);
  REQUIRE_NE((void *)shrunk, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 17), 1);

  memset(shrunk, 0x5a, 17);
  for (int i = 0; i < 17; ++i) {
    REQUIRE_EQ(shrunk[i], (char)0x5a);
  }

  crmempool_free_entry(rmp, shrunk);
  for (int i = 0; i < 64; ++i) crmempool_free_entry(rmp, pool1_fill[i]);
  for (int i = 1; i < 32; ++i) crmempool_free_entry(rmp, pool2_fill[i]);

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools,
     double_free_of_pseudo_pool_entry_with_another_still_live_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    ccol_memmgmt_procs procs = {.malloc = no_clobber_malloc,
                                .calloc = no_clobber_calloc,
                                .realloc = no_clobber_realloc,
                                .free = no_clobber_free};
    SCOPED_R_MEMPOOL(rmp) = crmempool_create_mp(
        4, 6, 7, crmempool_fallback_at_last_exhaustion, true, &procs, NULL);

    for (size_t i = 0; i < 128; ++i) crmempool_alloc_entry(rmp, 16);
    for (size_t i = 0; i < 64; ++i) crmempool_alloc_entry(rmp, 32);
    for (size_t i = 0; i < 32; ++i) crmempool_alloc_entry(rmp, 64);

    void *e1 = crmempool_alloc_entry(rmp, 16);
    void *e2 = crmempool_alloc_entry(rmp, 32);
    (void)e2;
    void *e1_saved = e1;
    crmempool_free_entry(rmp, e1);
    crmempool__free_entry(rmp, e1_saved);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// Under this policy each tier checks for leaks in its own destroy; nothing
// else would notice that check breaking.
TEST(r_mempools, destroy_with_leaked_first_exhaustion_entry_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    SCOPED_R_MEMPOOL(rmp) = crmempool_create(
        4, 6, 7, crmempool_fallback_at_first_exhaustion, true, NULL);
    for (size_t i = 0; i < 128; ++i) crmempool_alloc_entry(rmp, 16);
    void *leaked = crmempool_alloc_entry(rmp, 16);
    (void)leaked;
    crmempool_destroy(rmp);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

TEST(r_mempools,
     destroy_with_leaked_last_exhaustion_pseudo_pool_entry_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    SCOPED_R_MEMPOOL(rmp) = crmempool_create(
        4, 6, 7, crmempool_fallback_at_last_exhaustion, true, NULL);
    for (size_t i = 0; i < 128; ++i) crmempool_alloc_entry(rmp, 16);
    for (size_t i = 0; i < 64; ++i) crmempool_alloc_entry(rmp, 32);
    for (size_t i = 0; i < 32; ++i) crmempool_alloc_entry(rmp, 64);
    void *leaked = crmempool_alloc_entry(rmp, 16);
    (void)leaked;
    crmempool_destroy(rmp);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

TEST(r_mempools, create_fails_smallest_size_below_minimum) {
  char *err;
  SCOPED_R_MEMPOOL(rmp) =
      crmempool_create(3, 10, 10, crmempool_fallback_disabled, false, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(r_mempools, create_preallocated_buffer_fails) {
  CRMEMPOOL_DECLARE_PREALLOCATED_BUFFER(correct_buf, 4, 5, 4);
  char *err;

  SCOPED_R_MEMPOOL(rmp) = crmempool_create_from_preallocated_buffer(
      NULL, sizeof(correct_buf), 4, 5, 4, crmempool_fallback_disabled, false,
      &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  _Alignas(cmempool__entry_align) uint8_t small_buf[sizeof(correct_buf) - 1];
  rmp = crmempool_create_from_preallocated_buffer(
      small_buf, sizeof(small_buf), 4, 5, 4, crmempool_fallback_disabled, false,
      &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  _Alignas(cmempool__entry_align) uint8_t large_buf[sizeof(correct_buf) + 1];
  rmp = crmempool_create_from_preallocated_buffer(
      large_buf, sizeof(large_buf), 4, 5, 4, crmempool_fallback_disabled, false,
      &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  rmp = crmempool_create_from_preallocated_buffer(
      correct_buf, sizeof(correct_buf), 5, 4, 4, crmempool_fallback_disabled,
      false, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(r_mempools, create_preallocated_buffer_fails_misaligned) {
  _Alignas(cmempool__entry_align) uint8_t buf[897];
  char *err = NULL;
  SCOPED_R_MEMPOOL(rmp) = crmempool_create_from_preallocated_buffer(
      buf + 1, sizeof(buf) - 1, 4, 5, 4, crmempool_fallback_disabled, false,
      &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(preallocated_r_mempools, exhaust_all_fallback_at_first_exhaustion) {
  static CRMEMPOOL_DECLARE_PREALLOCATED_BUFFER(buf, 4, 6, 7);

  SCOPED_R_MEMPOOL(rmp) = crmempool_create_from_preallocated_buffer(
      buf, sizeof(buf), 4, 6, 7, crmempool_fallback_at_first_exhaustion, false,
      NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *fill[224];
  size_t iter = 0;
  for (size_t i = 0; i < 128; ++i)
    fill[iter++] = crmempool_alloc_entry(rmp, 16);
  for (size_t i = 0; i < 64; ++i) fill[iter++] = crmempool_alloc_entry(rmp, 32);
  for (size_t i = 0; i < 32; ++i) fill[iter++] = crmempool_alloc_entry(rmp, 64);

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size),
               crmempool_total_capacity(rmp, size));
  }

  void *dyn16 = crmempool_alloc_entry(rmp, 16);
  REQUIRE_NE(dyn16, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);

  void *dyn32 = crmempool_alloc_entry(rmp, 32);
  REQUIRE_NE(dyn32, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 1);

  crmempool_free_entry(rmp, dyn16);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);

  crmempool_free_entry(rmp, dyn32);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 0);

  for (size_t i = 0; i < 224; ++i) crmempool_free_entry(rmp, fill[i]);

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size), 0);
  }

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(preallocated_r_mempools, exhaust_all_fallback_at_last_exhaustion) {
  static CRMEMPOOL_DECLARE_PREALLOCATED_BUFFER(buf, 4, 6, 7);

  SCOPED_R_MEMPOOL(rmp) = crmempool_create_from_preallocated_buffer(
      buf, sizeof(buf), 4, 6, 7, crmempool_fallback_at_last_exhaustion, false,
      NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *fill[224];
  size_t iter = 0;
  for (size_t i = 0; i < 128; ++i)
    fill[iter++] = crmempool_alloc_entry(rmp, 16);
  for (size_t i = 0; i < 64; ++i) fill[iter++] = crmempool_alloc_entry(rmp, 32);
  for (size_t i = 0; i < 32; ++i) fill[iter++] = crmempool_alloc_entry(rmp, 64);

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size),
               crmempool_total_capacity(rmp, size));
  }

  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 0);

  void *dyn16 = crmempool_alloc_entry(rmp, 16);
  REQUIRE_NE(dyn16, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 1);

  crmempool_free_entry(rmp, dyn16);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);

  for (size_t i = 0; i < 224; ++i) crmempool_free_entry(rmp, fill[i]);

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, query_functions_return_zero_for_invalid_sizes) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 7, crmempool_fallback_at_first_exhaustion, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  REQUIRE_EQ(crmempool_used_count(rmp, 0), 0);
  REQUIRE_EQ(crmempool_total_capacity(rmp, 0), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 0), 0);

  REQUIRE_EQ(crmempool_used_count(rmp, 65), 0);
  REQUIRE_EQ(crmempool_total_capacity(rmp, 65), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 65), 0);

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(cmempools, create_small_elem_size_is_bumped_to_min) {
  char *err = NULL;
  SCOPED_MEMPOOL(mp) = cmempool_create(8, 1, false, false, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)mp, NULL);
  REQUIRE_EQ(cmempool_total_capacity(mp), 8);
  REQUIRE_EQ(cmempool_used_count(mp), 0);

  void *ptrs[8];
  for (size_t i = 0; i < 8; ++i) {
    ptrs[i] = cmempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    REQUIRE_EQ(cmempool_used_count(mp), i + 1);
  }
  REQUIRE_EQ((void *)cmempool_alloc_entry(mp), NULL);

  for (size_t i = 0; i < 8; ++i) {
    cmempool_free_entry(mp, ptrs[i]);
  }
  REQUIRE_EQ(cmempool_used_count(mp), 0);

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(r_mempools, dynamic_allocs_count_always_zero_when_fallback_disabled) {
  crmempool *rmp =
      crmempool_create(4, 6, 7, crmempool_fallback_disabled, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 0);

  void *p = crmempool_alloc_entry(rmp, 16);
  REQUIRE_NE(p, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 0);

  crmempool_free_entry(rmp, p);

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, calloc_entry_zeroes_fallback_at_first_exhaustion) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 7, crmempool_fallback_at_first_exhaustion, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *fill[128];
  for (size_t i = 0; i < 128; ++i) {
    fill[i] = crmempool_alloc_entry(rmp, 16);
    REQUIRE_NE(fill[i], NULL);
    memset(fill[i], 0xFF, 16);
  }
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 128);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);

  char *p = crmempool_calloc_entry(rmp, 16);
  REQUIRE_NE((void *)p, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);
  for (size_t i = 0; i < 16; ++i) {
    REQUIRE_EQ(p[i], 0);
  }

  crmempool_free_entry(rmp, p);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);

  for (size_t i = 0; i < 128; ++i) {
    crmempool_free_entry(rmp, fill[i]);
  }
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, calloc_entry_zeroes_fallback_at_last_exhaustion) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 7, crmempool_fallback_at_last_exhaustion, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *fill[224];
  size_t iter = 0;
  for (size_t i = 0; i < 128; ++i)
    fill[iter++] = crmempool_alloc_entry(rmp, 16);
  for (size_t i = 0; i < 64; ++i) fill[iter++] = crmempool_alloc_entry(rmp, 32);
  for (size_t i = 0; i < 32; ++i) fill[iter++] = crmempool_alloc_entry(rmp, 64);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 0);

  char *p = crmempool_calloc_entry(rmp, 32);
  REQUIRE_NE((void *)p, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 1);
  for (size_t i = 0; i < 32; ++i) {
    REQUIRE_EQ(p[i], 0);
  }

  crmempool_free_entry(rmp, p);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 0);

  for (size_t i = 0; i < 224; ++i) {
    crmempool_free_entry(rmp, fill[i]);
  }

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, try_exhausting_with_fallback_at_first_exhaustion_no_locks) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 7, crmempool_fallback_at_first_exhaustion, true, NULL);

  size_t ptrs_len = 224;

  void *ptrs[ptrs_len];
  uint ptr_iterator = 0;

  for (size_t i = 0; i < 128; ++i) {
    ptrs[ptr_iterator++] = crmempool_alloc_entry(rmp, 16);
  }

  for (size_t i = 0; i < 64; ++i) {
    ptrs[ptr_iterator++] = crmempool_alloc_entry(rmp, 32);
  }

  for (size_t i = 0; i < 32; ++i) {
    ptrs[ptr_iterator++] = crmempool_alloc_entry(rmp, 64);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size),
               crmempool_total_capacity(rmp, size));
  }

  void *tmp_ptr_16 = crmempool_alloc_entry(rmp, 16);
  REQUIRE_NE((void *)tmp_ptr_16, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);

  void *tmp_ptr_32 = crmempool_alloc_entry(rmp, 32);
  REQUIRE_NE((void *)tmp_ptr_32, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 1);

  void *tmp_ptr_64 = crmempool_alloc_entry(rmp, 64);
  REQUIRE_NE((void *)tmp_ptr_64, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 1);

  for (size_t i = 0; i < ptrs_len; ++i) {
    crmempool_free_entry(rmp, ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size), 0);
  }

  crmempool_free_entry(rmp, tmp_ptr_16);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);

  crmempool_free_entry(rmp, tmp_ptr_32);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 0);

  crmempool_free_entry(rmp, tmp_ptr_64);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 0);

  crmempool_destroy(rmp);
}

TEST(r_mempools, try_exhausting_with_fallback_at_last_exhaustion_no_locks) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 7, crmempool_fallback_at_last_exhaustion, true, NULL);

  size_t ptrs_len = 224;

  void *ptrs[ptrs_len];
  uint ptr_iterator = 0;

  for (size_t i = 0; i < 128; ++i) {
    ptrs[ptr_iterator++] = crmempool_alloc_entry(rmp, 16);
  }

  for (size_t i = 0; i < 64; ++i) {
    ptrs[ptr_iterator++] = crmempool_alloc_entry(rmp, 32);
  }

  for (size_t i = 0; i < 32; ++i) {
    ptrs[ptr_iterator++] = crmempool_alloc_entry(rmp, 64);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size),
               crmempool_total_capacity(rmp, size));
  }

  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 0);

  void *tmp_ptr_16 = crmempool_alloc_entry(rmp, 16);
  REQUIRE_NE((void *)tmp_ptr_16, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 1);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 1);

  void *tmp_ptr_32 = crmempool_alloc_entry(rmp, 32);
  REQUIRE_NE((void *)tmp_ptr_32, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 2);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 2);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 64), 2);

  for (size_t i = 0; i < ptrs_len; ++i) {
    crmempool_free_entry(rmp, ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(crmempool_used_count(rmp, size), 0);
  }

  crmempool_free_entry(rmp, tmp_ptr_16);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);
  crmempool_free_entry(rmp, tmp_ptr_32);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);

  crmempool_destroy(rmp);
}

TEST(preallocated_r_mempools, custom_allocator_propagated) {
  ccol_memmgmt_procs m_procs = {
      .malloc = malloc, .calloc = calloc, .realloc = realloc, .free = free};

  static CRMEMPOOL_DECLARE_PREALLOCATED_BUFFER(buf, 4, 6, 7);

  SCOPED_R_MEMPOOL(rmp) = crmempool_create_from_preallocated_buffer_mp(
      buf, sizeof(buf), 4, 6, 7, crmempool_fallback_at_first_exhaustion, false,
      &m_procs, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *fill[128];
  for (size_t i = 0; i < 128; ++i) {
    fill[i] = crmempool_alloc_entry(rmp, 16);
    REQUIRE_NE(fill[i], NULL);
  }
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 128);

  void *dyn = crmempool_alloc_entry(rmp, 16);
  REQUIRE_NE(dyn, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 1);

  crmempool_free_entry(rmp, dyn);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);

  for (size_t i = 0; i < 128; ++i) {
    crmempool_free_entry(rmp, fill[i]);
  }
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools,
     fallback_at_last_exhaustion_escalation_not_counted_as_fallback) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 7, crmempool_fallback_at_last_exhaustion, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *fill[128];
  for (size_t i = 0; i < 128; ++i) {
    fill[i] = crmempool_alloc_entry(rmp, 16);
    REQUIRE_NE(fill[i], NULL);
  }
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 128);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);

  void *escalated = crmempool_alloc_entry(rmp, 8);
  REQUIRE_NE(escalated, NULL);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 1);

  crmempool_free_entry(rmp, escalated);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);

  for (size_t i = 0; i < 128; ++i) crmempool_free_entry(rmp, fill[i]);
  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

// A rejected triple can't go into the buffer macro (the binary wouldn't
// compile), so the guard is called directly and compared with cases the run
// time accepts or rejects.
TEST(preallocated_r_mempools, buffer_params_fit_matches_known_outcomes) {
#if SIZE_MAX > 0xFFFFFFFFu
  REQUIRE_FALSE(crmempool__buffer_params_fit(4, 63, 62));
#else
  REQUIRE_FALSE(crmempool__buffer_params_fit(4, 31, 30));
#endif

  REQUIRE_FALSE(crmempool__buffer_params_fit(4, 6, 1));

  REQUIRE_FALSE(crmempool__buffer_params_fit(8, 4, 10));
  REQUIRE_FALSE(crmempool__buffer_params_fit(6, 6, 10));

  REQUIRE_TRUE(crmempool__buffer_params_fit(4, 6, 7));
  REQUIRE_TRUE(crmempool__buffer_params_fit(4, 16, 12));
  REQUIRE_TRUE(crmempool__buffer_params_fit(4, 5, 4));

#if SIZE_MAX > 0xFFFFFFFFu
  REQUIRE_TRUE(crmempool__buffer_params_fit(4, 11, 56));
#else
  REQUIRE_TRUE(crmempool__buffer_params_fit(4, 11, 24));
#endif

#if SIZE_MAX > 0xFFFFFFFFu
  REQUIRE_TRUE(crmempool__buffer_params_fit(4, 33, 30));
#else
  REQUIRE_FALSE(crmempool__buffer_params_fit(4, 33, 30));
#endif

  REQUIRE_TRUE(crmempool__buffer_params_fit(4, 6, 2));
}

TEST(preallocated_r_mempools,
     declare_with_static_prefix_at_minimum_sc_boundary) {
  static CRMEMPOOL_DECLARE_PREALLOCATED_BUFFER(min_sc_buf, 4, 6, 2);
  SCOPED_R_MEMPOOL(rmp) = crmempool_create_from_preallocated_buffer(
      min_sc_buf, sizeof(min_sc_buf), 4, 6, 2, crmempool_fallback_disabled,
      false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *p = crmempool_alloc_entry(rmp, 64);
  REQUIRE_NE(p, NULL);
  REQUIRE_EQ(crmempool_used_count(rmp, 64), 1);
  crmempool_free_entry(rmp, p);

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

// Concurrency stress tests. Each worker records failure in its own argument
// struct, and only the main thread asserts, after every join. MP_STRESS_* are
// the threads and iterations of the plain pool stress.
#define MP_STRESS_THREADS 8
#define MP_STRESS_ITERS 2000

// Arguments and result of mp_stress_worker().
typedef struct {
  cmempool *mp;
  int thread_idx;
  bool ok;
} mp_stress_arg_t;

// Allocates, writes a marker, reads it back and frees MP_STRESS_ITERS times.
// A slot handed to two threads shows up as a clobbered marker; a NULL means
// the pool (with fallback) failed. Sets a->ok.
static void *mp_stress_worker(void *arg) {
  mp_stress_arg_t *a = (mp_stress_arg_t *)arg;
  a->ok = true;
  for (int i = 0; i < MP_STRESS_ITERS; ++i) {
    int64_t *slot = (int64_t *)cmempool_alloc_entry(a->mp);
    if (!slot) {
      a->ok = false;
      break;
    }
    int64_t marker = (int64_t)a->thread_idx * 1000000 + i;
    *slot = marker;
    if (*slot != marker) {
      a->ok = false;
      cmempool_free_entry(a->mp, slot);
      break;
    }
    cmempool_free_entry(a->mp, slot);
  }
  return NULL;
}

TEST(cmempools, concurrent_alloc_free_stress) {
  cmempool *mp = cmempool_create(64, sizeof(int64_t), true, false, NULL);
  REQUIRE_NE((void *)mp, NULL);

  pthread_t tids[MP_STRESS_THREADS];
  mp_stress_arg_t args[MP_STRESS_THREADS];
  int started = 0;
  for (int i = 0; i < MP_STRESS_THREADS; ++i) {
    args[i].mp = mp;
    args[i].thread_idx = i;
    args[i].ok = false;
    if (pthread_create(&tids[i], NULL, mp_stress_worker, &args[i]) != 0) break;
    started++;
  }
  int join_rv = 0;
  for (int i = 0; i < started; ++i) {
    if (pthread_join(tids[i], NULL) != 0) join_rv = -1;
  }
  REQUIRE_EQ(started, MP_STRESS_THREADS);
  REQUIRE_EQ(join_rv, 0);
  for (int i = 0; i < MP_STRESS_THREADS; ++i) {
    REQUIRE_TRUE(args[i].ok);
  }

  REQUIRE_EQ(cmempool_used_count(mp), 0);
  REQUIRE_EQ(cmempool_dynamic_allocs_count(mp), 0);
  REQUIRE_EQ(cmempool_total_capacity(mp), 64);

  cmempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

// Threads and iterations of the ranged pool stress.
#define RMP_STRESS_THREADS 8
#define RMP_STRESS_ITERS 1500

// Arguments and result of rmp_stress_worker().
typedef struct {
  crmempool *rmp;
  int thread_idx;
  bool ok;
} rmp_stress_arg_t;

// The ranged form of mp_stress_worker(), cycling through the 16, 32 and 64
// byte tiers.
static void *rmp_stress_worker(void *arg) {
  rmp_stress_arg_t *a = (rmp_stress_arg_t *)arg;
  static const size_t sizes[] = {8, 20, 50};
  a->ok = true;
  for (int i = 0; i < RMP_STRESS_ITERS; ++i) {
    size_t size = sizes[i % 3];
    int64_t *p = (int64_t *)crmempool_alloc_entry(a->rmp, size);
    if (!p) {
      a->ok = false;
      break;
    }
    int64_t marker = (int64_t)a->thread_idx * 1000000 + i;
    *p = marker;
    if (*p != marker) {
      a->ok = false;
      crmempool_free_entry(a->rmp, p);
      break;
    }
    crmempool_free_entry(a->rmp, p);
  }
  return NULL;
}

TEST(r_mempools, concurrent_alloc_free_stress) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 4, crmempool_fallback_at_last_exhaustion, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  pthread_t tids[RMP_STRESS_THREADS];
  rmp_stress_arg_t args[RMP_STRESS_THREADS];
  int started = 0;
  for (int i = 0; i < RMP_STRESS_THREADS; ++i) {
    args[i].rmp = rmp;
    args[i].thread_idx = i;
    args[i].ok = false;
    if (pthread_create(&tids[i], NULL, rmp_stress_worker, &args[i]) != 0) break;
    started++;
  }
  int join_rv = 0;
  for (int i = 0; i < started; ++i) {
    if (pthread_join(tids[i], NULL) != 0) join_rv = -1;
  }
  REQUIRE_EQ(started, RMP_STRESS_THREADS);
  REQUIRE_EQ(join_rv, 0);
  for (int i = 0; i < RMP_STRESS_THREADS; ++i) {
    REQUIRE_TRUE(args[i].ok);
  }

  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 64), 0);
  REQUIRE_EQ(crmempool_dynamic_allocs_count(rmp, 16), 0);

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

// Threads and iterations of the ranged realloc stress.
#define RMP_REALLOC_STRESS_THREADS 6
#define RMP_REALLOC_STRESS_ITERS 800

// Arguments and result of rmp_realloc_stress_worker().
typedef struct {
  crmempool *rmp;
  int thread_idx;
  bool ok;
} rmp_realloc_stress_arg_t;

// Allocates, reallocates across tiers and frees its own entries, checking
// their contents. Each thread touches only its own pointers; sharing one
// pointer between threads isn't supported by any allocator.
static void *rmp_realloc_stress_worker(void *arg) {
  rmp_realloc_stress_arg_t *a = (rmp_realloc_stress_arg_t *)arg;
  a->ok = true;
  for (int i = 0; i < RMP_REALLOC_STRESS_ITERS; ++i) {
    size_t size = 8 + (size_t)(i % 3) * 16;
    char *p = (char *)crmempool_alloc_entry(a->rmp, size);
    if (!p) {
      a->ok = false;
      break;
    }
    p[0] = (char)(a->thread_idx & 0x7f);

    size_t grown_size = 8 + (size_t)((i + 1) % 3) * 16;
    p = (char *)crmempool_realloc_entry(a->rmp, p, grown_size);
    if (!p) {
      a->ok = false;
      break;
    }
    if (p[0] != (char)(a->thread_idx & 0x7f)) {
      a->ok = false;
      crmempool_free_entry(a->rmp, p);
      break;
    }
    crmempool_free_entry(a->rmp, p);
  }
  return NULL;
}

TEST(r_mempools, concurrent_realloc_stress) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 4, crmempool_fallback_at_last_exhaustion, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  pthread_t tids[RMP_REALLOC_STRESS_THREADS];
  rmp_realloc_stress_arg_t args[RMP_REALLOC_STRESS_THREADS];
  int started = 0;
  for (int i = 0; i < RMP_REALLOC_STRESS_THREADS; ++i) {
    args[i].rmp = rmp;
    args[i].thread_idx = i;
    args[i].ok = false;
    if (pthread_create(&tids[i], NULL, rmp_realloc_stress_worker, &args[i]) !=
        0)
      break;
    started++;
  }
  int join_rv = 0;
  for (int i = 0; i < started; ++i) {
    if (pthread_join(tids[i], NULL) != 0) join_rv = -1;
  }
  REQUIRE_EQ(started, RMP_REALLOC_STRESS_THREADS);
  REQUIRE_EQ(join_rv, 0);
  for (int i = 0; i < RMP_REALLOC_STRESS_THREADS; ++i) {
    REQUIRE_TRUE(args[i].ok);
  }

  REQUIRE_EQ(crmempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(crmempool_used_count(rmp, 64), 0);

  crmempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

// The allocation-failure sweep: The sweep procs fail the nth allocation of
// any kind after _mp_arm(n), so every unwinding path of the constructors runs.
// g_mp_alloc_seen counts calls and g_mp_fail_at is the failing call (0 is
// off).
static _Atomic int g_mp_alloc_seen = 0;
static _Atomic int g_mp_fail_at = 0;

// Tells whether the current allocation is the one to fail, counting it.
static bool _mp_should_fail(void) {
  int at = atomic_load(&g_mp_fail_at);
  if (at == 0) return false;
  return (atomic_fetch_add(&g_mp_alloc_seen, 1) + 1) == at;
}
// The sweep procs: Each fails when _mp_should_fail() says so.
static void *_mp_sweep_malloc(size_t n) {
  return _mp_should_fail() ? NULL : malloc(n);
}
static void _mp_sweep_free(void *p) { free(p); }
static void *_mp_sweep_calloc(size_t a, size_t b) {
  return _mp_should_fail() ? NULL : calloc(a, b);
}
static void *_mp_sweep_realloc(void *p, size_t n) {
  return _mp_should_fail() ? NULL : realloc(p, n);
}
// The procs table of the sweep allocator.
static ccol_memmgmt_procs g_mp_sweep_procs = {
    _mp_sweep_malloc, _mp_sweep_free, _mp_sweep_calloc, _mp_sweep_realloc};

// Arms the sweep to fail the nth allocation from now.
static void _mp_arm(int nth) {
  atomic_store(&g_mp_alloc_seen, 0);
  atomic_store(&g_mp_fail_at, nth);
}
// Disarms the sweep.
static void _mp_disarm(void) { atomic_store(&g_mp_fail_at, 0); }

// How many allocation indices each sweep tries.
#define MP_SWEEP_DEPTH 12

TEST(cmempool_oom, fixed_pool_construction_unwinds_at_every_allocation) {
  bool all_handled = true;
  for (int n = 1; n <= MP_SWEEP_DEPTH; n++) {
    _mp_arm(n);
    char *err = NULL;
    SCOPED_MEMPOOL(mp) = cmempool_create_mp(32, sizeof(int), false, false,
                                            &g_mp_sweep_procs, &err);
    _mp_disarm();
    if (mp) {
      void *e = cmempool_alloc_entry(mp);
      if (!e) all_handled = false;
      if (e) cmempool_free_entry(mp, e);
      cmempool_destroy(mp);
    } else if (!err) {
      all_handled = false;
    }
  }
  REQUIRE_TRUE(all_handled);
}

TEST(cmempool_oom, fixed_pool_with_fallback_unwinds_at_every_allocation) {
  bool all_handled = true;
  for (int n = 1; n <= MP_SWEEP_DEPTH; n++) {
    _mp_arm(n);
    char *err = NULL;
    SCOPED_MEMPOOL(mp) = cmempool_create_mp(16, sizeof(long long), true, true,
                                            &g_mp_sweep_procs, &err);
    _mp_disarm();
    if (mp)
      cmempool_destroy(mp);
    else if (!err)
      all_handled = false;
  }
  REQUIRE_TRUE(all_handled);
}

TEST(cmempool_oom, preallocated_buffer_pool_unwinds_at_every_allocation) {
  bool all_handled = true;
  for (int n = 1; n <= MP_SWEEP_DEPTH; n++) {
    static unsigned char buf[4096];
    memset(buf, 0xA5, sizeof(buf));
    _mp_arm(n);
    char *err = NULL;
    SCOPED_MEMPOOL(mp) = cmempool_create_from_preallocated_buffer_mp(
        buf, sizeof(buf), 32, false, false, &g_mp_sweep_procs, &err);
    _mp_disarm();
    if (mp)
      cmempool_destroy(mp);
    else if (!err)
      all_handled = false;
  }
  REQUIRE_TRUE(all_handled);
}

TEST(cmempool_oom, ranged_pool_construction_unwinds_at_every_allocation) {
  bool all_handled = true;
  for (int n = 1; n <= 40; n++) {
    _mp_arm(n);
    char *err = NULL;
    SCOPED_R_MEMPOOL(rmp) = crmempool_create_mp(
        4, 8, 4, crmempool_fallback_disabled, false, &g_mp_sweep_procs, &err);
    _mp_disarm();
    if (rmp)
      crmempool_destroy(rmp);
    else if (!err)
      all_handled = false;
  }
  REQUIRE_TRUE(all_handled);
}

TEST(cmempool_oom, ranged_pool_with_fallback_unwinds_at_every_allocation) {
  bool all_handled = true;
  for (int n = 1; n <= 40; n++) {
    _mp_arm(n);
    char *err = NULL;
    crmempool *rmp =
        crmempool_create_mp(4, 7, 3, crmempool_fallback_at_last_exhaustion,
                            true, &g_mp_sweep_procs, &err);
    _mp_disarm();
    if (rmp)
      crmempool_destroy(rmp);
    else if (!err)
      all_handled = false;
  }
  REQUIRE_TRUE(all_handled);
}

TEST(cmempool_oom, a_pool_built_under_a_late_failure_still_serves_entries) {
  bool built = false, usable = false;
  for (int n = 20; n <= 80 && !built; n++) {
    _mp_arm(n);
    SCOPED_R_MEMPOOL(rmp) = crmempool_create_mp(
        4, 8, 4, crmempool_fallback_disabled, false, &g_mp_sweep_procs, NULL);
    _mp_disarm();
    if (rmp) {
      built = true;
      void *e = crmempool_alloc_entry(rmp, 16);
      usable = (e != NULL);
      if (e) crmempool_free_entry(rmp, e);
      crmempool_destroy(rmp);
    }
  }
  REQUIRE_TRUE(built);
  REQUIRE_TRUE(usable);
}

// Thread-cache tests: Their failures are silent (memory grows or the cache
// stops working), so they assert on the library's own counters. This is the
// pool size most of them use.
#define MP_CACHE_POOL_ELEMS 512

// Sleeps 1 us in every wait loop of these tests. A real sleep, because under
// valgrind sched_yield() doesn't hand the CPU to the thread being waited
// for.
static void mp_test_short_sleep(void) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000};
  nanosleep(&ts, NULL);
}

// A stress of the ranged owner lookup, which reads every tier's dynamic count
// without the tiers' locks while other threads change them. The tiers are
// tiny, so most requests take the fallback. ThreadSanitizer is where this
// pays off.
#define RMP_DYN_STRESS_THREADS 6
#define RMP_DYN_STRESS_ITERS 400

// Arguments and result of rmp_dyn_stress_worker().
typedef struct {
  crmempool *rmp;
  int idx;
  bool ok;
} rmp_dyn_stress_arg_t;

// The start gate of the dynamic stress: Workers count in, then wait for go,
// so thread creation doesn't compete with the work.
static atomic_int rmp_dyn_arrived;
static atomic_bool rmp_dyn_go;

// Waits at the gate, then allocates, reallocates and frees across the tiers,
// setting a->ok.
static void *rmp_dyn_stress_worker(void *arg) {
  rmp_dyn_stress_arg_t *a = (rmp_dyn_stress_arg_t *)arg;
  atomic_fetch_add(&rmp_dyn_arrived, 1);
  while (!atomic_load(&rmp_dyn_go)) {
    mp_test_short_sleep();
  }
  a->ok = true;
  for (int i = 0; i < RMP_DYN_STRESS_ITERS; ++i) {
    size_t size = 8 + (size_t)(i % 3) * 16;
    char *p = (char *)crmempool_alloc_entry(a->rmp, size);
    if (!p) {
      a->ok = false;
      break;
    }
    p[0] = (char)(a->idx & 0x7f);
    size_t next = 8 + (size_t)((i + 1) % 3) * 16;
    char *q = (char *)crmempool_realloc_entry(a->rmp, p, next);
    if (!q) {
      crmempool_free_entry(a->rmp, p);
      a->ok = false;
      break;
    }
    if (q[0] != (char)(a->idx & 0x7f)) a->ok = false;
    crmempool_free_entry(a->rmp, q);
    if (!a->ok) break;
  }
  return NULL;
}

TEST(r_mempools, concurrent_dynamic_fallback_churn) {
  SCOPED_R_MEMPOOL(rmp) = crmempool_create(
      4, 6, 2, crmempool_fallback_at_first_exhaustion, false, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  atomic_store(&rmp_dyn_arrived, 0);
  atomic_store(&rmp_dyn_go, false);

  pthread_t tids[RMP_DYN_STRESS_THREADS];
  rmp_dyn_stress_arg_t args[RMP_DYN_STRESS_THREADS];
  int started = 0;
  for (int i = 0; i < RMP_DYN_STRESS_THREADS; ++i) {
    args[i].rmp = rmp;
    args[i].idx = i;
    args[i].ok = false;
    if (pthread_create(&tids[i], NULL, rmp_dyn_stress_worker, &args[i]) != 0) {
      break;
    }
    started++;
  }
  while (atomic_load(&rmp_dyn_arrived) < started) {
    mp_test_short_sleep();
  }
  atomic_store(&rmp_dyn_go, true);

  int join_rv = 0;
  for (int i = 0; i < started; ++i) {
    if (pthread_join(tids[i], NULL) != 0) join_rv = -1;
  }
  bool all_ok = true;
  for (int i = 0; i < started; ++i) {
    if (!args[i].ok) all_ok = false;
  }
  size_t leaked_16 = crmempool_dynamic_allocs_count(rmp, 16);
  size_t leaked_32 = crmempool_dynamic_allocs_count(rmp, 32);
  size_t leaked_64 = crmempool_dynamic_allocs_count(rmp, 64);
  size_t used_16 = crmempool_used_count(rmp, 16);

  crmempool_destroy(rmp);

  REQUIRE_EQ(started, RMP_DYN_STRESS_THREADS);
  REQUIRE_EQ(join_rv, 0);
  REQUIRE_TRUE(all_ok);
  REQUIRE_EQ(leaked_16, 0);
  REQUIRE_EQ(leaked_32, 0);
  REQUIRE_EQ(leaked_64, 0);
  REQUIRE_EQ(used_16, 0);
}

// Arguments of the cache workers. filled and release are a counted handshake
// instead of a pthread_barrier, so a failed pthread_create can't leave the
// started threads parked forever.
typedef struct {
  cmempool *mp;
  size_t count;
  bool ok;
  _Atomic int *filled;
  _Atomic bool *release;
} mp_cache_arg_t;

// Allocates count entries, frees them and exits, so they sit in this thread's
// cache and must come back to the pool at the exit. Sets a->ok.
static void *mp_cache_fill_and_exit(void *arg) {
  mp_cache_arg_t *a = arg;
  void **p = calloc(a->count, sizeof(void *));
  if (!p) return NULL;
  bool ok = true;
  for (size_t i = 0; i < a->count; ++i) {
    p[i] = cmempool_alloc_entry(a->mp);
    if (!p[i]) ok = false;
  }
  for (size_t i = 0; i < a->count; ++i) {
    if (p[i]) cmempool_free_entry(a->mp, p[i]);
  }
  free(p);
  a->ok = ok;
  return NULL;
}

TEST(cmempools, thread_cache_is_returned_when_its_thread_exits) {
  SCOPED_MEMPOOL(mp) =
      cmempool_create(MP_CACHE_POOL_ELEMS, sizeof(int), false, false, NULL);
  REQUIRE_NE((void *)mp, NULL);

  mp_cache_arg_t arg = {.mp = mp, .count = 64, .ok = false};
  pthread_t th;
  int create_rv = pthread_create(&th, NULL, mp_cache_fill_and_exit, &arg);
  int join_rv = (create_rv == 0) ? pthread_join(th, NULL) : 0;

  size_t used_after_exit = cmempool_used_count(mp);
  size_t live_mags_after_exit = cmempool__live_magazines_for_tests(mp);

  void **all = calloc(MP_CACHE_POOL_ELEMS, sizeof(void *));
  bool all_ok = (all != NULL);
  size_t got = 0;
  if (all_ok) {
    for (size_t i = 0; i < MP_CACHE_POOL_ELEMS; ++i) {
      all[i] = cmempool_alloc_entry(mp);
      if (all[i]) got++;
    }
    for (size_t i = 0; i < MP_CACHE_POOL_ELEMS; ++i) {
      if (all[i]) cmempool_free_entry(mp, all[i]);
    }
    free(all);
  }

  cmempool_destroy(mp);

  REQUIRE_EQ(create_rv, 0);
  REQUIRE_EQ(join_rv, 0);
  REQUIRE_TRUE(arg.ok);
  REQUIRE_EQ(used_after_exit, (size_t)0);
  REQUIRE_EQ(live_mags_after_exit, (size_t)0);
  REQUIRE_TRUE(all_ok);
  REQUIRE_EQ(got, (size_t)MP_CACHE_POOL_ELEMS);
}

// Fills this thread's cache, counts itself in *a->filled (on every path, so
// the main thread's wait can't hang) and holds the cache until *a->release.
static void *mp_cache_hold(void *arg) {
  mp_cache_arg_t *a = arg;
  void **p = calloc(a->count, sizeof(void *));
  if (p) {
    for (size_t i = 0; i < a->count; ++i) {
      p[i] = cmempool_alloc_entry(a->mp);
    }
    for (size_t i = 0; i < a->count; ++i) {
      if (p[i]) cmempool_free_entry(a->mp, p[i]);
    }
  }
  atomic_fetch_add_explicit(a->filled, 1, memory_order_release);
  while (!atomic_load_explicit(a->release, memory_order_acquire))
    mp_test_short_sleep();
  free(p);
  return NULL;
}

// Entries parked in other threads' caches must never make the pool refuse a
// caller within the advertised capacity; that's what the reserve is for.
TEST(cmempools, advertised_capacity_is_reachable_despite_other_thread_caches) {
  SCOPED_MEMPOOL(mp) =
      cmempool_create(MP_CACHE_POOL_ELEMS, sizeof(int), false, false, NULL);
  REQUIRE_NE((void *)mp, NULL);

  enum { HOLDERS = 4 };
  _Atomic int filled = 0;
  _Atomic bool release = false;

  mp_cache_arg_t args[HOLDERS];
  pthread_t th[HOLDERS];
  int started = 0;
  for (int i = 0; i < HOLDERS; ++i) {
    args[i] = (mp_cache_arg_t){.mp = mp,
                               .count = 16,
                               .ok = false,
                               .filled = &filled,
                               .release = &release};
    if (pthread_create(&th[i], NULL, mp_cache_hold, &args[i]) != 0) break;
    started++;
  }

  size_t got = 0;
  void **all = calloc(MP_CACHE_POOL_ELEMS, sizeof(void *));
  if (started == HOLDERS && all) {
    while (atomic_load_explicit(&filled, memory_order_acquire) < started)
      mp_test_short_sleep();
    for (size_t i = 0; i < MP_CACHE_POOL_ELEMS; ++i) {
      all[i] = cmempool_alloc_entry(mp);
      if (all[i]) got++;
    }
  }
  atomic_store_explicit(&release, true, memory_order_release);

  for (int i = 0; i < started; ++i) pthread_join(th[i], NULL);
  if (all) {
    for (size_t i = 0; i < MP_CACHE_POOL_ELEMS; ++i) {
      if (all[i]) cmempool_free_entry(mp, all[i]);
    }
    free(all);
  }
  cmempool_destroy(mp);

  REQUIRE_EQ(started, HOLDERS);
  REQUIRE_EQ(got, (size_t)MP_CACHE_POOL_ELEMS);
}

// Without the slot coming back at thread exit, churn fills the magazine budget
// and the pool quietly locks on every call.
TEST(cmempools, magazine_slots_are_reclaimed_across_thread_churn) {
  SCOPED_MEMPOOL(mp) =
      cmempool_create(MP_CACHE_POOL_ELEMS, sizeof(int), false, false, NULL);
  REQUIRE_NE((void *)mp, NULL);

  bool all_ok = true;
  size_t peak = 0;
  for (int round = 0; round < 32; ++round) {
    mp_cache_arg_t arg = {.mp = mp, .count = 8, .ok = false};
    pthread_t th;
    if (pthread_create(&th, NULL, mp_cache_fill_and_exit, &arg) != 0) {
      all_ok = false;
      break;
    }
    pthread_join(th, NULL);
    if (!arg.ok) all_ok = false;
    size_t live = cmempool__live_magazines_for_tests(mp);
    if (live > peak) peak = live;
  }

  cmempool_destroy(mp);

  REQUIRE_TRUE(all_ok);
  REQUIRE_EQ(peak, (size_t)0);
}

// A counting allocator for the next test. A thread the pool refuses a
// magazine must not pay the caller's allocator for the refusal; building the
// magazine before asking makes the count climb to one per operation.
static atomic_size_t mp_deny_allocs;

// The counting procs: Every allocation bumps mp_deny_allocs.
static void *mp_deny_malloc(size_t size) {
  atomic_fetch_add(&mp_deny_allocs, 1);
  return malloc(size);
}

static void *mp_deny_calloc(size_t count, size_t size) {
  atomic_fetch_add(&mp_deny_allocs, 1);
  return calloc(count, size);
}

static void *mp_deny_realloc(void *ptr, size_t size) {
  atomic_fetch_add(&mp_deny_allocs, 1);
  return realloc(ptr, size);
}

static void mp_deny_free(void *ptr) { free(ptr); }

// Arguments of mp_deny_hold().
typedef struct {
  cmempool *mp;
  atomic_bool claimed;
  atomic_bool release;
  bool ok;
} mp_deny_arg_t;

// Takes the pool's only magazine and holds it until a->release, so the pool
// refuses the main thread every time. It sets claimed on every path.
static void *mp_deny_hold(void *arg) {
  mp_deny_arg_t *a = (mp_deny_arg_t *)arg;
  void *e = cmempool_alloc_entry(a->mp);
  a->ok = (e != NULL);
  if (e) cmempool_free_entry(a->mp, e);
  atomic_store(&a->claimed, true);
  while (!atomic_load(&a->release)) {
    mp_test_short_sleep();
  }
  return NULL;
}

TEST(cmempools, a_pool_with_no_magazine_to_give_does_not_allocate_per_call) {
  enum { OPS = 4096 };
  ccol_memmgmt_procs procs = {.malloc = mp_deny_malloc,
                              .calloc = mp_deny_calloc,
                              .realloc = mp_deny_realloc,
                              .free = mp_deny_free};
  cmempool *mp = cmempool_create_mp(8, sizeof(int), false, false, &procs, NULL);
  REQUIRE_NE((void *)mp, NULL);

  mp_deny_arg_t arg = {.mp = mp, .ok = false};
  atomic_init(&arg.claimed, false);
  atomic_init(&arg.release, false);

  pthread_t th;
  bool started = (pthread_create(&th, NULL, mp_deny_hold, &arg) == 0);
  size_t allocs_during_ops = 0;
  bool served = true;

  if (started) {
    while (!atomic_load(&arg.claimed)) {
      mp_test_short_sleep();
    }
    size_t before = atomic_load(&mp_deny_allocs);
    for (int i = 0; i < OPS; ++i) {
      void *e = cmempool_alloc_entry(mp);
      if (!e) {
        served = false;
        break;
      }
      cmempool_free_entry(mp, e);
    }
    allocs_during_ops = atomic_load(&mp_deny_allocs) - before;
    atomic_store(&arg.release, true);
    pthread_join(th, NULL);
  }

  bool holder_ok = arg.ok;
  cmempool_destroy(mp);

  REQUIRE_TRUE(started);
  REQUIRE_TRUE(holder_ok);
  REQUIRE_TRUE(served);
  REQUIRE_LE(allocs_during_ops, (size_t)1);
}

// Every pthread_mutex_lock of this binary goes through the wrapper below (the
// Makefile links with --wrap, or the wrapper interposes and finds the real one
// with dlsym). It counts locks per thread, so a test reads only what its own
// thread paid, and it notices thread B of the deadlock test going for the
// outer pool's lock.
#if defined(TEST_NO_LD_WRAP)
static int __real_pthread_mutex_lock(pthread_mutex_t *m) {
  static int (*_Atomic fn)(pthread_mutex_t *);
  if (!fn)
    fn = (int (*)(pthread_mutex_t *))dlsym(RTLD_NEXT, "pthread_mutex_lock");
  return fn(m);
}
#else
int __real_pthread_mutex_lock(pthread_mutex_t *m);
#endif
// The locks the calling thread has taken.
static __thread unsigned long mp_test_locks_taken;
// Set by a constructor after the first access to mp_test_locks_taken, because
// emulated TLS (OpenBSD) takes a mutex on that first access.
static atomic_bool mp_test_lock_count_ready;
// State the wrapper reads for the deadlock test: The outer pool sits at the
// start of one ARM_INNER_STRIDE entry of the inner pool, and thread B marks
// itself by its ID (its TLS is gone in a key destructor on macOS) before the
// wrapper watches for its lock on the outer pool.
#define ARM_INNER_STRIDE 4096
static cmempool *g_arm_outer, *g_arm_inner;
static atomic_bool g_arm_b_exiting;
static pthread_t g_arm_b_self;
static atomic_bool g_arm_b_wants_outer_lock;
// Makes the first access to the lock counter, then lets the wrapper count.
__attribute__((constructor)) static void mp_test_lock_count_arm(void) {
  mp_test_locks_taken = 0;
  atomic_store(&mp_test_lock_count_ready, true);
}
int __wrap_pthread_mutex_lock(pthread_mutex_t *m);
// Counts the lock for the calling thread, flags thread B's attempt on the
// outer pool's lock, and locks.
int __wrap_pthread_mutex_lock(pthread_mutex_t *m) {
  if (atomic_load_explicit(&mp_test_lock_count_ready, memory_order_relaxed))
    ++mp_test_locks_taken;
  if (atomic_load_explicit(&g_arm_b_exiting, memory_order_acquire) &&
      pthread_equal(pthread_self(), g_arm_b_self) &&
      (uintptr_t)m - (uintptr_t)g_arm_outer < ARM_INNER_STRIDE)
    atomic_store(&g_arm_b_wants_outer_lock, true);
  return __real_pthread_mutex_lock(m);
}
// Without --wrap, this definition takes every pthread_mutex_lock call of the
// binary and hands it to the counting wrapper.
#if defined(TEST_NO_LD_WRAP)
int pthread_mutex_lock(pthread_mutex_t *m) {
  return __wrap_pthread_mutex_lock(m);
}
#endif

// The budget question and the service share one critical section, so a
// refused thread pays one lock per call, like a pool with no cache, and gets a
// magazine as soon as one is free.
TEST(cmempools, a_refused_thread_takes_one_lock_per_operation) {
  enum { OPS = 1024, CACHED_OPS = 256 };
  cmempool *mp = cmempool_create(8, sizeof(int), false, false, NULL);
  REQUIRE_NE((void *)mp, NULL);

  mp_deny_arg_t arg = {.mp = mp, .ok = false};
  atomic_init(&arg.claimed, false);
  atomic_init(&arg.release, false);

  pthread_t th;
  bool started = (pthread_create(&th, NULL, mp_deny_hold, &arg) == 0);
  unsigned long refused_locks = 0;
  bool refused_served = true;
  unsigned long cached_locks = 0;
  bool cached_served = true;
  size_t magazines_after = 0;

  if (started) {
    while (!atomic_load(&arg.claimed)) {
      mp_test_short_sleep();
    }
    unsigned long before = mp_test_locks_taken;
    for (int i = 0; i < OPS; ++i) {
      void *e = cmempool_alloc_entry(mp);
      if (!e) {
        refused_served = false;
        break;
      }
      cmempool_free_entry(mp, e);
    }
    refused_locks = mp_test_locks_taken - before;

    atomic_store(&arg.release, true);
    pthread_join(th, NULL);

    void *warm = cmempool_alloc_entry(mp);
    if (warm) cmempool_free_entry(mp, warm);
    magazines_after = cmempool__live_magazines_for_tests(mp);
    before = mp_test_locks_taken;
    for (int i = 0; i < CACHED_OPS; ++i) {
      void *e = cmempool_alloc_entry(mp);
      if (!e) {
        cached_served = false;
        break;
      }
      cmempool_free_entry(mp, e);
    }
    cached_locks = mp_test_locks_taken - before;
  }

  bool holder_ok = arg.ok;
  cmempool_destroy(mp);

  REQUIRE_TRUE(started);
  REQUIRE_TRUE(holder_ok);
  REQUIRE_TRUE(refused_served);
  REQUIRE_EQ(refused_locks, (unsigned long)(2 * OPS));
  REQUIRE_EQ(magazines_after, (size_t)1);
  REQUIRE_TRUE(cached_served);
  REQUIRE_EQ(cached_locks, 0ul);
}

// Asking the locked path again after the refusal would cost a second lock for
// the same NULL, on every request that passes an exhausted tier.
TEST(cmempools, an_exhausted_cached_pool_refuses_with_one_lock) {
  enum { COUNT = 64, TRIES = 16 };
  cmempool *mp = cmempool_create(COUNT, sizeof(int), false, false, NULL);
  REQUIRE_NE((void *)mp, NULL);
  void *held[COUNT + 8];
  size_t n = 0;
  while (n < COUNT + 8) {
    void *e = cmempool_alloc_entry(mp);
    if (!e) break;
    held[n++] = e;
  }
  unsigned long before = mp_test_locks_taken;
  size_t refused = 0;
  for (int i = 0; i < TRIES; ++i) {
    if (!cmempool_alloc_entry(mp)) ++refused;
  }
  unsigned long locks = mp_test_locks_taken - before;
  for (size_t i = 0; i < n; ++i) cmempool_free_entry(mp, held[i]);
  cmempool_destroy(mp);

  REQUIRE_EQ(n, (size_t)COUNT);
  REQUIRE_EQ(refused, (size_t)TRIES);
  REQUIRE_EQ(locks, (unsigned long)TRIES);
}

// Arguments of mp_park_batch().
typedef struct {
  cmempool *mp;
  atomic_bool parked;
  atomic_bool release;
  bool ok;
} mp_park_arg_t;

// Allocates and frees one entry, which parks a refill batch in this thread's
// cache, then holds it until a->release.
static void *mp_park_batch(void *arg) {
  mp_park_arg_t *a = (mp_park_arg_t *)arg;
  void *e = cmempool_alloc_entry(a->mp);
  a->ok = (e != NULL);
  if (e) cmempool_free_entry(a->mp, e);
  atomic_store(&a->parked, true);
  while (!atomic_load(&a->release)) {
    mp_test_short_sleep();
  }
  return NULL;
}

// When the entries off the shared list reach the advertised count, a refill
// gives nothing while another thread caches the rest; such a request is
// served in the refill's critical section, one lock each.
TEST(cmempools, an_empty_refill_serves_the_request_in_the_same_lock) {
  enum { ELEMS = 64 };
  cmempool *mp = cmempool_create(ELEMS, sizeof(int), false, false, NULL);
  REQUIRE_NE((void *)mp, NULL);
  size_t reserve = cmempool__reserve_for_tests(mp);

  mp_park_arg_t arg = {.mp = mp, .ok = false};
  atomic_init(&arg.parked, false);
  atomic_init(&arg.release, false);

  void *held[ELEMS + 1] = {0};
  size_t got = 0;
  unsigned long max_locks_per_alloc = 0;
  size_t used_at_end = 0;

  pthread_t th;
  bool started = (pthread_create(&th, NULL, mp_park_batch, &arg) == 0);
  if (started) {
    while (!atomic_load(&arg.parked)) {
      mp_test_short_sleep();
    }
    held[got] = cmempool_alloc_entry(mp);
    if (held[got]) ++got;
    while (got <= ELEMS) {
      unsigned long before = mp_test_locks_taken;
      void *e = cmempool_alloc_entry(mp);
      if (!e) break;
      unsigned long taken = mp_test_locks_taken - before;
      if (taken > max_locks_per_alloc) max_locks_per_alloc = taken;
      held[got++] = e;
    }
    used_at_end = cmempool_used_count(mp);
    for (size_t i = 0; i < got; ++i) cmempool_free_entry(mp, held[i]);
    atomic_store(&arg.release, true);
    pthread_join(th, NULL);
  }

  bool parked_ok = arg.ok;
  cmempool_destroy(mp);

  REQUIRE_TRUE(started);
  REQUIRE_TRUE(parked_ok);
  REQUIRE_GT(reserve, (size_t)0);
  REQUIRE_EQ(got, (size_t)ELEMS);
  REQUIRE_EQ(used_at_end, (size_t)ELEMS);
  REQUIRE_EQ(max_locks_per_alloc, 1ul);
}

TEST(cmempools, orphaned_magazines_do_not_accumulate) {
  size_t before = cmempool__thread_magazines_for_tests();

  bool all_ok = true;
  for (int i = 0; i < 256; ++i) {
    SCOPED_MEMPOOL(mp) =
        cmempool_create(MP_CACHE_POOL_ELEMS, sizeof(int), false, false, NULL);
    if (!mp) {
      all_ok = false;
      break;
    }
    void *p = cmempool_alloc_entry(mp);
    if (p)
      cmempool_free_entry(mp, p);
    else
      all_ok = false;
    cmempool_destroy(mp);
    if (!all_ok) break;
  }

  size_t after = cmempool__thread_magazines_for_tests();
  REQUIRE_TRUE(all_ok);
  REQUIRE_LT(after, before + 4);
}

// A pool whose allocator is a second cached pool. A thread's exit drain frees
// the first pool's magazine into the second pool on the same thread, which
// builds a new magazine there; the drain must take it too, or it leaks and the
// second pool's magazine count stays up. Small blocks come from the backing
// pool, larger ones from malloc, tracked in mp_nested_big.
#define MP_NESTED_BACKING_ELEM 1024
#define MP_NESTED_MAX_BIG 8
static cmempool *mp_nested_backing;
static void *mp_nested_big[MP_NESTED_MAX_BIG];
static pthread_mutex_t mp_nested_big_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic size_t mp_nested_backing_allocs;

// The nested procs: Allocations up to MP_NESTED_BACKING_ELEM come from the
// backing pool (counted), larger ones from malloc; free sends each back where
// it came from; realloc is never used.
static void *mp_nested_malloc(size_t n) {
  if (n <= MP_NESTED_BACKING_ELEM) {
    atomic_fetch_add(&mp_nested_backing_allocs, 1);
    return cmempool_alloc_entry(mp_nested_backing);
  }
  void *p = malloc(n);
  if (!p) return NULL;
  pthread_mutex_lock(&mp_nested_big_lock);
  bool kept = false;
  for (size_t i = 0; i < MP_NESTED_MAX_BIG && !kept; ++i) {
    if (!mp_nested_big[i]) {
      mp_nested_big[i] = p;
      kept = true;
    }
  }
  pthread_mutex_unlock(&mp_nested_big_lock);
  if (!kept) {
    free(p);
    return NULL;
  }
  return p;
}

static void *mp_nested_calloc(size_t a, size_t b) {
  if (b != 0 && a > SIZE_MAX / b) return NULL;
  void *p = mp_nested_malloc(a * b);
  if (p) memset(p, 0, a * b);
  return p;
}

static void mp_nested_free(void *p) {
  if (!p) return;
  pthread_mutex_lock(&mp_nested_big_lock);
  bool big = false;
  for (size_t i = 0; i < MP_NESTED_MAX_BIG && !big; ++i) {
    if (mp_nested_big[i] == p) {
      mp_nested_big[i] = NULL;
      big = true;
    }
  }
  pthread_mutex_unlock(&mp_nested_big_lock);
  if (big)
    free(p);
  else
    cmempool_free_entry(mp_nested_backing, p);
}

static void *mp_nested_realloc(void *p, size_t n) {
  (void)p;
  (void)n;
  return NULL;
}

// Thread body: Allocates and frees one entry of the front pool and gives the
// pool back as a success marker.
static void *mp_nested_worker(void *arg) {
  cmempool *front = arg;
  void *e = cmempool_alloc_entry(front);
  if (!e) return NULL;
  cmempool_free_entry(front, e);
  return front;
}

TEST(cmempools, exit_drain_takes_magazines_that_a_nested_free_builds) {
  mp_nested_backing = cmempool_create(
      MP_CACHE_POOL_ELEMS, MP_NESTED_BACKING_ELEM, false, false, NULL);
  REQUIRE_NE((void *)mp_nested_backing, NULL);
  ccol_memmgmt_procs procs = {mp_nested_malloc, mp_nested_free,
                              mp_nested_calloc, mp_nested_realloc};
  cmempool *front =
      cmempool_create_mp(MP_CACHE_POOL_ELEMS, 32, false, false, &procs, NULL);
  if (!front) {
    cmempool_destroy(mp_nested_backing);
    REQUIRE_NE((void *)front, NULL);
  }

  size_t allocs_before = atomic_load(&mp_nested_backing_allocs);
  size_t backing_mags_before =
      cmempool__live_magazines_for_tests(mp_nested_backing);
  pthread_t th;
  void *served = NULL;
  int create_rv = pthread_create(&th, NULL, mp_nested_worker, front);
  if (create_rv == 0) pthread_join(th, &served);
  size_t allocs_in_thread =
      atomic_load(&mp_nested_backing_allocs) - allocs_before;
  size_t backing_mags = cmempool__live_magazines_for_tests(mp_nested_backing);
  size_t front_mags = cmempool__live_magazines_for_tests(front);

  cmempool_destroy(front);
  cmempool_destroy(mp_nested_backing);

  REQUIRE_EQ(create_rv, 0);
  REQUIRE_NE(served, NULL);
  REQUIRE_GE(allocs_in_thread, (size_t)1);
  REQUIRE_EQ(front_mags, (size_t)0);
  REQUIRE_EQ(backing_mags, backing_mags_before);
}

// The reap re-entry test: Pool X's allocator destroys pool Y and allocates
// from pool Q in the middle of a lookup walk, whose own reap then frees Y's
// magazine. Y's allocator zeroes and keeps freed blocks, so a walk that read on
// through Y's freed magazine would miss M's magazine. Every pool struct is
// 2048-byte aligned, so all pools share one direct-map slot and every lookup
// misses.
#define MP_REAP_ALIGN 2048
#define MP_REAP_MAX_PARKED 16
static void *mp_reap_parked[MP_REAP_MAX_PARKED];
static size_t mp_reap_parked_count;
static bool mp_reap_park_overflow;
static cmempool *mp_reap_y;
static cmempool *mp_reap_q;
static bool mp_reap_hook_armed;
static bool mp_reap_hook_ran;

// The reap procs: 2048-byte aligned blocks, calloc on top of them, no
// realloc, and a plain free.
static void *mp_reap_malloc(size_t n) {
  size_t rounded = (n + MP_REAP_ALIGN - 1) / MP_REAP_ALIGN * MP_REAP_ALIGN;
  return aligned_alloc(MP_REAP_ALIGN, rounded ? rounded : MP_REAP_ALIGN);
}

static void *mp_reap_calloc(size_t a, size_t b) {
  if (b != 0 && a > SIZE_MAX / b) return NULL;
  void *p = mp_reap_malloc(a * b);
  if (p) memset(p, 0, a * b);
  return p;
}

static void *mp_reap_realloc(void *p, size_t n) {
  (void)p;
  (void)n;
  return NULL;
}

static void mp_reap_free(void *p) { free(p); }

// Y's free: Zeroes the block (at least MP_REAP_ALIGN bytes) and keeps it
// instead of freeing it.
static void mp_reap_quarantine_free(void *p) {
  if (!p) return;
  memset(p, 0, MP_REAP_ALIGN);
  if (mp_reap_parked_count < MP_REAP_MAX_PARKED)
    mp_reap_parked[mp_reap_parked_count++] = p;
  else {
    mp_reap_park_overflow = true;
    free(p);
  }
}

// X's free: Once armed, destroys Y and allocates from Q, so the lookup walk is
// re-entered, then frees.
static void mp_reap_hook_free(void *p) {
  if (mp_reap_hook_armed) {
    mp_reap_hook_armed = false;
    mp_reap_hook_ran = true;
    cmempool_destroy(mp_reap_y);
    void *e = cmempool_alloc_entry(mp_reap_q);
    if (e) cmempool_free_entry(mp_reap_q, e);
  }
  free(p);
}

// Arguments and results of mp_reap_worker().
typedef struct {
  cmempool *m;
  cmempool *x;
  bool ok;
  size_t m_mags;
} mp_reap_arg_t;

// Allocates and frees one entry of mp, so this thread gets a magazine for it;
// clears *ok on failure.
static void touch_pool(cmempool *mp, bool *ok) {
  void *e = cmempool_alloc_entry(mp);
  if (e)
    cmempool_free_entry(mp, e);
  else
    *ok = false;
}

// Builds this thread's list as Y, X, M, Q, orphans X's magazine, arms the hook
// and touches M again, then records M's magazine count.
static void *mp_reap_worker(void *arg) {
  mp_reap_arg_t *a = arg;
  bool ok = true;
  touch_pool(mp_reap_q, &ok);
  touch_pool(a->m, &ok);
  touch_pool(a->x, &ok);
  touch_pool(mp_reap_y, &ok);
  cmempool_destroy(a->x);
  mp_reap_hook_armed = true;
  touch_pool(a->m, &ok);
  a->m_mags = cmempool__live_magazines_for_tests(a->m);
  a->ok = ok;
  return NULL;
}

// M must end with one magazine for this thread; a walk that kept going through
// a freed magazine builds a second one.
TEST(cmempools, a_reap_that_reenters_the_lookup_restarts_the_walk) {
  ccol_memmgmt_procs plain = {mp_reap_malloc, mp_reap_free, mp_reap_calloc,
                              mp_reap_realloc};
  ccol_memmgmt_procs quarantine = {mp_reap_malloc, mp_reap_quarantine_free,
                                   mp_reap_calloc, mp_reap_realloc};
  ccol_memmgmt_procs hook = {mp_reap_malloc, mp_reap_hook_free, mp_reap_calloc,
                             mp_reap_realloc};
  mp_reap_parked_count = 0;
  mp_reap_park_overflow = false;
  mp_reap_hook_armed = false;
  mp_reap_hook_ran = false;
  mp_reap_q = cmempool_create_mp(MP_CACHE_POOL_ELEMS, sizeof(int), false, false,
                                 &plain, NULL);
  cmempool *m = cmempool_create_mp(MP_CACHE_POOL_ELEMS, sizeof(int), false,
                                   false, &plain, NULL);
  cmempool *x = cmempool_create_mp(MP_CACHE_POOL_ELEMS, sizeof(int), false,
                                   false, &hook, NULL);
  mp_reap_y = cmempool_create_mp(MP_CACHE_POOL_ELEMS, sizeof(int), false, false,
                                 &quarantine, NULL);
  bool created = mp_reap_q && m && x && mp_reap_y;
  bool one_slot = created && (((uintptr_t)mp_reap_q >> 8) & 7) == 0 &&
                  (((uintptr_t)m >> 8) & 7) == 0 &&
                  (((uintptr_t)x >> 8) & 7) == 0 &&
                  (((uintptr_t)mp_reap_y >> 8) & 7) == 0;

  mp_reap_arg_t arg = {.m = m, .x = x, .ok = false, .m_mags = 0};
  int create_rv = -1;
  if (created) {
    pthread_t th;
    create_rv = pthread_create(&th, NULL, mp_reap_worker, &arg);
    if (create_rv == 0) pthread_join(th, NULL);
  }
  bool x_destroyed = created && create_rv == 0;
  bool y_destroyed = mp_reap_hook_ran;

  if (!x_destroyed && x) cmempool_destroy(x);
  if (!y_destroyed && mp_reap_y) cmempool_destroy(mp_reap_y);
  if (m) cmempool_destroy(m);
  if (mp_reap_q) cmempool_destroy(mp_reap_q);
  for (size_t i = 0; i < mp_reap_parked_count; ++i) free(mp_reap_parked[i]);
  mp_reap_parked_count = 0;

  REQUIRE_TRUE(created);
  REQUIRE_TRUE(one_slot);
  REQUIRE_EQ(create_rv, 0);
  REQUIRE_TRUE(arg.ok);
  REQUIRE_TRUE(y_destroyed);
  REQUIRE_FALSE(mp_reap_park_overflow);
  REQUIRE_EQ(arg.m_mags, (size_t)1);
}

// The overshoot measurement: Many threads race to empty a cached pool, and
// the total excess over the advertised count is reported every run but
// asserted only with CCOL_MP_BOUND_GATE=1, on a measured, quiet machine. The
// correct and wrong refill formulas overlap as soon as the machine is busy,
// and under valgrind both read zero. The capacity guarantee is always
// asserted.
#define MP_BOUND_THREADS 16
#define MP_BOUND_ELEMS 1024
#define MP_BOUND_PER_THREAD 4096

// Arguments and result of mp_bound_drain().
typedef struct {
  cmempool *mp;
  void **slots;
  size_t taken;
} mp_bound_arg_t;

// The start gate of the measurement: The main thread waits for the threads it
// really started, then releases them together.
static _Atomic int mp_bound_ready;
static _Atomic bool mp_bound_go;

// Waits at the gate, then allocates until the pool refuses or
// MP_BOUND_PER_THREAD is reached, keeping the entries in a->slots.
static void *mp_bound_drain(void *arg) {
  mp_bound_arg_t *a = (mp_bound_arg_t *)arg;
  atomic_fetch_add_explicit(&mp_bound_ready, 1, memory_order_release);
  while (!atomic_load_explicit(&mp_bound_go, memory_order_acquire))
    mp_test_short_sleep();
  size_t k = 0;
  for (; k < MP_BOUND_PER_THREAD; k++) {
    void *p = cmempool_alloc_entry(a->mp);
    if (!p) break;
    a->slots[k] = p;
  }
  a->taken = k;
  return NULL;
}

TEST(cmempools, concurrent_hand_out_stays_near_the_advertised_count) {
  enum { ROUNDS = 48 };
  size_t total_overshoot = 0;
  size_t reserve = 0;
  size_t least = (size_t)-1;
  bool fixtures_ok = true;
  bool pools_ok = true;
  int short_started = 0;

  for (int round = 0; round < ROUNDS && fixtures_ok && pools_ok; ++round) {
    cmempool *mp = cmempool_create(MP_BOUND_ELEMS, 32, false, false, NULL);
    if (!mp) {
      pools_ok = false;
      break;
    }
    reserve = cmempool__reserve_for_tests(mp);

    mp_bound_arg_t args[MP_BOUND_THREADS];
    pthread_t th[MP_BOUND_THREADS];
    for (int i = 0; i < MP_BOUND_THREADS; ++i) {
      args[i].mp = mp;
      args[i].taken = 0;
      args[i].slots = (void **)calloc(MP_BOUND_PER_THREAD, sizeof(void *));
      if (!args[i].slots) fixtures_ok = false;
    }

    size_t total = 0;
    int started = 0;
    if (fixtures_ok) {
      atomic_store_explicit(&mp_bound_ready, 0, memory_order_relaxed);
      atomic_store_explicit(&mp_bound_go, false, memory_order_relaxed);
      for (int i = 0; i < MP_BOUND_THREADS; ++i) {
        if (pthread_create(&th[i], NULL, mp_bound_drain, &args[i]) != 0) break;
        started++;
      }
      while (atomic_load_explicit(&mp_bound_ready, memory_order_acquire) <
             started)
        mp_test_short_sleep();
      atomic_store_explicit(&mp_bound_go, true, memory_order_release);
      for (int i = 0; i < started; ++i) pthread_join(th[i], NULL);
      for (int i = 0; i < started; ++i) total += args[i].taken;
    }
    if (started != MP_BOUND_THREADS) short_started++;
    if (total > (size_t)MP_BOUND_ELEMS)
      total_overshoot += total - (size_t)MP_BOUND_ELEMS;
    if (total < least) least = total;

    for (int i = 0; i < MP_BOUND_THREADS; ++i) {
      for (size_t k = 0; k < args[i].taken; ++k)
        cmempool_free_entry(mp, args[i].slots[k]);
      free(args[i].slots);
    }
    cmempool_destroy(mp);
  }

  REQUIRE_TRUE(pools_ok);
  REQUIRE_TRUE(fixtures_ok);
  REQUIRE_EQ(short_started, 0);
  REQUIRE_GT(reserve, (size_t)0);
  printf("[overshoot] total=%zu over %d rounds, reserve=%zu, least=%zu\n",
         total_overshoot, ROUNDS, reserve, least);
  const char *gate = getenv("CCOL_MP_BOUND_GATE");
  if (gate && gate[0] == '1') {
    REQUIRE_LT(total_overshoot,
               (size_t)ROUNDS * ((size_t)MP_BOUND_THREADS / 2));
  }
  REQUIRE_GE(least, (size_t)MP_BOUND_ELEMS);
}

TEST(cmempools, pools_without_a_thread_cache_carry_no_reserve) {
  cmempool *tiny = cmempool_create(4, sizeof(int), false, false, NULL);
  bool tiny_ok = (tiny != NULL);
  size_t tiny_reserve = 0, tiny_cap = 0;
  if (tiny_ok) {
    tiny_reserve = cmempool__reserve_for_tests(tiny);
    tiny_cap = cmempool_total_capacity(tiny);
    cmempool_destroy(tiny);
  }

  SCOPED_MEMPOOL(st) =
      cmempool_create(MP_CACHE_POOL_ELEMS, sizeof(int), false, true, NULL);
  bool st_ok = (st != NULL);
  size_t st_reserve = 0, st_cap = 0;
  if (st_ok) {
    st_reserve = cmempool__reserve_for_tests(st);
    st_cap = cmempool_total_capacity(st);
    cmempool_destroy(st);
  }

  REQUIRE_TRUE(tiny_ok);
  REQUIRE_EQ(tiny_reserve, (size_t)0);
  REQUIRE_EQ(tiny_cap, (size_t)4);
  REQUIRE_TRUE(st_ok);
  REQUIRE_EQ(st_reserve, (size_t)0);
  REQUIRE_EQ(st_cap, (size_t)MP_CACHE_POOL_ELEMS);
}

// An entry in a magazine stays marked free, which keeps its double free
// fatal.
TEST(cmempools, double_free_of_a_cached_entry_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    SCOPED_MEMPOOL(mp) =
        cmempool_create(MP_CACHE_POOL_ELEMS, sizeof(int), false, false, NULL);
    void *p = cmempool_alloc_entry(mp);
    void *saved = p;
    cmempool_free_entry(mp, p);
    cmempool__free_entry(mp, saved);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// An allocator that hands a destroyed pool's block back for the next request
// of the same size, so a new pool gets the old pool's address and a stale
// cache entry could match it. It records the size of every block it gives
// out.
#define MP_ABA_TRACKED 64
static void *mp_aba_ptrs[MP_ABA_TRACKED];
static size_t mp_aba_sizes[MP_ABA_TRACKED];
static void *mp_aba_block;
static size_t mp_aba_block_size;
static int mp_aba_reuses;
// Recycling only happens inside the test body. A destroyed pool's magazine
// comes back to this allocator at process exit, and a block kept then would
// be reported as leaked.
static bool mp_aba_recycling;

// Remembers the size of block p.
static void mp_aba_record(void *p, size_t n) {
  for (int i = 0; i < MP_ABA_TRACKED; ++i) {
    if (!mp_aba_ptrs[i]) {
      mp_aba_ptrs[i] = p;
      mp_aba_sizes[i] = n;
      return;
    }
  }
}
// Forgets block p and gives its size, or 0 when it's unknown.
static size_t mp_aba_forget(void *p) {
  for (int i = 0; i < MP_ABA_TRACKED; ++i) {
    if (mp_aba_ptrs[i] == p) {
      size_t n = mp_aba_sizes[i];
      mp_aba_ptrs[i] = NULL;
      return n;
    }
  }
  return 0;
}

// The recycling procs: malloc hands the kept block back for a request of its
// size, free keeps the last freed block while recycling, calloc and realloc
// build on them.
static void *mp_aba_malloc(size_t n) {
  if (mp_aba_recycling && mp_aba_block && n == mp_aba_block_size) {
    void *p = mp_aba_block;
    mp_aba_block = NULL;
    mp_aba_reuses++;
    mp_aba_record(p, n);
    return p;
  }
  void *p = malloc(n);
  if (p) mp_aba_record(p, n);
  return p;
}
static void mp_aba_free(void *p) {
  if (!p) return;
  size_t n = mp_aba_forget(p);
  if (mp_aba_recycling && n && !mp_aba_block) {
    mp_aba_block = p;
    mp_aba_block_size = n;
    return;
  }
  free(p);
}
static void *mp_aba_calloc(size_t n, size_t sz) {
  void *p = mp_aba_malloc(n * sz);
  if (p) memset(p, 0, n * sz);
  return p;
}
static void *mp_aba_realloc(void *p, size_t n) { return realloc(p, n); }

TEST(cmempools, pool_address_reuse_does_not_resurrect_a_stale_cache) {
  ccol_memmgmt_procs procs = {.malloc = mp_aba_malloc,
                              .free = mp_aba_free,
                              .calloc = mp_aba_calloc,
                              .realloc = mp_aba_realloc};
  mp_aba_block = NULL;
  mp_aba_block_size = 0;
  mp_aba_reuses = 0;
  memset(mp_aba_ptrs, 0, sizeof(mp_aba_ptrs));
  mp_aba_recycling = true;

  bool ok = true;
  for (int i = 0; i < 64; ++i) {
    SCOPED_MEMPOOL(mp) = cmempool_create_mp(MP_CACHE_POOL_ELEMS, sizeof(int),
                                            false, false, &procs, NULL);
    if (!mp) {
      ok = false;
      break;
    }
    void *p[8];
    for (int k = 0; k < 8; ++k) p[k] = cmempool_alloc_entry(mp);
    for (int k = 0; k < 8; ++k) {
      if (!p[k]) ok = false;
    }
    for (int k = 0; k < 8; ++k) {
      if (p[k]) cmempool_free_entry(mp, p[k]);
    }
    if (cmempool_used_count(mp) != 0) ok = false;
    if (cmempool_total_capacity(mp) != MP_CACHE_POOL_ELEMS) ok = false;
    void *fresh = cmempool_alloc_entry(mp);
    if (!fresh || cmempool_used_count(mp) != 1) ok = false;
    if (fresh) cmempool_free_entry(mp, fresh);
    cmempool_destroy(mp);
  }
  mp_aba_recycling = false;
  free(mp_aba_block);
  mp_aba_block = NULL;

  REQUIRE_TRUE(ok);
  REQUIRE_GT(mp_aba_reuses, 0);
}

// The ranged buffer predicate has the constructor's floor on the smallest
// size, so a shape the constructor refuses is refused at compile time.
TEST(cmempools, ranged_buffer_params_reject_an_element_size_below_the_floor) {
  REQUIRE_FALSE(crmempool__buffer_params_fit(3, 6, 10));
  REQUIRE_FALSE(crmempool__buffer_params_fit(0, 4, 4));
  REQUIRE_FALSE(crmempool__buffer_params_fit(3, 4, 1));

  REQUIRE_TRUE(crmempool__buffer_params_fit(4, 6, 2));
  REQUIRE_TRUE(crmempool__buffer_params_fit(4, 8, 10));
  REQUIRE_TRUE(crmempool__buffer_params_fit(5, 10, 12));

  REQUIRE_EQ((size_t)1 << 4, (size_t)16);
}

// Walks arrays backwards with F(a[--k]); a macro that evaluates its argument
// twice frees every second entry and leaves the used count at half.
TEST(cmempools, free_and_destroy_macros_evaluate_their_argument_once) {
  cmempool *mps[2] = {cmempool_create(8, 16, false, false, NULL),
                      cmempool_create(8, 16, false, false, NULL)};
  crmempool *rmps[2] = {
      crmempool_create(4, 8, 6, crmempool_fallback_disabled, false, NULL),
      crmempool_create(4, 8, 6, crmempool_fallback_disabled, false, NULL)};
  bool created = mps[0] && mps[1] && rmps[0] && rmps[1];
  void *e[4] = {NULL, NULL, NULL, NULL};
  void *re[4] = {NULL, NULL, NULL, NULL};
  size_t used_after = 0, r_used_after = 0;
  bool entries_cleared = true;
  if (created) {
    for (int i = 0; i < 4; i++) {
      e[i] = cmempool_alloc_entry(mps[0]);
      re[i] = crmempool_alloc_entry(rmps[0], 8);
    }
    int k = 4;
    while (k > 0) cmempool_free_entry(mps[0], e[--k]);
    k = 4;
    while (k > 0) crmempool_free_entry(rmps[0], re[--k]);
    used_after = cmempool_used_count(mps[0]);
    r_used_after = crmempool_used_count(rmps[0], 8);
    for (int i = 0; i < 4; i++)
      if (e[i] || re[i]) entries_cleared = false;
  }
  int k = 2;
  while (k > 0) cmempool_destroy(mps[--k]);
  k = 2;
  while (k > 0) crmempool_destroy(rmps[--k]);
  REQUIRE_TRUE(created);
  REQUIRE_EQ(used_after, (size_t)0);
  REQUIRE_EQ(r_used_after, (size_t)0);
  REQUIRE_TRUE(entries_cleared);
  REQUIRE_EQ((void *)mps[0], NULL);
  REQUIRE_EQ((void *)mps[1], NULL);
  REQUIRE_EQ((void *)rmps[0], NULL);
  REQUIRE_EQ((void *)rmps[1], NULL);
}

// A pool whose allocator is another pool, against a thread exit. A arms the
// inner pool from the outer pool's fallback, under the outer lock, while B's
// exit drain waits for that lock. These flags stage the meeting; g_arm_is_a
// marks thread A.
static atomic_int g_arm_in_outer_lock, g_arm_b_may_exit, g_arm_overlapped;
static __thread int g_arm_is_a;
// The entries B holds; atomic because the child's main thread polls them.
static void *_Atomic g_arm_held[8];

// The outer pool's allocator, drawing from the inner pool. A's first call runs
// under the outer pool's lock: It lets B exit, waits (bounded, 20 s) until B's
// drain goes for the outer lock, records whether they met, and only then arms
// the inner pool.
static void *arm_malloc(size_t s) {
  (void)s;
  if (g_arm_is_a && !atomic_load(&g_arm_in_outer_lock)) {
    atomic_store(&g_arm_in_outer_lock, 1);
    atomic_store(&g_arm_b_may_exit, 1);
    struct timespec now, end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    end.tv_sec += 20;
    do {
      if (atomic_load(&g_arm_b_wants_outer_lock)) break;
      usleep(1000);
      clock_gettime(CLOCK_MONOTONIC, &now);
    } while (now.tv_sec < end.tv_sec ||
             (now.tv_sec == end.tv_sec && now.tv_nsec < end.tv_nsec));
    atomic_store(&g_arm_overlapped, atomic_load(&g_arm_b_wants_outer_lock));
  }
  return cmempool_alloc_entry(g_arm_inner);
}
// The rest of the outer pool's procs, on top of the inner pool.
static void *arm_calloc(size_t n, size_t s) {
  void *p = arm_malloc(n * s);
  if (p) memset(p, 0, n * s);
  return p;
}
static void *arm_realloc(void *p, size_t s) {
  (void)p;
  (void)s;
  return NULL;
}
static void arm_free(void *p) {
  if (p) cmempool_free_entry(g_arm_inner, p);
}

// Thread B: Takes every entry of the outer pool through its cache, waits for
// the go and exits, which drains that cache.
static void *arm_thread_b(void *a) {
  (void)a;
  for (int i = 0; i < 8; i++)
    atomic_store(&g_arm_held[i], cmempool_alloc_entry(g_arm_outer));
  while (!atomic_load(&g_arm_b_may_exit)) usleep(1000);
  g_arm_b_self = pthread_self();
  atomic_store_explicit(&g_arm_b_exiting, true, memory_order_release);
  return NULL;
}

// Thread A: Its first allocation from the full outer pool takes the fallback,
// so the inner pool arms this thread with the outer lock held.
static void *arm_thread_a(void *a) {
  (void)a;
  g_arm_is_a = 1;
  void *e = cmempool_alloc_entry(g_arm_outer);
  return e;
}

// Without the separate arm lock, A (arming) and B (draining) would wait for
// each other forever and the parent would kill the child. The verdict 2 means
// the two never met, so nothing was tested.
TEST(cmempools, a_pool_drawing_from_a_pool_does_not_deadlock_a_thread_exit) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  pid_t pid = fork();
  if (pid == 0) {
    close(pfd[0]);
    char verdict = '0';
    g_arm_inner = cmempool_create(1024, ARM_INNER_STRIDE, false, false, NULL);
    ccol_memmgmt_procs procs = {arm_malloc, arm_free, arm_calloc, arm_realloc};
    g_arm_outer = cmempool_create_mp(8, 16, true, false, &procs, NULL);
    pthread_t b, ta;
    if (g_arm_inner && g_arm_outer &&
        pthread_create(&b, NULL, arm_thread_b, NULL) == 0) {
      while (!atomic_load(&g_arm_held[7])) usleep(1000);
      if (pthread_create(&ta, NULL, arm_thread_a, NULL) == 0) {
        pthread_join(ta, NULL);
        verdict = atomic_load(&g_arm_overlapped) ? '1' : '2';
      }
      atomic_store(&g_arm_b_may_exit, 1);
      pthread_join(b, NULL);
    }
    ssize_t w;
    do {
      w = write(pfd[1], &verdict, 1);
    } while (w < 0 && errno == EINTR);
    test_child_exit(0);
  }
  close(pfd[1]);
  char verdict = 0;
  if (pid > 0) {
    struct pollfd p = {.fd = pfd[0], .events = POLLIN};
    int pr;
    do {
      pr = poll(&p, 1, 60000);
    } while (pr < 0 && errno == EINTR);
    if (pr > 0) {
      ssize_t r;
      do {
        r = read(pfd[0], &verdict, 1);
      } while (r < 0 && errno == EINTR);
    } else {
      kill(pid, SIGKILL);
    }
  }
  close(pfd[0]);
  int status = 0;
  if (pid > 0) waitpid(pid, &status, 0);
  REQUIRE_NE(pid, -1);
  REQUIRE_EQ(verdict, '1');
}
