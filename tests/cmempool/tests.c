#include <cmempool.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <unistd.h>
TAU_MAIN()  // sets up Tau (+ main function)

// C_MEMPOOL TESTS

TEST(cmempools, create_fails) {
  char *err;
  mempool *mp = mempool_create(0, sizeof(int), false, false, NULL, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);

  mp = mempool_create(16, 0, false, false, NULL, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);

  ccol_memmgmt_procs_t m_procs = {
      .malloc = malloc, .calloc = calloc, .realloc = realloc, .free = NULL};
  mp = mempool_create(16, 0, false, false, &m_procs, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// Regression: the third case in create_fails above also passes elem_size=0,
// so mempool_create's own elem_size==0 check rejects the call before
// ccol_verify_memmgmt_procs is ever reached; the incomplete-procs
// validation path was never actually exercised by that test. These use
// otherwise fully valid size parameters so the memmgmt-procs check itself is
// the only thing that can cause rejection.
TEST(cmempools, create_rejects_incomplete_procs_with_valid_size_params) {
  ccol_memmgmt_procs_t m_procs = {
      .malloc = malloc, .calloc = calloc, .realloc = realloc, .free = NULL};
  char *err = NULL;
  mempool *mp = mempool_create(16, sizeof(int), false, false, &m_procs, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(cmempools,
     create_from_preallocated_rejects_incomplete_procs_with_valid_params) {
  _Alignas(__internal_entry_header) uint8_t buf[256];
  ccol_memmgmt_procs_t m_procs = {
      .malloc = NULL, .calloc = calloc, .realloc = realloc, .free = free};
  char *err = NULL;
  mempool *mp = mempool_create_from_preallocated_buffer(
      buf, sizeof(buf), 16, false, false, &m_procs, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(cmempools, create_rejects_elem_size_that_would_overflow_header_addition) {
  // elem_size within offsetof(__internal_entry_header, next) bytes of
  // SIZE_MAX makes extended_elem_size = elem_size + header_overhead wrap
  // around to a value smaller than the header itself. Before this was
  // rejected, every per-element header write in mempool_init_internal_scalars
  // landed outside the (tiny, wrapped-size) allocated buffer; a real,
  // reproducible heap buffer overflow. Must now fail cleanly instead.
  char *err = NULL;
  mempool *mp = mempool_create(2, SIZE_MAX - 8, false, true, NULL, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);

  err = NULL;
  mp = mempool_create(1, SIZE_MAX, false, true, NULL, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(
    cmempools,
    create_from_preallocated_rejects_elem_size_that_would_overflow_header_addition) {
  // Same overflow as create_rejects_elem_size_that_would_overflow_header_
  // addition, but reachable through the preallocated-buffer constructor with
  // an ordinary, small stack buffer. Before this was rejected, the wrapped
  // extended_elem_size made elem_count = buf_size / extended_elem_size come
  // out non-zero, and the header-initialization loop wrote far past the end
  // of this 256-byte stack array (a real, ASan-confirmed stack-buffer-
  // overflow).
  _Alignas(__internal_entry_header) uint8_t buf[256];
  memset(buf, 0, sizeof(buf));
  char *err = NULL;
  mempool *mp = mempool_create_from_preallocated_buffer(
      buf, sizeof(buf), SIZE_MAX - 8, false, true, NULL, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// Regression: mempool_create used to compute extended_elem_size as plain
// elem_size + header overhead, with no rounding to __internal_entry_
// header's own alignment requirement (sizeof(uintptr_t) on every mainstream
// platform this library targets). Whenever that sum was not already a
// multiple of the header's alignment (e.g. any elem_size that is >=
// sizeof(uintptr_t), so the separate "bump small sizes up to
// sizeof(uintptr_t)" rounding never kicks in, but itself not a multiple of
// sizeof(uintptr_t), such as elem_size=9 below, or a plain struct built
// only from smaller-than-word-size members, e.g. "struct { float x, y, z;
// }", 12 bytes on a typical 64-bit build), every entry past index 0 in
// the pool's contiguous backing buffer landed on a progressively
// misaligned address, one stride at a time. Every subsequent
// header->elem_status/pool_ptr/next read or write was then a dereference
// through a pointer that violates __internal_entry_header's own required
// alignment: undefined behavior per the C standard, invisible on
// x86/x86_64's alignment-tolerant load/store instructions (which is why
// this went unnoticed under ordinary testing), but a real fault risk on
// stricter-alignment architectures and exactly the class of defect
// -fsanitize=alignment exists to catch. mempool_create now rounds
// extended_elem_size itself up to __internal_entry_header's own alignment
// requirement, so every entry (not just the first) is correctly
// aligned regardless of elem_size.
TEST(cmempools, pool_entries_beyond_first_are_properly_aligned) {
  mempool *mp = mempool_create(8, 9, false, true, NULL, NULL);
  REQUIRE_NE((void *)mp, NULL);

  void *ptrs[8];
  for (size_t i = 0; i < 8; ++i) {
    ptrs[i] = mempool_alloc_entry(mp);
    REQUIRE_NE(ptrs[i], NULL);
    uintptr_t header_addr =
        (uintptr_t)ptrs[i] - offsetof(__internal_entry_header, next);
    REQUIRE_EQ(header_addr % _Alignof(__internal_entry_header), 0);
  }

  for (size_t i = 0; i < 8; ++i) {
    mempool_free_entry(ptrs[i]);
  }

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

// Regression: DECLARE_PREALLOCATED_MEMPOOL_BUFFER's own internal guard,
// _ccol_mempool_buffer_params_fit, now rejects elem_count == 0 outright (a
// genuinely invalid preallocated-pool configuration) instead of letting it
// silently produce a zero-length array (a GNU extension, not standard ISO
// C) the way the old, elem_count == 0-tolerant _Static_assert condition
// did. A genuinely rejected elem_count can't be fed into
// DECLARE_PREALLOCATED_MEMPOOL_BUFFER itself without failing to compile
// this whole test binary, so the guard's boolean logic is exercised
// directly here instead, mirroring the equivalent
// r_mempools.buffer_params_fit_matches_known_outcomes test for
// _ccol_rmempool_buffer_params_fit.
TEST(cmempools, buffer_params_fit_rejects_zero_elem_count) {
  REQUIRE_FALSE(_ccol_mempool_buffer_params_fit(0, 64));

  // Every (elem_count, elem_size) pair already used by a real
  // DECLARE_PREALLOCATED_MEMPOOL_BUFFER call site elsewhere in this file
  // must be accepted; if it weren't, this whole binary would already
  // have failed to compile. Re-asserted here directly so a regression that
  // narrows the guard shows up as an ordinary test failure rather than
  // only ever as a build break someone has to bisect.
  REQUIRE_TRUE(_ccol_mempool_buffer_params_fit(100, 1));
  REQUIRE_TRUE(_ccol_mempool_buffer_params_fit(1, 1));
  REQUIRE_TRUE(_ccol_mempool_buffer_params_fit(32768, 256));
  REQUIRE_TRUE(_ccol_mempool_buffer_params_fit(50, 9));
}

// Regression: _ccol_mempool_buffer_params_fit used to compute
// ccol_max(elem_size, sizeof(uintptr_t)) + offsetof(__internal_entry_header,
// next) as a plain addition BEFORE ever handing that sum to
// _ccol_mempool_align_up_fits, which only proves the align_up step's OWN
// addition is overflow-safe for whatever value it is given; it says nothing
// about whether that value itself is already the result of a silent wrap.
// For an elem_size within offsetof(__internal_entry_header, next) bytes of
// SIZE_MAX, that first addition wrapped to a small value before
// _ccol_mempool_align_up_fits ever saw it, so the whole macro (and, since
// DECLARE_PREALLOCATED_MEMPOOL_BUFFER's array-size expression repeats the
// identical unprotected addition, the buffer declaration itself) silently
// treated a pathological elem_size as fitting instead of rejecting it the
// way mempool_create()'s own equivalent, subtraction-only check already
// does for the exact same elem_size range. Every elem_size in this window
// must now be rejected regardless of elem_count, and the boundary value
// exactly one byte below the window must still be accepted.
TEST(cmempools, buffer_params_fit_rejects_elem_size_near_size_max) {
  size_t header_overhead = offsetof(__internal_entry_header, next);

  for (size_t back_off = 0; back_off < header_overhead; ++back_off) {
    size_t elem_size = SIZE_MAX - back_off;
    REQUIRE_FALSE(_ccol_mempool_buffer_params_fit(1, elem_size));
    REQUIRE_FALSE(_ccol_mempool_buffer_params_fit(100, elem_size));
  }

  // The largest elem_size mempool_create()'s own equivalent, overflow-safe
  // check would still accept: neither the first addition (elem_size +
  // header_overhead) nor align_up's own rounding addition overflows for
  // this value, so this must be accepted regardless of the bug this test
  // guards against, confirming the fix didn't over-reject.
  size_t align_overhead = _Alignof(__internal_entry_header) - 1;
  size_t boundary_elem_size = SIZE_MAX - header_overhead - align_overhead;
  REQUIRE_TRUE(_ccol_mempool_buffer_params_fit(1, boundary_elem_size));
}

TEST(cmempools, create_succeeds) {
  char *err;
  mempool *mp = mempool_create(256, sizeof(int), false, false, NULL, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)mp, NULL);

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);

  mp = mempool_create(256, sizeof(int), false, true, NULL, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)mp, NULL);

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);

  ccol_memmgmt_procs_t m_procs = {
      .malloc = malloc, .calloc = calloc, .realloc = realloc, .free = free};
  mp = mempool_create(256, sizeof(int), false, false, &m_procs, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)mp, NULL);

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);

  mp = mempool_create(256, sizeof(int), false, true, &m_procs, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)mp, NULL);

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, allocations_and_deallocations_fallback_disabled) {
  mempool *mp = mempool_create(256, sizeof(int), false, false, NULL, NULL);
  REQUIRE_NE((void *)mp, NULL);

  REQUIRE_EQ(mempool_used_count(mp), 0);
  REQUIRE_EQ(mempool_total_capacity(mp), 256);

  int *ptrs[256] = {0};

  // Allocate all buffers.
  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = mempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(mempool_used_count(mp), i + 1);
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Verify the failure to allocate further.
  int *tmp_ptr = mempool_alloc_entry(mp);
  REQUIRE_EQ((void *)tmp_ptr, NULL);

  // Deallocate all.
  for (size_t i = 0; i < 256; ++i) {
    mempool_free_entry(ptrs[i]);
    REQUIRE_EQ(mempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Reallocate all buffers.
  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = mempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(mempool_used_count(mp), i + 1);
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Verify the failure to allocate further again.
  tmp_ptr = mempool_alloc_entry(mp);
  REQUIRE_EQ((void *)tmp_ptr, NULL);

  // Deallocate all.
  for (size_t i = 0; i < 256; ++i) {
    mempool_free_entry(ptrs[i]);
    REQUIRE_EQ(mempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, allocations_and_deallocations_fallback_disabled_no_locks) {
  mempool *mp = mempool_create(256, sizeof(int), false, true, NULL, NULL);
  REQUIRE_NE((void *)mp, NULL);

  REQUIRE_EQ(mempool_used_count(mp), 0);
  REQUIRE_EQ(mempool_total_capacity(mp), 256);

  int *ptrs[256] = {0};

  // Allocate all buffers.
  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = mempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(mempool_used_count(mp), i + 1);
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Verify the failure to allocate further.
  int *tmp_ptr = mempool_alloc_entry(mp);
  REQUIRE_EQ((void *)tmp_ptr, NULL);

  // Deallocate all.
  for (size_t i = 0; i < 256; ++i) {
    mempool_free_entry(ptrs[i]);
    REQUIRE_EQ(mempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Reallocate all buffers.
  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = mempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(mempool_used_count(mp), i + 1);
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Verify the failure to allocate further again.
  tmp_ptr = mempool_alloc_entry(mp);
  REQUIRE_EQ((void *)tmp_ptr, NULL);

  // Deallocate all.
  for (size_t i = 0; i < 256; ++i) {
    mempool_free_entry(ptrs[i]);
    REQUIRE_EQ(mempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, allocations_and_deallocations_fallback_enabled) {
  mempool *mp = mempool_create(256, sizeof(int), true, false, NULL, NULL);
  REQUIRE_NE((void *)mp, NULL);

  REQUIRE_EQ(mempool_used_count(mp), 0);
  REQUIRE_EQ(mempool_total_capacity(mp), 256);

  int *ptrs[256] = {0};

  // Allocate all buffers.
  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = mempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(mempool_used_count(mp), i + 1);
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Verify the allocation of a buffer via fallback.
  int *tmp_ptr = mempool_alloc_entry(mp);
  REQUIRE_NE((void *)tmp_ptr, NULL);
  // Free the fallback buffer.
  mempool_free_entry(tmp_ptr);

  // Deallocate all.
  for (size_t i = 0; i < 256; ++i) {
    mempool_free_entry(ptrs[i]);
    REQUIRE_EQ(mempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Reallocate all buffers.
  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = mempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(mempool_used_count(mp), i + 1);
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Verify the allocation of a buffer via fallback again.
  tmp_ptr = mempool_alloc_entry(mp);
  REQUIRE_NE((void *)tmp_ptr, NULL);
  // Free the fallback buffer.
  mempool_free_entry(tmp_ptr);

  // Deallocate all.
  for (size_t i = 0; i < 256; ++i) {
    mempool_free_entry(ptrs[i]);
    REQUIRE_EQ(mempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools,
     allocations_and_deallocations_fallback_enabled_custom_mem_procs) {
  ccol_memmgmt_procs_t m_procs = {
      .malloc = malloc, .calloc = calloc, .realloc = realloc, .free = free};
  char *err;

  mempool *mp = mempool_create(256, sizeof(int), true, false, &m_procs, &err);
  REQUIRE_NE((void *)mp, NULL);

  REQUIRE_EQ(mempool_used_count(mp), 0);
  REQUIRE_EQ(mempool_total_capacity(mp), 256);

  int *ptrs[256] = {0};

  // Allocate all buffers.
  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = mempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(mempool_used_count(mp), i + 1);
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Verify the allocation of a buffer via fallback.
  int *tmp_ptr = mempool_alloc_entry(mp);
  REQUIRE_NE((void *)tmp_ptr, NULL);
  // Free the fallback buffer.
  mempool_free_entry(tmp_ptr);

  // Deallocate all.
  for (size_t i = 0; i < 256; ++i) {
    mempool_free_entry(ptrs[i]);
    REQUIRE_EQ(mempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Reallocate all buffers.
  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = mempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(mempool_used_count(mp), i + 1);
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Verify the allocation of a buffer via fallback again.
  tmp_ptr = mempool_alloc_entry(mp);
  REQUIRE_NE((void *)tmp_ptr, NULL);
  // Free the fallback buffer.
  mempool_free_entry(tmp_ptr);

  // Deallocate all.
  for (size_t i = 0; i < 256; ++i) {
    mempool_free_entry(ptrs[i]);
    REQUIRE_EQ(mempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, allocations_and_deallocations_fallback_enabled_no_locks) {
  mempool *mp = mempool_create(256, sizeof(int), true, true, NULL, NULL);
  REQUIRE_NE((void *)mp, NULL);

  REQUIRE_EQ(mempool_used_count(mp), 0);
  REQUIRE_EQ(mempool_total_capacity(mp), 256);

  int *ptrs[256] = {0};

  // Allocate all buffers.
  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = mempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(mempool_used_count(mp), i + 1);
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Verify the allocation of a buffer via fallback.
  int *tmp_ptr = mempool_alloc_entry(mp);
  REQUIRE_NE((void *)tmp_ptr, NULL);
  // Free the fallback buffer.
  mempool_free_entry(tmp_ptr);

  // Deallocate all.
  for (size_t i = 0; i < 256; ++i) {
    mempool_free_entry(ptrs[i]);
    REQUIRE_EQ(mempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Reallocate all buffers.
  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = mempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(mempool_used_count(mp), i + 1);
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Verify the allocation of a buffer via fallback again.
  tmp_ptr = mempool_alloc_entry(mp);
  REQUIRE_NE((void *)tmp_ptr, NULL);
  // Free the fallback buffer.
  mempool_free_entry(tmp_ptr);

  // Deallocate all.
  for (size_t i = 0; i < 256; ++i) {
    mempool_free_entry(ptrs[i]);
    REQUIRE_EQ(mempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, c_allocations_and_deallocations_fallback_disabled) {
  mempool *mp = mempool_create(256, sizeof(int), false, false, NULL, NULL);
  REQUIRE_NE((void *)mp, NULL);

  REQUIRE_EQ(mempool_used_count(mp), 0);
  REQUIRE_EQ(mempool_total_capacity(mp), 256);

  int *ptrs[256] = {0};

  // Allocate all buffers.
  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = mempool_calloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(mempool_used_count(mp), i + 1);
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Verify the failure to allocate further.
  int *tmp_ptr = mempool_calloc_entry(mp);
  REQUIRE_EQ((void *)tmp_ptr, NULL);

  // Deallocate all.
  for (size_t i = 0; i < 256; ++i) {
    mempool_free_entry(ptrs[i]);
    REQUIRE_EQ(mempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Reallocate all buffers.
  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = mempool_calloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(mempool_used_count(mp), i + 1);
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Verify the failure to allocate further again.
  tmp_ptr = mempool_calloc_entry(mp);
  REQUIRE_EQ((void *)tmp_ptr, NULL);

  // Deallocate all.
  for (size_t i = 0; i < 256; ++i) {
    mempool_free_entry(ptrs[i]);
    REQUIRE_EQ(mempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, c_allocations_and_deallocations_fallback_enabled) {
  mempool *mp = mempool_create(256, sizeof(int), true, false, NULL, NULL);
  REQUIRE_NE((void *)mp, NULL);

  REQUIRE_EQ(mempool_used_count(mp), 0);
  REQUIRE_EQ(mempool_total_capacity(mp), 256);

  int *ptrs[256] = {0};

  // Allocate all buffers.
  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = mempool_calloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(mempool_used_count(mp), i + 1);
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Verify the allocation of a buffer via fallback.
  int *tmp_ptr = mempool_calloc_entry(mp);
  REQUIRE_NE((void *)tmp_ptr, NULL);
  // Free the fallback buffer.
  mempool_free_entry(tmp_ptr);

  // Deallocate all.
  for (size_t i = 0; i < 256; ++i) {
    mempool_free_entry(ptrs[i]);
    REQUIRE_EQ(mempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Reallocate all buffers.
  for (size_t i = 0; i < 256; ++i) {
    ptrs[i] = mempool_calloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    *ptrs[i] = i;
    REQUIRE_EQ(mempool_used_count(mp), i + 1);
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  // Verify the allocation of a buffer via fallback again.
  tmp_ptr = mempool_calloc_entry(mp);
  REQUIRE_NE((void *)tmp_ptr, NULL);
  // Free the fallback buffer.
  mempool_free_entry(tmp_ptr);

  // Deallocate all.
  for (size_t i = 0; i < 256; ++i) {
    mempool_free_entry(ptrs[i]);
    REQUIRE_EQ(mempool_used_count(mp), 256 - (i + 1));
    REQUIRE_EQ(mempool_total_capacity(mp), 256);
  }

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, calloc_entry_zeroes_memory) {
  // Verify that mempool_calloc_entry returns memory that is actually zeroed,
  // including when the slot was previously used with non-zero content.
  const size_t elem_size = sizeof(long long);
  mempool *mp = mempool_create(4, elem_size, false, false, NULL, NULL);
  REQUIRE_NE((void *)mp, NULL);

  long long *slot = mempool_alloc_entry(mp);
  REQUIRE_NE((void *)slot, NULL);
  memset(slot, 0xFF, elem_size);
  mempool_free_entry(slot);

  slot = mempool_calloc_entry(mp);
  REQUIRE_NE((void *)slot, NULL);

  const char zeroes[sizeof(long long)] = {0};
  REQUIRE_EQ(memcmp(slot, zeroes, elem_size), 0);

  mempool_free_entry(slot);
  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, free_null_is_noop) {
  void *p = NULL;
  mempool_free_entry(p);
  REQUIRE_EQ((void *)p, NULL);
}

// Regression: mempool_free_entry's own documented contract ("Detects
// double-free via assertions") had no direct test coverage at all for a
// plain mempool entry; the only forked/SIGABRT-asserting test in this
// whole file exercised r_mempool_realloc_entry, not mempool_free_entry
// itself. _mempool_free_entry (the raw function) is used directly here,
// since the mempool_free_entry macro nulls its own argument after freeing,
// which would make a second call on the same variable a harmless no-op
// rather than a genuine double free.
TEST(cmempools, double_free_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    mempool *mp = mempool_create(4, sizeof(int), false, true, NULL, NULL);
    void *p = mempool_alloc_entry(mp);
    void *p_saved = p;
    mempool_free_entry(p);        /* first free: fine */
    _mempool_free_entry(p_saved); /* second free of the same entry: fatal */
    _exit(0); /* unreachable if ccol_assert() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// Regression: mempool_free_entry's own documented contract ("Detects
// corruption via magic values") had no direct test coverage either. This
// simulates memory corruption from an unrelated bug elsewhere in the
// caller by overwriting the header's elem_status field with neither of its
// two valid values (elem_is_taken/elem_is_free) directly, via the public
// __internal_entry_header layout cmempool.h exposes for exactly this kind
// of low-level introspection.
TEST(cmempools, free_entry_with_corrupted_status_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    mempool *mp = mempool_create(4, sizeof(int), false, true, NULL, NULL);
    void *p = mempool_alloc_entry(mp);
    size_t *status_field =
        (size_t *)((uint8_t *)p - offsetof(__internal_entry_header, next));
    *status_field = 0xdeaddeadUL; /* neither elem_is_taken nor elem_is_free */
    _mempool_free_entry(p);
    _exit(0); /* unreachable if ccol_assert() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// Regression for a real double-free-detection gap found via a deep-scan
// review: mempool_free_entry's only guard against double-freeing a
// dynamically allocated (fallback) entry was
// active_dynamic_memory_buffer_count == 0, which only ever caught the entry
// being double-freed being the SOLE outstanding dynamic entry. The moment a
// second, still-live dynamic entry existed, that check passed on the
// second (illegitimate) free too, and nothing ever rewrote the entry's own
// elem_status on free, so the call silently invoked the allocator's free()
// a second time on the same block and under-counted
// active_dynamic_memory_buffer_count, defeating mempool_destroy's own leak
// detector for the still-outstanding entry. A custom allocator whose
// free() does not clobber the freed block's payload (a fully legitimate
// allocator shape; nothing in this module's documented contract requires
// free() to poison freed memory the way glibc's tcache/fastbin machinery
// incidentally does, which is what made this bug hard to see under a
// plain glibc build) makes this reproducible deterministically, independent
// of the platform's libc internals.
static void *no_clobber_malloc(size_t s) { return malloc(s); }
static void *no_clobber_calloc(size_t n, size_t s) { return calloc(n, s); }
static void *no_clobber_realloc(void *p, size_t s) { return realloc(p, s); }
static void no_clobber_free(void *p) {
  (void)p; /* Deliberately never reclaims/touches p, so a double free is
              never masked by the freed payload happening to get
              overwritten before the second free reads it back. */
}

TEST(cmempools, double_free_of_dynamic_entry_with_another_still_live_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    ccol_memmgmt_procs_t procs = {.malloc = no_clobber_malloc,
                                  .calloc = no_clobber_calloc,
                                  .realloc = no_clobber_realloc,
                                  .free = no_clobber_free};
    mempool *mp = mempool_create(2, sizeof(int), true, true, &procs, NULL);
    mempool_alloc_entry(mp);
    mempool_alloc_entry(mp);
    /* Two dynamic/fallback entries outstanding at once. */
    void *d1 = mempool_alloc_entry(mp);
    void *d2 = mempool_alloc_entry(mp);
    (void)d2; /* kept alive; never freed by this test */
    void *d1_saved = d1;
    mempool_free_entry(d1);        /* first free: fine */
    _mempool_free_entry(d1_saved); /* second free, d2 still live: fatal */
    _exit(0); /* unreachable if ccol_assert() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// Regression: mempool_destroy's own documented contract ("Unfreed,
// dynamically allocated pointers via the fallback memory management
// mechanism will make this function assert") had no direct test coverage at
// all. A regression that silently weakened or removed the
// mempool_dynamic_allocs_count(mp) > 0 check in _mempool_destroy would go
// completely unnoticed by the rest of this suite, since every other test
// carefully frees every dynamic entry it allocates before destroying its
// pool.
TEST(cmempools, destroy_with_leaked_dynamic_entry_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    mempool *mp = mempool_create(2, sizeof(int), true, true, NULL, NULL);
    mempool_alloc_entry(mp);
    mempool_alloc_entry(mp);
    void *leaked = mempool_alloc_entry(mp); /* pool-owned slots exhausted */
    (void)leaked;                           /* deliberately never freed */
    mempool_destroy(mp);                    /* must assert on the leak */
    _exit(0); /* unreachable if ccol_assert() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

TEST(cmempools, dynamic_allocs_count_zero_without_fallback) {
  mempool *mp = mempool_create(4, sizeof(int), false, false, NULL, NULL);
  REQUIRE_NE((void *)mp, NULL);
  REQUIRE_EQ(mempool_dynamic_allocs_count(mp), 0);

  int *ptrs[4];
  for (size_t i = 0; i < 4; ++i) {
    ptrs[i] = mempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    REQUIRE_EQ(mempool_dynamic_allocs_count(mp), 0);
  }

  REQUIRE_EQ((void *)mempool_alloc_entry(mp), NULL);
  REQUIRE_EQ(mempool_dynamic_allocs_count(mp), 0);

  for (size_t i = 0; i < 4; ++i) {
    mempool_free_entry(ptrs[i]);
  }
  REQUIRE_EQ(mempool_dynamic_allocs_count(mp), 0);

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, dynamic_allocs_count_tracks_correctly_with_fallback) {
  mempool *mp = mempool_create(4, sizeof(int), true, false, NULL, NULL);
  REQUIRE_NE((void *)mp, NULL);

  int *ptrs[4];
  for (size_t i = 0; i < 4; ++i) {
    ptrs[i] = mempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    REQUIRE_EQ(mempool_dynamic_allocs_count(mp), 0);
  }

  int *fallback1 = mempool_alloc_entry(mp);
  REQUIRE_NE((void *)fallback1, NULL);
  REQUIRE_EQ(mempool_dynamic_allocs_count(mp), 1);

  int *fallback2 = mempool_alloc_entry(mp);
  REQUIRE_NE((void *)fallback2, NULL);
  REQUIRE_EQ(mempool_dynamic_allocs_count(mp), 2);

  mempool_free_entry(fallback1);
  REQUIRE_EQ(mempool_dynamic_allocs_count(mp), 1);

  mempool_free_entry(fallback2);
  REQUIRE_EQ(mempool_dynamic_allocs_count(mp), 0);

  for (size_t i = 0; i < 4; ++i) {
    mempool_free_entry(ptrs[i]);
  }
  REQUIRE_EQ(mempool_dynamic_allocs_count(mp), 0);
  REQUIRE_EQ(mempool_used_count(mp), 0);

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, c_allocations_and_deallocations_fallback_enabled_no_locks) {
  mempool *mp = mempool_create(4, sizeof(int), true, true, NULL, NULL);
  REQUIRE_NE((void *)mp, NULL);

  REQUIRE_EQ(mempool_used_count(mp), 0);
  REQUIRE_EQ(mempool_total_capacity(mp), 4);

  int *ptrs[4];
  for (size_t i = 0; i < 4; ++i) {
    ptrs[i] = mempool_calloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    REQUIRE_EQ(*ptrs[i], 0);
    *ptrs[i] = (int)i;
    REQUIRE_EQ(mempool_used_count(mp), i + 1);
  }

  int *fallback = mempool_calloc_entry(mp);
  REQUIRE_NE((void *)fallback, NULL);
  REQUIRE_EQ(*fallback, 0);
  REQUIRE_EQ(mempool_dynamic_allocs_count(mp), 1);
  mempool_free_entry(fallback);
  REQUIRE_EQ(mempool_dynamic_allocs_count(mp), 0);

  for (size_t i = 0; i < 4; ++i) {
    mempool_free_entry(ptrs[i]);
    REQUIRE_EQ(mempool_used_count(mp), 4 - (i + 1));
  }

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, create_from_preallocated_fails_null_buffer) {
  char *err;
  mempool *mp = mempool_create_from_preallocated_buffer(NULL, 256, 16, false,
                                                        false, NULL, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// Regression: mempool_create_from_preallocated_buffer used to reject any
// elem_size < sizeof(uintptr_t) outright, an inconsistency with
// mempool_create's own silent-round-up behavior for the identical condition
// (see create_small_elem_size_is_bumped_to_min above). Both constructors now
// round a small-but-nonzero elem_size up to sizeof(uintptr_t) identically.
TEST(cmempools, create_from_preallocated_small_elem_size_is_bumped_to_min) {
  _Alignas(__internal_entry_header) uint8_t buf[256];
  char *err = NULL;
  mempool *mp = mempool_create_from_preallocated_buffer(
      buf, sizeof(buf), sizeof(uintptr_t) - 1, false, false, NULL, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)mp, NULL);

  void *p = mempool_alloc_entry(mp);
  REQUIRE_NE(p, NULL);
  memset(p, 0xAB, sizeof(uintptr_t));
  mempool_free_entry(p);

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

// A genuine elem_size of zero is still a distinct, hard error (matching
// mempool_create's own "elem_count or elem_size is zero" rejection), not
// folded into the small-elem_size rounding path above.
TEST(cmempools, create_from_preallocated_fails_elem_size_zero) {
  _Alignas(__internal_entry_header) uint8_t buf[256];
  char *err = NULL;
  mempool *mp = mempool_create_from_preallocated_buffer(
      buf, sizeof(buf), 0, false, false, NULL, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(cmempools, create_from_preallocated_fails_elem_count_zero) {
  // A buffer exactly sizeof(__internal_entry_header) bytes passes the initial
  // size guard but cannot hold even one element of size 64 (extended_elem_size
  // = 64 + offsetof(header, next) > sizeof(header)).  The function must return
  // NULL with the "calculated elem_count is zero" error.
  _Alignas(__internal_entry_header)
      uint8_t buf[sizeof(__internal_entry_header)];
  char *err;
  mempool *mp = mempool_create_from_preallocated_buffer(
      buf, sizeof(buf), 64, false, false, NULL, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// Regression: neither preallocated-buffer constructor validated that the
// caller's buffer was actually aligned for __internal_entry_header's own
// size_t/pointer fields before reinterpreting its bytes as one; a real,
// if latent, misaligned-access hazard on any platform/compiler that doesn't
// happen to over-align a plain uint8_t[]. A deliberately-offset-by-one
// pointer (guaranteed misaligned relative to a properly aligned backing
// array, regardless of what alignment the compiler chose for the array
// itself) must now be rejected outright rather than silently accepted.
TEST(cmempools, create_from_preallocated_fails_misaligned_buffer) {
  _Alignas(__internal_entry_header) uint8_t buf[257];
  char *err = NULL;
  mempool *mp = mempool_create_from_preallocated_buffer(
      buf + 1, sizeof(buf) - 1, 16, false, false, NULL, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// Regression: DECLARE_PREALLOCATED_MEMPOOL_BUFFER used to size its buffer
// using the caller's raw elem_size, while mempool_create_from_preallocated_
// buffer (the constructor it exists to feed) silently rounds elem_size up to
// sizeof(uintptr_t) before dividing the buffer into elem_count elements. A
// buffer declared for an elem_size smaller than sizeof(uintptr_t) was
// therefore sized for the caller's smaller, unrounded elem_size while the
// constructor divided it up using the larger, rounded one, yielding fewer
// elements than elem_count promised. The macro now applies the identical
// rounding before computing the buffer's size, so the two always agree.
DECLARE_PREALLOCATED_MEMPOOL_BUFFER(preallocated_mp_small_elem_buffer, 100, 1);

TEST(cmempools,
     declare_preallocated_buffer_small_elem_size_yields_exact_elem_count) {
  mempool *mp = mempool_create_from_preallocated_buffer(
      preallocated_mp_small_elem_buffer,
      sizeof(preallocated_mp_small_elem_buffer), 1, false, false, NULL, NULL);
  REQUIRE_NE((void *)mp, NULL);

  // Before the fix, the buffer was sized for elem_size=1 (unrounded) while
  // the constructor divides it up using extended_elem_size for the rounded
  // elem_size=sizeof(uintptr_t), so fewer than 100 elements would fit.
  REQUIRE_EQ(mempool_total_capacity(mp), 100);

  void *ptrs[100];
  for (size_t i = 0; i < 100; ++i) {
    ptrs[i] = mempool_alloc_entry(mp);
    REQUIRE_NE(ptrs[i], NULL);
  }
  REQUIRE_EQ((void *)mempool_alloc_entry(mp), NULL);

  for (size_t i = 0; i < 100; ++i) {
    mempool_free_entry(ptrs[i]);
  }

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

// A minimal case that used to fail creation outright: before the fix, a
// buffer declared for exactly 1 element of elem_size 1 was sized using the
// unrounded elem_size (1 byte plus header overhead), too small to hold even
// a single element once the constructor rounded elem_size up to
// sizeof(uintptr_t); mempool_create_from_preallocated_buffer would return
// NULL with "calculated elem_count is zero" despite the caller having
// followed the macro's own documented usage pattern exactly.
DECLARE_PREALLOCATED_MEMPOOL_BUFFER(preallocated_mp_single_small_elem_buffer, 1,
                                    1);

TEST(cmempools, declare_preallocated_buffer_single_small_elem_size_succeeds) {
  mempool *mp = mempool_create_from_preallocated_buffer(
      preallocated_mp_single_small_elem_buffer,
      sizeof(preallocated_mp_single_small_elem_buffer), 1, false, false, NULL,
      NULL);
  REQUIRE_NE((void *)mp, NULL);
  REQUIRE_EQ(mempool_total_capacity(mp), 1);

  void *p = mempool_alloc_entry(mp);
  REQUIRE_NE(p, NULL);
  REQUIRE_EQ((void *)mempool_alloc_entry(mp), NULL);
  mempool_free_entry(p);

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

// Regression: DECLARE_PREALLOCATED_MEMPOOL_BUFFER used to size its buffer
// using elem_size + header overhead with no rounding to __internal_entry_
// header's own alignment requirement, while mempool_create_from_
// preallocated_buffer (the constructor it exists to feed) now rounds that
// same sum up to the header's own alignment before dividing the buffer
// into elem_count elements (see pool_entries_beyond_first_are_properly_
// aligned above for the underlying bug this closes). A buffer declared for
// an elem_size of 9 (deliberately not a multiple of sizeof(uintptr_t), so
// neither the small-elem_size rounding nor a lucky already-aligned size
// masks the mismatch) was therefore sized using a smaller stride than the
// constructor actually divides it up with, yielding fewer usable elements
// than elem_count promised. The macro now applies the identical rounding,
// so the two always agree, and every entry the resulting pool hands out is
// correctly aligned.
DECLARE_PREALLOCATED_MEMPOOL_BUFFER(preallocated_mp_odd_elem_buffer, 50, 9);

TEST(
    cmempools,
    declare_preallocated_buffer_odd_elem_size_yields_exact_elem_count_and_alignment) {
  mempool *mp = mempool_create_from_preallocated_buffer(
      preallocated_mp_odd_elem_buffer, sizeof(preallocated_mp_odd_elem_buffer),
      9, false, false, NULL, NULL);
  REQUIRE_NE((void *)mp, NULL);

  // Before the fix, the buffer was sized for the unaligned stride (9 + header
  // overhead), while the constructor divides it up using the aligned,
  // slightly larger stride, so fewer than 50 elements would fit.
  REQUIRE_EQ(mempool_total_capacity(mp), 50);

  void *ptrs[50];
  for (size_t i = 0; i < 50; ++i) {
    ptrs[i] = mempool_alloc_entry(mp);
    REQUIRE_NE(ptrs[i], NULL);
    uintptr_t header_addr =
        (uintptr_t)ptrs[i] - offsetof(__internal_entry_header, next);
    REQUIRE_EQ(header_addr % _Alignof(__internal_entry_header), 0);
  }
  REQUIRE_EQ((void *)mempool_alloc_entry(mp), NULL);

  for (size_t i = 0; i < 50; ++i) {
    mempool_free_entry(ptrs[i]);
  }

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

// Preallocated memory pool tests
DECLARE_PREALLOCATED_MEMPOOL_BUFFER(preallocated_mp_buffer, 32768, 256);
char *preallocated_ptrs[32768] = {0};  // 8388608 / 256 = 32768

TEST(cmempools, preallocated_buffer_without_fallback) {
  mempool *mp = mempool_create_from_preallocated_buffer(
      preallocated_mp_buffer, sizeof(preallocated_mp_buffer), 256, false, false,
      NULL, NULL);
  REQUIRE_NE((void *)mp, NULL);

  size_t capacity = mempool_total_capacity(mp);
  REQUIRE_EQ(capacity, 32768);

  for (size_t i = 0; i < capacity; ++i) {
    preallocated_ptrs[i] = mempool_alloc_entry(mp);
    REQUIRE_NE((void *)preallocated_ptrs[i], NULL);
  }

  REQUIRE_EQ(mempool_alloc_entry(mp), NULL);

  for (size_t i = 0; i < capacity; ++i) {
    mempool_free_entry(preallocated_ptrs[i]);
    REQUIRE_EQ((void *)preallocated_ptrs[i], NULL);
  }

  for (size_t i = 0; i < capacity; ++i) {
    preallocated_ptrs[i] = mempool_calloc_entry(mp);
    REQUIRE_NE((void *)preallocated_ptrs[i], NULL);
  }

  REQUIRE_EQ(mempool_calloc_entry(mp), NULL);

  for (size_t i = 0; i < capacity; ++i) {
    mempool_free_entry(preallocated_ptrs[i]);
    REQUIRE_EQ((void *)preallocated_ptrs[i], NULL);
  }

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, preallocated_buffer_without_fallback_no_locks) {
  mempool *mp = mempool_create_from_preallocated_buffer(
      preallocated_mp_buffer, sizeof(preallocated_mp_buffer), 256, false, true,
      NULL, NULL);
  REQUIRE_NE((void *)mp, NULL);

  size_t capacity = mempool_total_capacity(mp);
  REQUIRE_EQ(capacity, 32768);

  for (size_t i = 0; i < capacity; ++i) {
    preallocated_ptrs[i] = mempool_alloc_entry(mp);
    REQUIRE_NE((void *)preallocated_ptrs[i], NULL);
  }

  REQUIRE_EQ(mempool_alloc_entry(mp), NULL);

  for (size_t i = 0; i < capacity; ++i) {
    mempool_free_entry(preallocated_ptrs[i]);
    REQUIRE_EQ((void *)preallocated_ptrs[i], NULL);
  }

  for (size_t i = 0; i < capacity; ++i) {
    preallocated_ptrs[i] = mempool_calloc_entry(mp);
    REQUIRE_NE((void *)preallocated_ptrs[i], NULL);
  }

  REQUIRE_EQ(mempool_calloc_entry(mp), NULL);

  for (size_t i = 0; i < capacity; ++i) {
    mempool_free_entry(preallocated_ptrs[i]);
    REQUIRE_EQ((void *)preallocated_ptrs[i], NULL);
  }

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, preallocated_buffer_with_fallback) {
  mempool *mp = mempool_create_from_preallocated_buffer(
      preallocated_mp_buffer, sizeof(preallocated_mp_buffer), 256, true, false,
      NULL, NULL);
  REQUIRE_NE((void *)mp, NULL);

  size_t capacity = mempool_total_capacity(mp);
  REQUIRE_EQ(capacity, 32768);

  for (size_t i = 0; i < capacity; ++i) {
    preallocated_ptrs[i] = mempool_alloc_entry(mp);
    REQUIRE_NE((void *)preallocated_ptrs[i], NULL);
  }

  void *tmp = mempool_alloc_entry(mp);
  REQUIRE_NE(tmp, NULL);
  mempool_free_entry(tmp);

  for (size_t i = 0; i < capacity; ++i) {
    mempool_free_entry(preallocated_ptrs[i]);
    REQUIRE_EQ((void *)preallocated_ptrs[i], NULL);
  }

  for (size_t i = 0; i < capacity; ++i) {
    preallocated_ptrs[i] = mempool_calloc_entry(mp);
    REQUIRE_NE((void *)preallocated_ptrs[i], NULL);
  }

  tmp = mempool_calloc_entry(mp);
  REQUIRE_NE(tmp, NULL);
  mempool_free_entry(tmp);

  for (size_t i = 0; i < capacity; ++i) {
    mempool_free_entry(preallocated_ptrs[i]);
    REQUIRE_EQ((void *)preallocated_ptrs[i], NULL);
  }

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(cmempools, preallocated_buffer_with_fallback_no_locks) {
  mempool *mp = mempool_create_from_preallocated_buffer(
      preallocated_mp_buffer, sizeof(preallocated_mp_buffer), 256, true, true,
      NULL, NULL);
  REQUIRE_NE((void *)mp, NULL);

  size_t capacity = mempool_total_capacity(mp);
  REQUIRE_EQ(capacity, 32768);

  for (size_t i = 0; i < capacity; ++i) {
    preallocated_ptrs[i] = mempool_alloc_entry(mp);
    REQUIRE_NE((void *)preallocated_ptrs[i], NULL);
  }

  void *tmp = mempool_alloc_entry(mp);
  REQUIRE_NE(tmp, NULL);
  mempool_free_entry(tmp);

  for (size_t i = 0; i < capacity; ++i) {
    mempool_free_entry(preallocated_ptrs[i]);
    REQUIRE_EQ((void *)preallocated_ptrs[i], NULL);
  }

  for (size_t i = 0; i < capacity; ++i) {
    preallocated_ptrs[i] = mempool_calloc_entry(mp);
    REQUIRE_NE((void *)preallocated_ptrs[i], NULL);
  }

  tmp = mempool_calloc_entry(mp);
  REQUIRE_NE(tmp, NULL);
  mempool_free_entry(tmp);

  for (size_t i = 0; i < capacity; ++i) {
    mempool_free_entry(preallocated_ptrs[i]);
    REQUIRE_EQ((void *)preallocated_ptrs[i], NULL);
  }

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

// C_R_MEMPOOL TESTS

TEST(r_mempools, create_fails) {
  char *err;

  // SC = 0
  r_mempool *rmp =
      r_mempool_create(4, 17, 0, fallback_disabled, false, NULL, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  // LS = 0
  rmp = r_mempool_create(4, 0, 17, fallback_disabled, false, NULL, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  // SS = 0
  rmp = r_mempool_create(0, 17, 17, fallback_disabled, false, NULL, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  // LS < SS
  rmp = r_mempool_create(4, 3, 17, fallback_disabled, false, NULL, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  // LS == SS
  rmp = r_mempool_create(4, 4, 17, fallback_disabled, false, NULL, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  // SC positive but smaller than LS - SS: SC=2, LS-SS=3 (7-4=3 > 2).
  // This exercises the (SC < LS-SS) branch in assess_r_mempool_create_inputs,
  // which is distinct from the SC=0 zero-check tested above.
  rmp = r_mempool_create(4, 7, 2, fallback_disabled, false, NULL, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  // invalid fallback policy
  rmp = r_mempool_create(4, 6, 17, -1, false, NULL, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  rmp = r_mempool_create(4, 6, 17, __fallback_end_place_holder, false, NULL,
                         &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  rmp = r_mempool_create(4, 6, 17, __fallback_end_place_holder + 1, false, NULL,
                         &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// Regression: neither r_mempool_create nor
// r_mempool_create_from_preallocated_buffer previously had a test pairing
// otherwise-valid size parameters with an incomplete ccol_memmgmt_procs_t,
// so a regression in ccol_verify_memmgmt_procs's call site for either
// function would go unnoticed.
TEST(r_mempools, create_rejects_incomplete_procs_with_valid_size_params) {
  ccol_memmgmt_procs_t m_procs = {
      .malloc = malloc, .calloc = NULL, .realloc = realloc, .free = free};
  char *err = NULL;
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_disabled, false, &m_procs, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(r_mempools,
     create_from_preallocated_rejects_incomplete_procs_with_valid_params) {
  static DECLARE_PREALLOCATED_RMEMPOOL_BUFFER(buf, 4, 6, 7);
  ccol_memmgmt_procs_t m_procs = {
      .malloc = malloc, .calloc = calloc, .realloc = NULL, .free = free};
  char *err = NULL;
  r_mempool *rmp = r_mempool_create_from_preallocated_buffer(
      buf, sizeof(buf), 4, 6, 7, fallback_disabled, true, &m_procs, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(r_mempools, create_succeeds_with_minimum_sc) {
  // SC == LS - SS is the tightest valid configuration: the largest sub-pool
  // gets exactly one element (2^SC / 2^(LS-SS) = 1).
  // SS=4, LS=6, SC=2: pool[0]=4x16B, pool[1]=2x32B, pool[2]=1x64B.
  char *err = NULL;
  r_mempool *rmp =
      r_mempool_create(4, 6, 2, fallback_disabled, false, NULL, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  REQUIRE_EQ(r_mempool_total_capacity(rmp, 16), 4);
  REQUIRE_EQ(r_mempool_total_capacity(rmp, 32), 2);
  REQUIRE_EQ(r_mempool_total_capacity(rmp, 64), 1);

  // Allocate from each tier and verify accounting.
  void *p16 = r_mempool_alloc_entry(rmp, 16);
  REQUIRE_NE(p16, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 1);

  void *p32 = r_mempool_alloc_entry(rmp, 32);
  REQUIRE_NE(p32, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 1);

  // The 64-byte pool has exactly one slot; a second request returns NULL.
  void *p64 = r_mempool_alloc_entry(rmp, 64);
  REQUIRE_NE(p64, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 64), 1);

  REQUIRE_EQ((void *)r_mempool_alloc_entry(rmp, 64), NULL);

  r_mempool_free_entry(p16);
  r_mempool_free_entry(p32);
  r_mempool_free_entry(p64);

  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 64), 0);

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, create_succeeds) {
  char *err;
  r_mempool *rmp =
      r_mempool_create(4, 17, 17, fallback_disabled, false, NULL, &err);
  REQUIRE_NE((void *)rmp, NULL);

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);

  rmp = r_mempool_create(4, 17, 17, fallback_disabled, true, NULL, &err);
  REQUIRE_NE((void *)rmp, NULL);

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, simple_allocations) {
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_disabled, false, NULL, NULL);
  char *ptr = NULL;

  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);
  ptr = r_mempool_alloc_entry(rmp, 1);  // Should come from buffers of 16
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 1);
  r_mempool_free_entry(ptr);

  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);
  ptr = r_mempool_alloc_entry(rmp, 16);  // Should come from buffers of 16
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 1);
  r_mempool_free_entry(ptr);

  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  ptr = r_mempool_alloc_entry(rmp, 17);  // Should come from buffers of 32
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 1);
  r_mempool_free_entry(ptr);

  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  ptr = r_mempool_alloc_entry(rmp, 32);  // Should come from buffers of 32
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 1);
  r_mempool_free_entry(ptr);

  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 64), 0);
  ptr = r_mempool_alloc_entry(rmp, 33);  // Should come from buffers of 64
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 64), 1);
  r_mempool_free_entry(ptr);

  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 64), 0);
  ptr = r_mempool_alloc_entry(rmp, 63);  // Should come from buffers of 64
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 64), 1);
  r_mempool_free_entry(ptr);

  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 64), 0);
  ptr = r_mempool_alloc_entry(rmp, 64);  // Should come from buffers of 64
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 64), 1);
  r_mempool_free_entry(ptr);

  REQUIRE_EQ((void *)r_mempool_alloc_entry(rmp, 0), NULL);
  REQUIRE_EQ((void *)r_mempool_alloc_entry(rmp, 65), NULL);

  r_mempool_destroy(rmp);
}

// Regression: exhaustively verifies the O(1), table-free
// r_mempool_pool_index_for_size formula (a shift plus a bit-scan, which
// replaced an O(largest_size/smallest_size) lookup table (exponential in the
// number of tiers) against every single size boundary across a
// 7-tier pool, not just the handful of individual sizes the other tests
// happen to touch. r_mempool_total_capacity is used as the externally
// observable probe: each tier's own fixed capacity (independently known from
// how r_mempool_create halves the element count per doubling) must hold
// constant across an entire size range and jump to the next tier's capacity
// at exactly the next power-of-two-times-smallest_size boundary, never one
// byte early or late.
TEST(r_mempools, pool_index_boundaries_exhaustive) {
  // SS=4, LS=10, SC=6: tiers of 16/32/64/128/256/512/1024 bytes holding
  // 64/32/16/8/4/2/1 elements respectively (each halving from the last).
  r_mempool *rmp =
      r_mempool_create(4, 10, 6, fallback_disabled, false, NULL, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  const size_t tier_sizes[] = {16, 32, 64, 128, 256, 512, 1024};
  const size_t tier_caps[] = {64, 32, 16, 8, 4, 2, 1};
  const size_t num_tiers = sizeof(tier_sizes) / sizeof(tier_sizes[0]);

  for (size_t t = 0; t < num_tiers; ++t) {
    REQUIRE_EQ(r_mempool_total_capacity(rmp, tier_sizes[t]), tier_caps[t]);
  }

  size_t prev_boundary = 0;
  for (size_t t = 0; t < num_tiers; ++t) {
    for (size_t size = prev_boundary + 1; size <= tier_sizes[t]; ++size) {
      REQUIRE_EQ(r_mempool_total_capacity(rmp, size), tier_caps[t]);
    }
    prev_boundary = tier_sizes[t];
  }

  // The formula must also route real allocations to the tier it claims to,
  // not just report the right capacity for it.
  for (size_t t = 0; t < num_tiers; ++t) {
    size_t just_over_prev = (t == 0) ? 1 : tier_sizes[t - 1] + 1;
    void *p = r_mempool_alloc_entry(rmp, just_over_prev);
    REQUIRE_NE(p, NULL);
    REQUIRE_EQ(r_mempool_used_count(rmp, tier_sizes[t]), 1);
    r_mempool_free_entry(p);
    REQUIRE_EQ(r_mempool_used_count(rmp, tier_sizes[t]), 0);

    p = r_mempool_alloc_entry(rmp, tier_sizes[t]);
    REQUIRE_NE(p, NULL);
    REQUIRE_EQ(r_mempool_used_count(rmp, tier_sizes[t]), 1);
    r_mempool_free_entry(p);
    REQUIRE_EQ(r_mempool_used_count(rmp, tier_sizes[t]), 0);
  }

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

// Regression for the O(2^(largest_pow - smallest_pow)) reverse-size lookup
// table this module used to allocate at construction time: with a
// preallocated-buffer pool (whose actual element storage is the caller's own
// externally supplied buffer, not heap memory), the OLD lookup table was the
// only heap allocation this constructor ever needed to make on the library's
// own behalf; so a caller specifically trying to avoid heap allocation
// (the entire point of the preallocated-buffer API) still silently required
// one, exponentially sized. A custom allocator that fails any single
// allocation request over 4096 bytes proves this constructor no longer needs
// one: the old lookup table for this configuration (2^12 * sizeof(size_t) =
// 32768 bytes) would have failed outright under this allocator; every
// allocation this constructor now actually makes (the r_mempool struct
// itself, the mem_pools array, and one small mempool struct per tier) stays
// far under that cap.
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
  // SS=4, LS=16, SC=12 (the minimum allowed, LS-SS): 13 tiers. The buffer
  // itself (sized by the linear CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE
  // formula) is comfortably under 1 MiB; declared static so it lives in the
  // binary's own data segment rather than risking a stack overflow.
  static DECLARE_PREALLOCATED_RMEMPOOL_BUFFER(buf, 4, 16, 12);

  ccol_memmgmt_procs_t m_procs = {.malloc = no_huge_alloc_malloc,
                                  .calloc = no_huge_alloc_calloc,
                                  .realloc = no_huge_alloc_realloc,
                                  .free = no_huge_alloc_free};
  char *err = NULL;
  r_mempool *rmp = r_mempool_create_from_preallocated_buffer(
      buf, sizeof(buf), 4, 16, 12, fallback_disabled, false, &m_procs, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *p = r_mempool_alloc_entry(rmp, 16);
  REQUIRE_NE(p, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 1);
  r_mempool_free_entry(p);

  p = r_mempool_alloc_entry(rmp, 1 << 16); /* the largest tier, 65536 bytes */
  REQUIRE_NE(p, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 1 << 16), 1);
  r_mempool_free_entry(p);

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

// Regression: r_mempool_create_from_preallocated_buffer's own buffer-size
// validation computed the expected total buffer size via a plain
// multiply-then-accumulate loop with no overflow checking at all, unlike
// the closed-form CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE macro this
// same total is meant to agree with, which was specifically designed to
// avoid intermediate size_t overflow. For a parameter combination whose
// true required buffer size exceeds SIZE_MAX (physically unrealizable, but
// not rejected by any of assess_r_mempool_create_inputs's own range
// checks), the validation loop's running sum could silently wrap. SS=4,
// LS=63 (the maximum allowed), SC=62 (>= LS-SS=59, satisfying the
// minimum-count constraint): the very first (smallest) tier's own term
// alone, 2^62 elements of 32 extended bytes each, is 2^67, well past
// SIZE_MAX on a 64-bit size_t, wrapping to a huge-but-not-SIZE_MAX value
// (0xffffffffffffff80) that (purely by chance) still doesn't equal this
// test's own small buf_size, so the call returns NULL either way and a bare
// NULL check alone cannot tell the fixed behavior apart from the
// pre-existing bug (both "fail", for different reasons). What genuinely
// distinguishes them is *why*: the unfixed loop would silently wrap all the
// way through its arithmetic and only ever report the generic, unrelated
// "buffer sizes differ" (the same message an ordinary, non-overflowing
// buf_size mismatch produces), while the fix detects the overflow directly,
// before ever comparing against buf_size, and reports a specific message
// naming the real cause instead. Asserting on that message is what actually
// pins the fix rather than a coincidence of these particular numbers.
TEST(preallocated_r_mempools,
     create_rejects_configuration_whose_true_buffer_size_overflows) {
#if SIZE_MAX > 0xFFFFFFFFu
  /* largest_size_power_of_two=63 is only a valid input at all on a platform
   * where size_t is wider than 32 bits: assess_r_mempool_create_inputs's own
   * "power of two exceeds size_t width" guard (checked before this
   * scenario's own true-buffer-size-overflow arithmetic is ever reached)
   * rejects any power-of-two exponent >= sizeof(size_t)*CHAR_BIT
   * unconditionally, which on a 32-bit size_t (e.g. i386) means 63 is
   * rejected immediately, for a different and unrelated reason, before
   * init_preallocated_r_mempool_internal_pools's own overflow-detecting
   * loop (the actual code this test exists to pin) is ever reached. There
   * is no equivalent (SS, LS, SC) triple that reaches that same loop's own
   * overflow branch on both word widths at once: the magnitude needed to
   * overflow size_t scales with the platform's own word width, while LS
   * itself is capped strictly below that same word width by the guard
   * above, so a 32-bit-safe LS can never carry enough magnitude to overflow
   * a 32-bit size_t through this loop either. Guarded with a compile-time
   * #if, not a runtime check, since SS=4, LS=63, SC=62 mean something
   * different (a different, and here inapplicable, rejection reason) on a
   * 32-bit platform, not merely a differently-timed one. */
  _Alignas(__internal_entry_header) uint8_t buf[256];
  char *err = NULL;
  r_mempool *rmp = r_mempool_create_from_preallocated_buffer(
      buf, sizeof(buf), 4, 63, 62, fallback_disabled, true, NULL, &err);
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
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_disabled, false, NULL, NULL);
  char *ptr = NULL;

  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  ptr = r_mempool_realloc_entry(rmp, ptr, 3);
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 1);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);

  for (int i = 0; i < 3; ++i) {
    ptr[i] = i;
  }

  char *orig = ptr;
  ptr = r_mempool_realloc_entry(rmp, ptr, 6);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 1);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  // Since the block size is still 16 no real "reallocation" happened
  REQUIRE_EQ((void *)orig, (void *)ptr);
  for (int i = 0; i < 6; ++i) {
    if (i < 3) {
      REQUIRE_EQ(ptr[i], i);
    } else {
      ptr[i] = i;
    }
  }

  ptr = r_mempool_realloc_entry(rmp, ptr, 20);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 1);
  REQUIRE_NE((void *)orig, (void *)ptr);
  for (int i = 0; i < 20; ++i) {
    if (i < 6) {
      REQUIRE_EQ(ptr[i], i);
    } else {
      ptr[i] = i;
    }
  }

  orig = ptr;
  ptr = r_mempool_realloc_entry(rmp, ptr, 5);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 1);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  REQUIRE_NE((void *)orig, (void *)ptr);
  for (int i = 0; i < 5; ++i) {
    REQUIRE_EQ(ptr[i], i);
  }

  r_mempool_free_entry(ptr);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);

  r_mempool_destroy(rmp);
}

TEST(r_mempools, simple_c_allocations) {
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_disabled, false, NULL, NULL);
  char *ptr = NULL;

  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);
  ptr = r_mempool_calloc_entry(rmp, 1);  // Should come from buffers of 16
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 1);
  r_mempool_free_entry(ptr);

  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);
  ptr = r_mempool_calloc_entry(rmp, 16);  // Should come from buffers of 16
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 1);
  r_mempool_free_entry(ptr);

  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  ptr = r_mempool_calloc_entry(rmp, 17);  // Should come from buffers of 32
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 1);
  r_mempool_free_entry(ptr);

  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  ptr = r_mempool_calloc_entry(rmp, 32);  // Should come from buffers of 32
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 1);
  r_mempool_free_entry(ptr);

  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 64), 0);
  ptr = r_mempool_calloc_entry(rmp, 33);  // Should come from buffers of 64
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 64), 1);
  r_mempool_free_entry(ptr);

  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 64), 0);
  ptr = r_mempool_calloc_entry(rmp, 63);  // Should come from buffers of 64
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 64), 1);
  r_mempool_free_entry(ptr);

  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 64), 0);
  ptr = r_mempool_calloc_entry(rmp, 64);  // Should come from buffers of 64
  REQUIRE_NE((void *)ptr, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 64), 1);
  r_mempool_free_entry(ptr);

  REQUIRE_EQ((void *)r_mempool_calloc_entry(rmp, 0), NULL);
  REQUIRE_EQ((void *)r_mempool_calloc_entry(rmp, 65), NULL);

  r_mempool_destroy(rmp);
}

TEST(r_mempools, exhaust_all_fallback_disabled) {
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_disabled, false, NULL, NULL);

  size_t ptrs_len = 224;  // 16: 128, 32: 64, 64: 32 => 128 + 64 + 32 = 224

  void *ptrs[ptrs_len];

  for (size_t i = 0; i < ptrs_len; ++i) {
    ptrs[i] = r_mempool_alloc_entry(rmp, 8);
    REQUIRE_NE((void *)ptrs[i], NULL);
    // It even gives 32 and 64 sized buffers to fulfill our request.
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size),
               r_mempool_total_capacity(rmp, size));
  }

  // It doesn't allocate new memory from the heap, once the pre-allocated
  // buffers are exhausted, that's it, no new memory till some are given
  // back.
  void *tmp_ptr = r_mempool_alloc_entry(rmp, 8);
  REQUIRE_EQ((void *)tmp_ptr, NULL);

  for (size_t i = 0; i < ptrs_len; ++i) {
    r_mempool_free_entry(ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size), 0);
  }

  r_mempool_destroy(rmp);
}

TEST(r_mempools, c_exhaust_all_fallback_disabled) {
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_disabled, false, NULL, NULL);

  size_t ptrs_len = 224;  // 16: 128, 32: 64, 64: 32 => 128 + 64 + 32 = 224

  void *ptrs[ptrs_len];

  for (size_t i = 0; i < ptrs_len; ++i) {
    ptrs[i] = r_mempool_calloc_entry(rmp, 8);
    REQUIRE_NE((void *)ptrs[i], NULL);
    // It even gives 32 and 64 sized buffers to fulfill our request.
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size),
               r_mempool_total_capacity(rmp, size));
  }

  // It doesn't allocate new memory from the heap, once the pre-allocated
  // buffers are exhausted, that's it, no new memory till some are given
  // back.
  void *tmp_ptr = r_mempool_calloc_entry(rmp, 8);
  REQUIRE_EQ((void *)tmp_ptr, NULL);

  for (size_t i = 0; i < ptrs_len; ++i) {
    r_mempool_free_entry(ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size), 0);
  }

  r_mempool_destroy(rmp);
}

TEST(r_mempools, exhaust_last_subpool_returns_null_fallback_disabled) {
  // Regression test for the off-by-one in r_mempool_alloc_entry.
  // The loop condition was `pool_index <= number_of_mempools` instead of
  // `pool_index < number_of_mempools`.  When the last sub-pool (index
  // number_of_mempools-1) was full, the loop incremented pool_index to
  // number_of_mempools and tried to dereference mem_pools[number_of_mempools],
  // which is NULL (the array is only valid up to index number_of_mempools-1),
  // causing an assertion crash instead of returning NULL.
  //
  // We target pool 2 (64-byte slots, 32 entries) directly so that the cascade
  // starts at the last valid pool index and the bug manifests on the very first
  // over-limit increment.
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_disabled, false, NULL, NULL);

  // 2^7 smallest / 2^2 scale-down = 32 slots in the 64-byte pool
  size_t capacity = 32;
  void *ptrs[32];

  for (size_t i = 0; i < capacity; ++i) {
    ptrs[i] = r_mempool_alloc_entry(rmp, 64);
    REQUIRE_NE((void *)ptrs[i], NULL);
  }

  REQUIRE_EQ(r_mempool_used_count(rmp, 64), r_mempool_total_capacity(rmp, 64));
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);

  // This must return NULL, not crash.
  void *tmp = r_mempool_alloc_entry(rmp, 64);
  REQUIRE_EQ((void *)tmp, NULL);

  for (size_t i = 0; i < capacity; ++i) {
    r_mempool_free_entry(ptrs[i]);
  }

  REQUIRE_EQ(r_mempool_used_count(rmp, 64), 0);

  r_mempool_destroy(rmp);
}

TEST(r_mempools, try_exhausting_with_fallback_at_first_exhaustion) {
  r_mempool *rmp = r_mempool_create(4, 6, 7, fallback_at_first_exhaustion,
                                    false, NULL, NULL);

  size_t ptrs_len = 224;  // 16: 128, 32: 64, 64: 32 => 128 + 64 + 32 = 224

  void *ptrs[ptrs_len];
  uint ptr_iterator = 0;

  for (size_t i = 0; i < 128; ++i) {
    ptrs[ptr_iterator++] = r_mempool_alloc_entry(rmp, 16);
  }

  for (size_t i = 0; i < 64; ++i) {
    ptrs[ptr_iterator++] = r_mempool_alloc_entry(rmp, 32);
  }

  for (size_t i = 0; i < 32; ++i) {
    ptrs[ptr_iterator++] = r_mempool_alloc_entry(rmp, 64);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size),
               r_mempool_total_capacity(rmp, size));
  }

  // Even if the pre-allocated buffers have been exhausted, as the
  // fallback is enabled, it will allocate more memory from heap
  // to fulfill the ask. The allocations will happen via separate
  // internal memory pools, and therefore the function
  // r_mempool_alloc_entry will return distinct values for different
  // sizes.
  void *tmp_ptr_16 = r_mempool_alloc_entry(rmp, 16);
  REQUIRE_NE((void *)tmp_ptr_16, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);

  void *tmp_ptr_32 = r_mempool_alloc_entry(rmp, 32);
  REQUIRE_NE((void *)tmp_ptr_32, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 1);

  void *tmp_ptr_64 = r_mempool_alloc_entry(rmp, 64);
  REQUIRE_NE((void *)tmp_ptr_64, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 1);

  for (size_t i = 0; i < ptrs_len; ++i) {
    r_mempool_free_entry(ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size), 0);
  }

  r_mempool_free_entry(tmp_ptr_16);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);

  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 1);
  r_mempool_free_entry(tmp_ptr_32);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);

  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 1);
  r_mempool_free_entry(tmp_ptr_64);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 0);

  r_mempool_destroy(rmp);
}

TEST(r_mempools, c_try_exhausting_with_fallback_at_first_exhaustion) {
  r_mempool *rmp = r_mempool_create(4, 6, 7, fallback_at_first_exhaustion,
                                    false, NULL, NULL);

  size_t ptrs_len = 224;  // 16: 128, 32: 64, 64: 32 => 128 + 64 + 32 = 224

  void *ptrs[ptrs_len];
  uint ptr_iterator = 0;

  for (size_t i = 0; i < 128; ++i) {
    ptrs[ptr_iterator++] = r_mempool_calloc_entry(rmp, 16);
  }

  for (size_t i = 0; i < 64; ++i) {
    ptrs[ptr_iterator++] = r_mempool_calloc_entry(rmp, 32);
  }

  for (size_t i = 0; i < 32; ++i) {
    ptrs[ptr_iterator++] = r_mempool_calloc_entry(rmp, 64);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size),
               r_mempool_total_capacity(rmp, size));
  }

  // Even if the pre-allocated buffers have been exhausted, as the
  // fallback is enabled, it will allocate more memory from heap
  // to fulfill the ask. The allocations will happen via separate
  // internal memory pools, and therefore the function
  // r_mempool_calloc_entry will return distinct values for different
  // sizes.
  void *tmp_ptr_16 = r_mempool_calloc_entry(rmp, 16);
  REQUIRE_NE((void *)tmp_ptr_16, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);

  void *tmp_ptr_32 = r_mempool_calloc_entry(rmp, 32);
  REQUIRE_NE((void *)tmp_ptr_32, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 1);

  void *tmp_ptr_64 = r_mempool_calloc_entry(rmp, 64);
  REQUIRE_NE((void *)tmp_ptr_64, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 1);

  for (size_t i = 0; i < ptrs_len; ++i) {
    r_mempool_free_entry(ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size), 0);
  }

  r_mempool_free_entry(tmp_ptr_16);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);

  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 1);
  r_mempool_free_entry(tmp_ptr_32);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);

  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 1);
  r_mempool_free_entry(tmp_ptr_64);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 0);

  r_mempool_destroy(rmp);
}

TEST(r_mempools, c_try_exhausting_with_fallback_at_first_exhaustion_no_locks) {
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_at_first_exhaustion, true, NULL, NULL);

  size_t ptrs_len = 224;  // 16: 128, 32: 64, 64: 32 => 128 + 64 + 32 = 224

  void *ptrs[ptrs_len];
  uint ptr_iterator = 0;

  for (size_t i = 0; i < 128; ++i) {
    ptrs[ptr_iterator++] = r_mempool_calloc_entry(rmp, 16);
  }

  for (size_t i = 0; i < 64; ++i) {
    ptrs[ptr_iterator++] = r_mempool_calloc_entry(rmp, 32);
  }

  for (size_t i = 0; i < 32; ++i) {
    ptrs[ptr_iterator++] = r_mempool_calloc_entry(rmp, 64);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size),
               r_mempool_total_capacity(rmp, size));
  }

  // Even if the pre-allocated buffers have been exhausted, as the
  // fallback is enabled, it will allocate more memory from heap
  // to fulfill the ask. The allocations will happen via separate
  // internal memory pools, and therefore the function
  // r_mempool_calloc_entry will return distinct values for different
  // sizes.
  void *tmp_ptr_16 = r_mempool_calloc_entry(rmp, 16);
  REQUIRE_NE((void *)tmp_ptr_16, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);

  void *tmp_ptr_32 = r_mempool_calloc_entry(rmp, 32);
  REQUIRE_NE((void *)tmp_ptr_32, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 1);

  void *tmp_ptr_64 = r_mempool_calloc_entry(rmp, 64);
  REQUIRE_NE((void *)tmp_ptr_64, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 1);

  for (size_t i = 0; i < ptrs_len; ++i) {
    r_mempool_free_entry(ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size), 0);
  }

  r_mempool_free_entry(tmp_ptr_16);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);

  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 1);
  r_mempool_free_entry(tmp_ptr_32);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);

  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 1);
  r_mempool_free_entry(tmp_ptr_64);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 0);

  r_mempool_destroy(rmp);
}

TEST(r_mempools, try_exhausting_with_fallback_at_last_exhaustion) {
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_at_last_exhaustion, false, NULL, NULL);

  size_t ptrs_len = 224;  // 16: 128, 32: 64, 64: 32 => 128 + 64 + 32 = 224

  void *ptrs[ptrs_len];
  uint ptr_iterator = 0;

  for (size_t i = 0; i < 128; ++i) {
    ptrs[ptr_iterator++] = r_mempool_alloc_entry(rmp, 16);
  }

  for (size_t i = 0; i < 64; ++i) {
    ptrs[ptr_iterator++] = r_mempool_alloc_entry(rmp, 32);
  }

  for (size_t i = 0; i < 32; ++i) {
    ptrs[ptr_iterator++] = r_mempool_alloc_entry(rmp, 64);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size),
               r_mempool_total_capacity(rmp, size));
  }

  // Even if the pre-allocated buffers have been exhausted, as the
  // fallback is enabled, it will allocate more memory from heap
  // to fulfill the ask.
  // Please notice that the r_mempool_dynamic_allocs_count will
  // climb up regardless of the size.
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 0);
  void *tmp_ptr_16 = r_mempool_alloc_entry(rmp, 16);
  REQUIRE_NE((void *)tmp_ptr_16, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 1);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 1);

  void *tmp_ptr_32 = r_mempool_alloc_entry(rmp, 32);
  REQUIRE_NE((void *)tmp_ptr_32, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 2);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 2);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 2);

  void *tmp_ptr_64 = r_mempool_alloc_entry(rmp, 64);
  REQUIRE_NE((void *)tmp_ptr_64, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 3);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 3);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 3);

  for (size_t i = 0; i < ptrs_len; ++i) {
    r_mempool_free_entry(ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size), 0);
  }

  // The r_mempool_dynamic_allocs_count still holds the cumulative
  // dynamic allocation number regardless of the size
  r_mempool_free_entry(tmp_ptr_16);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 2);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 2);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 2);
  r_mempool_free_entry(tmp_ptr_32);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 1);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 1);
  r_mempool_free_entry(tmp_ptr_64);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 0);

  r_mempool_destroy(rmp);
}

TEST(r_mempools, c_try_exhausting_with_fallback_at_last_exhaustion) {
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_at_last_exhaustion, false, NULL, NULL);

  size_t ptrs_len = 224;  // 16: 128, 32: 64, 64: 32 => 128 + 64 + 32 = 224

  void *ptrs[ptrs_len];
  uint ptr_iterator = 0;

  for (size_t i = 0; i < 128; ++i) {
    ptrs[ptr_iterator++] = r_mempool_calloc_entry(rmp, 16);
  }

  for (size_t i = 0; i < 64; ++i) {
    ptrs[ptr_iterator++] = r_mempool_calloc_entry(rmp, 32);
  }

  for (size_t i = 0; i < 32; ++i) {
    ptrs[ptr_iterator++] = r_mempool_calloc_entry(rmp, 64);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size),
               r_mempool_total_capacity(rmp, size));
  }

  // Even if the pre-allocated buffers have been exhausted, as the
  // fallback is enabled, it will allocate more memory from the heap
  // to fulfill the ask.
  // Please notice that the r_mempool_dynamic_allocs_count will
  // climb up regardless of the size.
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 0);
  void *tmp_ptr_16 = r_mempool_calloc_entry(rmp, 16);
  REQUIRE_NE((void *)tmp_ptr_16, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 1);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 1);

  void *tmp_ptr_32 = r_mempool_calloc_entry(rmp, 32);
  REQUIRE_NE((void *)tmp_ptr_32, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 2);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 2);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 2);

  void *tmp_ptr_64 = r_mempool_calloc_entry(rmp, 64);
  REQUIRE_NE((void *)tmp_ptr_64, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 3);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 3);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 3);

  for (size_t i = 0; i < ptrs_len; ++i) {
    r_mempool_free_entry(ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size), 0);
  }

  // The r_mempool_dynamic_allocs_count still holds the cumulative
  // dynamic allocation number regardless of the size
  r_mempool_free_entry(tmp_ptr_16);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 2);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 2);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 2);
  r_mempool_free_entry(tmp_ptr_32);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 1);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 1);
  r_mempool_free_entry(tmp_ptr_64);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 0);

  r_mempool_destroy(rmp);
}

TEST(r_mempools, c_try_exhausting_with_fallback_at_last_exhaustion_no_locks) {
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_at_last_exhaustion, true, NULL, NULL);

  size_t ptrs_len = 224;  // 16: 128, 32: 64, 64: 32 => 128 + 64 + 32 = 224

  void *ptrs[ptrs_len];
  uint ptr_iterator = 0;

  for (size_t i = 0; i < 128; ++i) {
    ptrs[ptr_iterator++] = r_mempool_calloc_entry(rmp, 16);
  }

  for (size_t i = 0; i < 64; ++i) {
    ptrs[ptr_iterator++] = r_mempool_calloc_entry(rmp, 32);
  }

  for (size_t i = 0; i < 32; ++i) {
    ptrs[ptr_iterator++] = r_mempool_calloc_entry(rmp, 64);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size),
               r_mempool_total_capacity(rmp, size));
  }

  // Even if the pre-allocated buffers have been exhausted, as the
  // fallback is enabled, it will allocate more memory from the heap
  // to fulfill the ask.
  // Please notice that the r_mempool_dynamic_allocs_count will
  // climb up regardless of the size.
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 0);
  void *tmp_ptr_16 = r_mempool_calloc_entry(rmp, 16);
  REQUIRE_NE((void *)tmp_ptr_16, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 1);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 1);

  void *tmp_ptr_32 = r_mempool_calloc_entry(rmp, 32);
  REQUIRE_NE((void *)tmp_ptr_32, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 2);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 2);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 2);

  void *tmp_ptr_64 = r_mempool_calloc_entry(rmp, 64);
  REQUIRE_NE((void *)tmp_ptr_64, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 3);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 3);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 3);

  for (size_t i = 0; i < ptrs_len; ++i) {
    r_mempool_free_entry(ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size), 0);
  }

  // The r_mempool_dynamic_allocs_count still holds the cumulative
  // dynamic allocation number regardless of the size
  r_mempool_free_entry(tmp_ptr_16);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 2);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 2);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 2);
  r_mempool_free_entry(tmp_ptr_32);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 1);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 1);
  r_mempool_free_entry(tmp_ptr_64);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 0);

  r_mempool_destroy(rmp);
}

// Preallocated rmempool tests
TEST(r_mempools, create_fails_power_exceeds_size_t_width) {
  // Regression for Bug 6: (size_t)1 << n is UB when n >=
  // sizeof(size_t)*CHAR_BIT. assess_r_mempool_create_inputs must reject all
  // three power parameters that would trigger that shift.
  char *err;

  r_mempool *rmp =
      r_mempool_create(64, 65, 65, fallback_disabled, false, NULL, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  rmp = r_mempool_create(4, 64, 64, fallback_disabled, false, NULL, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  rmp = r_mempool_create(4, 6, 64, fallback_disabled, false, NULL, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// Helpers for largest_size_power_of_two_at_true_maximum_passes_validation
// below: .malloc/.realloc/.free simply forward to the real allocator (used
// only for the tiny, fixed-size ccol_memmgmt_procs_t copy every constructor
// makes), while .calloc unconditionally reports failure without ever
// actually invoking the real calloc(). This is deliberate: the real sub-pool
// allocation this test's own config requires is a calloc() call whose
// count*size genuinely overflows size_t, which plain glibc handles by
// returning NULL (the graceful, documented "ran out of memory" outcome this
// test wants to observe) but which AddressSanitizer's own calloc
// interceptor instead reports as a hard "calloc-overflow" error and aborts
// the process; a real difference in how the two environments treat an
// overflowing calloc call, unrelated to anything this test is trying to
// verify. Routing calloc through this stub sidesteps that difference
// entirely under both plain and sanitized builds.
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

TEST(r_mempools, largest_size_power_of_two_at_true_maximum_passes_validation) {
  // Regression: max_allowed_largest_size used to be computed as SIZE_MAX / 2,
  // which on a 64-bit size_t is 2^63 - 1; one less than the single highest
  // power of two size_t can actually hold (2^63), and one less than this
  // module's own documented "largest size must be <= 2^63 bytes" ceiling.
  // largest_size_power_of_two = 63 was therefore always rejected by the
  // "sizes beyond limits" check, even though it is the exact value the docs
  // promise is valid. No real machine can back a working pool at this scale
  // (the smallest sub-pool alone would need to be 2^63 bytes), so this test
  // only asserts that a create attempt at the true ceiling is no longer
  // rejected AT THE VALIDATION STAGE; it still fails shortly after, for the
  // separate, unavoidable reason that the actual allocation cannot succeed
  // (see the allocator stubs above for why that failure is simulated rather
  // than left to the real allocator's own overflow handling).
  ccol_memmgmt_procs_t m_procs = {.malloc = max_size_test_malloc,
                                  .calloc = max_size_test_calloc,
                                  .realloc = max_size_test_realloc,
                                  .free = max_size_test_free};
  char *err = NULL;
  r_mempool *rmp =
      r_mempool_create(4, 63, 59, fallback_disabled, true, &m_procs, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ(strstr(err, "beyond limits"), NULL);
}

// Regression: CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE used to compute its
// second term as the full, un-reduced product 2 * 2^SC * header_overhead *
// (2^N - 1), dividing by 2^N only at the very end; for a large enough SC/N
// combination that intermediate product overflows size_t and silently wraps
// BEFORE the division could reduce it back into range, corrupting the final
// result; even though the true, fully-reduced value fits in a size_t
// without issue. SS=4, LS=33, SC=30 (N=30) is one such combination.
TEST(r_mempools, calculate_preallocated_buffer_size_no_premature_overflow) {
  const uint8_t ss = 4, ls = 33, sc = 30;

  // Ground truth: the exact same per-tier summation
  // init_preallocated_r_mempool_internal_pools performs at runtime, a
  // fundamentally different algorithm from the macro's closed form (no
  // large intermediate product ever formed), so this is a genuine
  // independent cross-check rather than a restatement of the fix.
  size_t expected = 0;
  size_t esize = (size_t)1 << ss;
  size_t ecount = (size_t)1 << sc;
  for (uint8_t i = 0; i < (uint8_t)(ls - ss + 1); ++i) {
    expected += ecount * (esize + offsetof(__internal_entry_header, next));
    esize *= 2;
    ecount /= 2;
  }

  REQUIRE_EQ(CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE(ss, ls, sc), expected);

  // Confirms these parameters really do exercise the overflow this test
  // guards against: recomputing the pre-fix formula's own multiply-before-
  // divide ordering here, in the same size_t arithmetic it used to run in,
  // yields a different (wrapped, wrong) result than the ground truth above.
  size_t old_formula_second_term =
      (2 * ((size_t)1 << sc) * offsetof(__internal_entry_header, next) *
       (((size_t)1 << (ls - ss + 1)) - 1)) /
      ((size_t)1 << (ls - ss + 1));
  size_t old_formula_result =
      ((size_t)(ls - ss + 1)) * ((size_t)1 << sc) * ((size_t)1 << ss) +
      old_formula_second_term;
  REQUIRE_NE(old_formula_result, expected);
}

TEST(r_mempools, realloc_first_exhaustion_entry_grows) {
  // Regression for Bug A: when a fallback_at_first_exhaustion dynamic entry
  // (elem_is_not_a_pool_member, pool_ptr->extended_elem_size != 0) is
  // reallocated to a larger pool, min_user_size must be bounded by the old
  // slot's user size, not the new requested size.  Before the fix, the copy
  // used new_size bytes from a smaller allocation; a heap over-read caught
  // by Valgrind / ASan.
  r_mempool *rmp = r_mempool_create(4, 6, 7, fallback_at_first_exhaustion,
                                    false, NULL, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  // Exhaust pool 0 completely (128 x 16-byte slots).
  void *fill[128];
  for (size_t i = 0; i < 128; ++i) {
    fill[i] = r_mempool_alloc_entry(rmp, 16);
    REQUIRE_NE(fill[i], NULL);
  }
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 128);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);

  // Next 16-byte request spills to the heap (pool[0] fallback).  This entry
  // is tagged elem_is_not_a_pool_member with pool_ptr == pool[0] and
  // pool_ptr->extended_elem_size encoding a 16-byte user area.
  char *entry = r_mempool_alloc_entry(rmp, 16);
  REQUIRE_NE((void *)entry, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);

  for (int i = 0; i < 16; ++i) {
    entry[i] = (char)(i + 1);
  }

  // Realloc to 32 bytes (pool[1]).  Only the first 16 bytes of the old entry
  // are valid; reading 32 bytes would over-run the original heap allocation.
  char *grown = r_mempool_realloc_entry(rmp, entry, 32);
  REQUIRE_NE((void *)grown, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 1);

  for (int i = 0; i < 16; ++i) {
    REQUIRE_EQ(grown[i], (char)(i + 1));
  }

  r_mempool_free_entry(grown);
  for (size_t i = 0; i < 128; ++i) {
    r_mempool_free_entry(fill[i]);
  }

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, realloc_last_exhaustion_pseudo_pool_entry_shrinks) {
  // Regression: r_mempool_realloc_entry must correctly identify
  // fallback_at_last_exhaustion pseudo_pool entries (pool_ptr->
  // extended_elem_size == 0) and recover their true user-visible size from
  // the dynamic-entry size prefix rather than reading beyond the original
  // allocation. Data written into a pseudo_pool entry must survive a
  // realloc that shrinks it, up to the new (smaller) capacity.
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_at_last_exhaustion, false, NULL, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  // Exhaust all three sub-pools so the next allocation uses the pseudo_pool.
  void *fill[224];
  size_t iter = 0;
  for (size_t i = 0; i < 128; ++i)
    fill[iter++] = r_mempool_alloc_entry(rmp, 16);
  for (size_t i = 0; i < 64; ++i) fill[iter++] = r_mempool_alloc_entry(rmp, 32);
  for (size_t i = 0; i < 32; ++i) fill[iter++] = r_mempool_alloc_entry(rmp, 64);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);

  // Request 32 bytes when all pools are exhausted -> pseudo_pool entry
  // (pool_ptr == &rmp->pseudo_pool, extended_elem_size == 0).
  char *entry = r_mempool_alloc_entry(rmp, 32);
  REQUIRE_NE((void *)entry, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 1);
  for (int i = 0; i < 32; ++i) entry[i] = (char)(i + 1);

  // Shrink to 16 bytes. The old entry is freed; a new one is returned. The
  // first 16 bytes of the original data must be preserved.
  char *shrunk = r_mempool_realloc_entry(rmp, entry, 16);
  REQUIRE_NE((void *)shrunk, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);
  for (int i = 0; i < 16; ++i) {
    REQUIRE_EQ(shrunk[i], (char)(i + 1));
  }

  r_mempool_free_entry(shrunk);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);

  for (size_t i = 0; i < 224; ++i) {
    r_mempool_free_entry(fill[i]);
  }

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, realloc_last_exhaustion_pseudo_pool_entry_grows) {
  // Regression: growing a pseudo_pool entry must not over-read the original
  // allocation, and must preserve the data that genuinely fits. The
  // dynamic-entry size prefix records the original entry's real size (16),
  // so growing to 32 copies exactly those 16 bytes forward and leaves the
  // rest of the new, larger buffer as freshly allocated (uninitialized)
  // memory.
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_at_last_exhaustion, false, NULL, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  // Exhaust all three sub-pools.
  void *fill[224];
  size_t iter = 0;
  for (size_t i = 0; i < 128; ++i)
    fill[iter++] = r_mempool_alloc_entry(rmp, 16);
  for (size_t i = 0; i < 64; ++i) fill[iter++] = r_mempool_alloc_entry(rmp, 32);
  for (size_t i = 0; i < 32; ++i) fill[iter++] = r_mempool_alloc_entry(rmp, 64);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);

  // Allocate 16 bytes from pseudo_pool (all pools full).
  char *entry = r_mempool_alloc_entry(rmp, 16);
  REQUIRE_NE((void *)entry, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);
  for (int i = 0; i < 16; ++i) entry[i] = (char)(i + 1);

  // Grow to 32 bytes. The original 16 bytes must be preserved.
  // fallback_at_last_exhaustion uses a single shared pseudo_pool counter for
  // all sizes, so after the realloc the old 16-byte entry is freed and the
  // new 32-byte entry is live; total pseudo_pool count stays at 1.
  char *grown = r_mempool_realloc_entry(rmp, entry, 32);
  REQUIRE_NE((void *)grown, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 1);
  for (int i = 0; i < 16; ++i) {
    REQUIRE_EQ(grown[i], (char)(i + 1));
  }

  r_mempool_free_entry(grown);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);

  for (size_t i = 0; i < 224; ++i) {
    r_mempool_free_entry(fill[i]);
  }

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

// Regression: r_mempool_realloc_entry's "same size class, return the
// original pointer unchanged" fast path could never trigger for a
// pseudo_pool (fallback_at_last_exhaustion) entry, since
// pseudo_pool.extended_elem_size is always 0 (the pseudo_pool has no fixed
// per-tier size the way a real sub-pool does), while the size computed for
// the fast-path comparison is always a real sub-pool's own nonzero
// extended_elem_size; the two could never be equal. Every realloc of a
// pseudo_pool entry, even to the exact same size it already held, therefore
// always performed a needless alloc-copy-free cycle instead of returning
// addr unchanged. Verifies the pointer (and its contents) are now preserved
// for an exact-size-match request, and that a genuinely different size
// still moves (right-sizing), matching how a real sub-pool entry shrunk far
// enough to cross into a smaller tier also still moves rather than holding
// onto its oversized block.
TEST(r_mempools,
     realloc_last_exhaustion_pseudo_pool_entry_same_size_is_a_noop) {
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_at_last_exhaustion, false, NULL, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  // Exhaust all three sub-pools so further allocations route through the
  // shared pseudo_pool.
  void *fill[224];
  size_t iter = 0;
  for (size_t i = 0; i < 128; ++i)
    fill[iter++] = r_mempool_alloc_entry(rmp, 16);
  for (size_t i = 0; i < 64; ++i) fill[iter++] = r_mempool_alloc_entry(rmp, 32);
  for (size_t i = 0; i < 32; ++i) fill[iter++] = r_mempool_alloc_entry(rmp, 64);

  char *entry = r_mempool_alloc_entry(rmp, 20);
  REQUIRE_NE((void *)entry, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 20), 1);
  for (int i = 0; i < 20; ++i) {
    entry[i] = (char)(i + 1);
  }

  // Requesting the exact same size back must be a true no-op: same pointer,
  // same content, no extra pseudo_pool allocation.
  char *same = r_mempool_realloc_entry(rmp, entry, 20);
  REQUIRE_EQ((void *)same, (void *)entry);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 20), 1);
  for (int i = 0; i < 20; ++i) {
    REQUIRE_EQ(same[i], (char)(i + 1));
  }

  // A genuinely different size must still move.
  char *moved = r_mempool_realloc_entry(rmp, same, 10);
  REQUIRE_NE((void *)moved, (void *)same);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 10), 1);
  for (int i = 0; i < 10; ++i) {
    REQUIRE_EQ(moved[i], (char)(i + 1));
  }

  r_mempool_free_entry(moved);
  for (size_t i = 0; i < 224; ++i) {
    r_mempool_free_entry(fill[i]);
  }

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

// Regression: mempool_pseudo_alloc_entry used to round elem_size up to
// sizeof(addr_t) before recording it in the entry's own dynamic-size
// prefix, so entry_user_size() reported the ROUNDED capacity rather than
// what the caller actually requested. r_mempool_realloc_entry's pseudo_pool
// same-size fast path (see the test right above this one) compares the
// caller's raw requested size directly against that recorded value, so any
// repeated request for the identical size below sizeof(uintptr_t) (e.g. 3
// bytes on every mainstream platform) never matched its own
// previously-rounded prefix (3 != 8) and always took the slow
// allocate-copy-free path instead of returning the original pointer
// unchanged, contradicting r_mempool_realloc_entry's own documented "does
// not require a differently-sized allocation" contract on every such call.
// Fixed by no longer rounding elem_size in mempool_pseudo_alloc_entry at
// all: a pseudo_pool entry is a one-off heap allocation, never linked into
// any free list, so it has no minimum-size requirement the way a
// pool-owned entry (whose free user area doubles as a free-list node) does.
TEST(
    r_mempools,
    realloc_last_exhaustion_pseudo_pool_entry_below_word_size_same_size_is_a_noop) {
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_at_last_exhaustion, false, NULL, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  // Exhaust all three sub-pools so further allocations route through the
  // shared pseudo_pool.
  void *fill[224];
  size_t iter = 0;
  for (size_t i = 0; i < 128; ++i)
    fill[iter++] = r_mempool_alloc_entry(rmp, 16);
  for (size_t i = 0; i < 64; ++i) fill[iter++] = r_mempool_alloc_entry(rmp, 32);
  for (size_t i = 0; i < 32; ++i) fill[iter++] = r_mempool_alloc_entry(rmp, 64);

  char *entry = r_mempool_alloc_entry(rmp, 3); /* < sizeof(uintptr_t) */
  REQUIRE_NE((void *)entry, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 3), 1);
  for (int i = 0; i < 3; ++i) {
    entry[i] = (char)(i + 1);
  }

  // Requesting the exact same sub-word size back must be a true no-op: same
  // pointer, same content, no extra pseudo_pool allocation.
  char *same = r_mempool_realloc_entry(rmp, entry, 3);
  REQUIRE_EQ((void *)same, (void *)entry);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 3), 1);
  for (int i = 0; i < 3; ++i) {
    REQUIRE_EQ(same[i], (char)(i + 1));
  }

  // A genuinely different (still sub-word) size must still move.
  char *moved = r_mempool_realloc_entry(rmp, same, 5);
  REQUIRE_NE((void *)moved, (void *)same);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 5), 1);
  for (int i = 0; i < 3; ++i) {
    REQUIRE_EQ(moved[i], (char)(i + 1));
  }

  r_mempool_free_entry(moved);
  for (size_t i = 0; i < 224; ++i) {
    r_mempool_free_entry(fill[i]);
  }

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, custom_allocator_propagated_to_pseudo_pool) {
  // Regression for Bug 3: init_r_mempool_pseudo_pool did not copy rmp->m_procs
  // into pseudo_pool.m_procs.  Under fallback_at_last_exhaustion, pseudo_pool
  // entries were allocated with NULL m_procs (plain malloc) but freed with the
  // custom allocator; an allocator mismatch detectable by Valgrind.
  ccol_memmgmt_procs_t m_procs = {
      .malloc = malloc, .calloc = calloc, .realloc = realloc, .free = free};

  r_mempool *rmp = r_mempool_create(4, 6, 7, fallback_at_last_exhaustion, false,
                                    &m_procs, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *fill[224];
  size_t iter = 0;
  for (size_t i = 0; i < 128; ++i)
    fill[iter++] = r_mempool_alloc_entry(rmp, 16);
  for (size_t i = 0; i < 64; ++i) fill[iter++] = r_mempool_alloc_entry(rmp, 32);
  for (size_t i = 0; i < 32; ++i) fill[iter++] = r_mempool_alloc_entry(rmp, 64);

  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);

  void *entry = r_mempool_alloc_entry(rmp, 16);
  REQUIRE_NE(entry, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);

  r_mempool_free_entry(entry);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);

  for (size_t i = 0; i < 224; ++i) {
    r_mempool_free_entry(fill[i]);
  }

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, custom_allocator_with_fallback_at_first_exhaustion) {
  // Verifies that r_mempool correctly propagates a custom allocator to each
  // sub-pool under the fallback_at_first_exhaustion policy.  Alloc and free
  // must go through the same custom functions, which Valgrind/ASan would catch
  // if the allocators were mismatched.
  ccol_memmgmt_procs_t m_procs = {
      .malloc = malloc, .calloc = calloc, .realloc = realloc, .free = free};

  r_mempool *rmp = r_mempool_create(4, 6, 7, fallback_at_first_exhaustion,
                                    false, &m_procs, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *fill[128];
  for (size_t i = 0; i < 128; ++i) {
    fill[i] = r_mempool_alloc_entry(rmp, 16);
    REQUIRE_NE(fill[i], NULL);
  }
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 128);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);

  void *dyn = r_mempool_alloc_entry(rmp, 16);
  REQUIRE_NE(dyn, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);

  r_mempool_free_entry(dyn);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);

  for (size_t i = 0; i < 128; ++i) {
    r_mempool_free_entry(fill[i]);
  }
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(preallocated_r_mempools, exhaust_all_fallback_disabled) {
  static DECLARE_PREALLOCATED_RMEMPOOL_BUFFER(
      preallocated_rmp_buffer,  // The name of the buffer.
      4,  // The size of the smallest element in the pool - 2^4 : 16
      6,  // The size of the largest element in the pool - 2^6 : 64
      7   // The number of smallest elements in the pool - 2^7 : 128
  );

  r_mempool *rmp = r_mempool_create_from_preallocated_buffer(
      preallocated_rmp_buffer, sizeof(preallocated_rmp_buffer), 4, 6, 7,
      fallback_disabled, false, NULL, NULL);

  size_t ptrs_len = 224;  // 16: 128, 32: 64, 64: 32 => 128 + 64 + 32 = 224

  void *ptrs[ptrs_len];

  for (size_t i = 0; i < ptrs_len; ++i) {
    ptrs[i] = r_mempool_alloc_entry(rmp, 8);
    REQUIRE_NE((void *)ptrs[i], NULL);
    // It even gives 32 and 64 sized buffers to fulfill our request.
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size),
               r_mempool_total_capacity(rmp, size));
  }

  // It doesn't allocate new memory from the heap, once the pre-allocated
  // buffers are exhausted, that's it, no new memory till some are given
  // back.
  void *tmp_ptr = r_mempool_alloc_entry(rmp, 8);
  REQUIRE_EQ((void *)tmp_ptr, NULL);

  for (size_t i = 0; i < ptrs_len; ++i) {
    r_mempool_free_entry(ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size), 0);
  }

  r_mempool_destroy(rmp);
}

TEST(preallocated_r_mempools, exhaust_all_fallback_disabled_no_locks) {
  static DECLARE_PREALLOCATED_RMEMPOOL_BUFFER(
      preallocated_rmp_buffer,  // The name of the buffer.
      4,  // The size of the smallest element in the pool - 2^4 : 16
      6,  // The size of the largest element in the pool - 2^6 : 64
      7   // The number of smallest elements in the pool - 2^7 : 128
  );

  r_mempool *rmp = r_mempool_create_from_preallocated_buffer(
      preallocated_rmp_buffer, sizeof(preallocated_rmp_buffer), 4, 6, 7,
      fallback_disabled, true, NULL, NULL);

  size_t ptrs_len = 224;  // 16: 128, 32: 64, 64: 32 => 128 + 64 + 32 = 224

  void *ptrs[ptrs_len];

  for (size_t i = 0; i < ptrs_len; ++i) {
    ptrs[i] = r_mempool_alloc_entry(rmp, 8);
    REQUIRE_NE((void *)ptrs[i], NULL);
    // It even gives 32 and 64 sized buffers to fulfill our request.
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size),
               r_mempool_total_capacity(rmp, size));
  }

  // It doesn't allocate new memory from the heap, once the pre-allocated
  // buffers are exhausted, that's it, no new memory till some are given
  // back.
  void *tmp_ptr = r_mempool_alloc_entry(rmp, 8);
  REQUIRE_EQ((void *)tmp_ptr, NULL);

  for (size_t i = 0; i < ptrs_len; ++i) {
    r_mempool_free_entry(ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size), 0);
  }

  r_mempool_destroy(rmp);
}

TEST(r_mempools, calloc_entry_zeroes_memory) {
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_disabled, false, NULL, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  char *p = r_mempool_alloc_entry(rmp, 16);
  REQUIRE_NE((void *)p, NULL);
  memset(p, 0xFF, 16);
  r_mempool_free_entry(p);

  p = r_mempool_calloc_entry(rmp, 16);
  REQUIRE_NE((void *)p, NULL);
  for (size_t i = 0; i < 16; ++i) {
    REQUIRE_EQ(p[i], 0);
  }
  r_mempool_free_entry(p);

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, realloc_null_addr_acts_like_alloc) {
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_disabled, false, NULL, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);
  void *p = r_mempool_realloc_entry(rmp, NULL, 8);
  REQUIRE_NE(p, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 1);

  r_mempool_free_entry(p);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, realloc_returns_null_for_invalid_size) {
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_disabled, false, NULL, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  REQUIRE_EQ((void *)r_mempool_realloc_entry(rmp, NULL, 0), NULL);
  REQUIRE_EQ((void *)r_mempool_realloc_entry(rmp, NULL, 65), NULL);

  void *p = r_mempool_alloc_entry(rmp, 16);
  REQUIRE_NE(p, NULL);
  r_mempool_free_entry(p);

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

// realloc_returns_null_for_invalid_size above only ever exercises an
// invalid size with a NULL addr; it never pins down what happens to a
// genuinely live, non-NULL addr in that same situation. r_mempool_
// realloc_entry deliberately leaves such an addr completely untouched
// (neither freed nor moved), treating an invalid size exactly like any
// other failed reallocation (the original entry stays valid and still
// owned by the caller), never as an implicit free the way some
// realloc(ptr, 0) implementations behave. This locks that contract down
// directly instead of leaving it as an implicit, untested side effect of
// the early "size == 0 || size > largest_size" return.
TEST(r_mempools,
     realloc_invalid_size_with_non_null_addr_leaves_addr_untouched) {
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_disabled, false, NULL, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  char *addr = r_mempool_alloc_entry(rmp, 16);
  REQUIRE_NE((void *)addr, NULL);
  addr[0] = 0x5a;

  REQUIRE_EQ((void *)r_mempool_realloc_entry(rmp, addr, 0), NULL);
  REQUIRE_EQ(addr[0], (char)0x5a);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 1);

  REQUIRE_EQ((void *)r_mempool_realloc_entry(rmp, addr, 65), NULL);
  REQUIRE_EQ(addr[0], (char)0x5a);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 1);

  r_mempool_free_entry(addr);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools,
     realloc_returns_null_when_full_no_fallback_preserves_original) {
  // SS=4, LS=5, SC=4: pool[0] = 16x16-byte, pool[1] = 8x32-byte.
  // When all pools are exhausted and there is no fallback, realloc must
  // return NULL without freeing or modifying the original pointer.
  r_mempool *rmp =
      r_mempool_create(4, 5, 4, fallback_disabled, false, NULL, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  size_t cap16 = r_mempool_total_capacity(rmp, 16);
  size_t cap32 = r_mempool_total_capacity(rmp, 32);
  REQUIRE_EQ(cap16, 16);
  REQUIRE_EQ(cap32, 8);

  void *fill16[16];
  void *fill32[8];

  for (size_t i = 0; i < cap16; ++i) {
    fill16[i] = r_mempool_alloc_entry(rmp, 16);
    REQUIRE_NE(fill16[i], NULL);
  }
  for (size_t i = 0; i < cap32; ++i) {
    fill32[i] = r_mempool_alloc_entry(rmp, 32);
    REQUIRE_NE(fill32[i], NULL);
  }

  void *orig = fill16[0];
  void *result = r_mempool_realloc_entry(rmp, fill16[0], 32);
  REQUIRE_EQ(result, NULL);
  REQUIRE_EQ(fill16[0], orig);

  for (size_t i = 0; i < cap16; ++i) r_mempool_free_entry(fill16[i]);
  for (size_t i = 0; i < cap32; ++i) r_mempool_free_entry(fill32[i]);

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

// r_mempool_realloc_entry must validate the entry header's mempool_mark
// before trusting header->pool_ptr->extended_elem_size, mirroring the
// corruption/foreign-pointer detection _mempool_free_entry already performs.
// Without this check, a foreign or corrupted addr causes an unchecked
// pointer dereference instead of a controlled assert. Run in a forked child
// since fatal_err()/ccol_assert() aborts the whole process.
TEST(r_mempools, realloc_foreign_pointer_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    r_mempool *rmp =
        r_mempool_create(4, 6, 7, fallback_disabled, false, NULL, NULL);
    int junk[8] = {0};
    void *foreign_ptr = &junk[2]; /* never came from this r_mempool */
    r_mempool_realloc_entry(rmp, foreign_ptr, 16);
    _exit(0); /* unreachable if ccol_assert() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// r_mempool_realloc_entry must reject a pointer that is a perfectly valid,
// live entry; just not one that belongs to THIS r_mempool. Before this was
// checked, the only validation performed was that addr's header names *some*
// mempool anywhere in the process sharing the global mempool_mark sentinel
// (every mempool does), which a pointer from a different r_mempool instance
// trivially satisfies; the call would silently succeed instead of asserting,
// contradicting the documented "not obtained from this r_mempool" contract.
TEST(r_mempools, realloc_pointer_from_a_different_r_mempool_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    r_mempool *rmp1 =
        r_mempool_create(4, 6, 7, fallback_disabled, true, NULL, NULL);
    r_mempool *rmp2 =
        r_mempool_create(4, 6, 7, fallback_disabled, true, NULL, NULL);
    void *from_rmp2 = r_mempool_alloc_entry(rmp2, 16);
    r_mempool_realloc_entry(rmp1, from_rmp2, 16); /* wrong rmp */
    _exit(0); /* unreachable if ccol_assert() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// r_mempool_realloc_entry must reject an already-freed addr, even when the
// requested size still maps to the same sub-pool tier the entry originally
// came from. That specific case used to take a fast "return addr unchanged"
// path with no elem_status validation at all, silently handing the caller
// back a node still linked into the pool's own free list; writing through
// it would corrupt the free list itself (verified separately: doing so
// crashes the pool with SIGSEGV on a later, unrelated allocation, not a
// controlled assert). This test only exercises the entry point that must now
// refuse the call outright.
TEST(r_mempools, realloc_already_freed_pointer_same_tier_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    r_mempool *rmp =
        r_mempool_create(4, 6, 7, fallback_disabled, true, NULL, NULL);
    void *a = r_mempool_alloc_entry(rmp, 16);
    r_mempool_alloc_entry(rmp, 16); /* keep a second entry live */
    void *a_saved = a;
    r_mempool_free_entry(a);                  /* a is now free-listed */
    r_mempool_realloc_entry(rmp, a_saved, 8); /* 8 still maps to the 16B tier */
    _exit(0); /* unreachable if ccol_assert() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// Regression: r_mempool_realloc_entry's own doc comment promises it will
// assert on a foreign/corrupted addr "mirroring mempool_free_entry()", but
// the validation used to stop at mempool_mark/entry_belongs_to_rmp/
// elem_status, never confirming (the way __mempool_free_entry's own
// valid_mempool_addr check does for every free) that a pool-owned
// (elem_is_taken) entry's address genuinely lands inside its claimed
// pool's own object buffer. A header that merely LOOKS like a live entry
// of one of rmp's own sub-pools (a real pool_ptr copied straight off a
// genuine entry, elem_is_taken, a correctly matching mempool_mark) but
// whose actual address sits nowhere near that sub-pool's buffer (exactly
// the shape a stale/corrupted header from an unrelated bug elsewhere could
// take) used to be silently accepted instead of caught.
TEST(r_mempools, realloc_valid_looking_header_outside_pool_bounds_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    r_mempool *rmp =
        r_mempool_create(4, 6, 7, fallback_disabled, true, NULL, NULL);
    void *real = r_mempool_alloc_entry(rmp, 16);
    REQUIRE_NE(real, NULL);
    __internal_entry_header *real_header =
        (__internal_entry_header *)((uint8_t *)real -
                                    offsetof(__internal_entry_header, next));

    // Stack-local, fabricated header: a genuine pool_ptr/mempool_mark/
    // elem_is_taken triple copied straight off a real, live entry, but at
    // an address (this local variable's own) nowhere inside that pool's
    // actual object buffer.
    __internal_entry_header fake_header;
    fake_header.elem_status = real_header->elem_status;
    fake_header.pool_ptr = real_header->pool_ptr;
    fake_header.next = NULL;
    void *fake_entry = (void *)&fake_header.next;

    r_mempool_realloc_entry(rmp, fake_entry, 16);
    _exit(0); /* unreachable if ccol_assert() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// Regression, the mirror-image gap: a genuinely pool-owned entry (correctly
// inside its pool's own object buffer) whose elem_status was corrupted from
// elem_is_taken to the exact sentinel value elem_is_not_a_pool_member uses.
// Before the bounds check was added, this satisfied every check the old
// validation performed (a real pool_ptr, a real mempool_mark, elem_status
// in {taken, not_a_pool_member}) and would have been treated as a genuine
// dynamic/fallback entry; eventually freeing it via the allocator's own
// free() on a block that was never independently heap-allocated in the
// first place. The real sentinel value is read off a genuinely dynamic
// entry (produced by exhausting a fallback_at_first_exhaustion sub-pool)
// rather than referencing the library's internal elem_is_not_a_pool_member
// symbol directly, since that symbol is deliberately not exposed via
// cmempool.h.
TEST(
    r_mempools,
    realloc_pool_owned_entry_with_corrupted_not_a_pool_member_status_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    r_mempool *rmp = r_mempool_create(4, 6, 2, fallback_at_first_exhaustion,
                                      true, NULL, NULL);
    size_t cap16 = r_mempool_total_capacity(rmp, 16);
    void *fill[64];
    for (size_t i = 0; i < cap16; ++i) {
      fill[i] = r_mempool_alloc_entry(rmp, 16);
      REQUIRE_NE(fill[i], NULL);
    }

    // The 16B tier is now exhausted; this next allocation is served via
    // that sub-pool's own dynamic fallback, genuinely tagged
    // elem_is_not_a_pool_member.
    void *dynamic_entry = r_mempool_alloc_entry(rmp, 16);
    REQUIRE_NE(dynamic_entry, NULL);
    size_t not_a_pool_member_value =
        *(size_t *)((uint8_t *)dynamic_entry -
                    offsetof(__internal_entry_header, next));

    // Corrupt a genuinely pool-owned (still in-bounds) entry's own status
    // field to that exact value, in place.
    void *pool_owned = fill[0];
    size_t *status_field = (size_t *)((uint8_t *)pool_owned -
                                      offsetof(__internal_entry_header, next));
    *status_field = not_a_pool_member_value;

    r_mempool_realloc_entry(rmp, pool_owned, 16);
    _exit(0); /* unreachable if ccol_assert() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

TEST(r_mempools, realloc_escalated_to_pseudo_pool_does_not_overflow_buffer) {
  // Regression for a real heap buffer overflow, caught directly by
  // AddressSanitizer while this fix was developed: r_mempool_realloc_entry
  // used to compute how many bytes to copy from the *ideal* target tier for
  // the requested size (rounded up to that tier's own, generally larger,
  // capacity) rather than the *actual* new entry that r_mempool_alloc_entry
  // produced. Under fallback_at_last_exhaustion, when both the ideal tier and
  // every larger real tier are exhausted, the actual new entry is served by
  // the pseudo_pool with exactly `size` real bytes, fewer than the ideal
  // tier's own capacity, so mem_cpy wrote past the end of that smaller,
  // genuine allocation. SS=4, LS=6, SC=7: pool0=128x16B, pool1=64x32B,
  // pool2=32x64B.
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_at_last_exhaustion, false, NULL, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  // Fill pool1 (32B tier) completely.
  void *pool1_fill[64];
  for (int i = 0; i < 64; ++i) pool1_fill[i] = r_mempool_alloc_entry(rmp, 32);

  // Fill pool2 (64B tier) completely; keep one entry as the one we shrink.
  void *pool2_fill[32];
  for (int i = 0; i < 32; ++i) pool2_fill[i] = r_mempool_alloc_entry(rmp, 64);
  char *old_entry = (char *)pool2_fill[0];
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), r_mempool_total_capacity(rmp, 32));
  REQUIRE_EQ(r_mempool_used_count(rmp, 64), r_mempool_total_capacity(rmp, 64));

  // Shrink to 17 bytes: the ideal tier (32B, pool1) is full, and the
  // escalation target (64B, pool2) is also full (old_entry itself still
  // counts as taken), so this must land in the pseudo_pool with exactly 17
  // real bytes.
  char *shrunk = r_mempool_realloc_entry(rmp, old_entry, 17);
  REQUIRE_NE((void *)shrunk, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 17), 1);

  // Writing the full, actual 17-byte capacity must not corrupt anything
  // adjacent (verified under make memtest / ASan; this assignment is itself
  // the regression check for a plain, unsanitized run too, since the prior
  // bug's 15-byte overflow would corrupt heap bookkeeping malloc/free would
  // eventually notice).
  memset(shrunk, 0x5a, 17);
  for (int i = 0; i < 17; ++i) {
    REQUIRE_EQ(shrunk[i], (char)0x5a);
  }

  r_mempool_free_entry(shrunk);
  for (int i = 0; i < 64; ++i) r_mempool_free_entry(pool1_fill[i]);
  for (int i = 1; i < 32; ++i) r_mempool_free_entry(pool2_fill[i]);

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

// Regression, r_mempool side of the same fix as
// cmempools.double_free_of_dynamic_entry_with_another_still_live_is_fatal:
// a pseudo_pool entry (fallback_at_last_exhaustion) is freed through the
// exact same __mempool_free_entry function a plain mempool's own dynamic
// fallback entries are, so it must get the identical protection. Uses the
// same no-clobber allocator so the assertion is exercised deterministically
// rather than relying on a particular libc's own free-list poisoning.
TEST(r_mempools,
     double_free_of_pseudo_pool_entry_with_another_still_live_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    ccol_memmgmt_procs_t procs = {.malloc = no_clobber_malloc,
                                  .calloc = no_clobber_calloc,
                                  .realloc = no_clobber_realloc,
                                  .free = no_clobber_free};
    r_mempool *rmp = r_mempool_create(4, 6, 7, fallback_at_last_exhaustion,
                                      true, &procs, NULL);

    // Exhaust all three sub-pools so further allocations route through the
    // shared pseudo_pool. The filled entries are deliberately never freed
    // (the process aborts before reaching any cleanup code below), so
    // there is no need to keep their pointers around past this loop.
    for (size_t i = 0; i < 128; ++i) r_mempool_alloc_entry(rmp, 16);
    for (size_t i = 0; i < 64; ++i) r_mempool_alloc_entry(rmp, 32);
    for (size_t i = 0; i < 32; ++i) r_mempool_alloc_entry(rmp, 64);

    /* Two pseudo_pool entries outstanding at once. */
    void *e1 = r_mempool_alloc_entry(rmp, 16);
    void *e2 = r_mempool_alloc_entry(rmp, 32);
    (void)e2; /* kept alive; never freed by this test */
    void *e1_saved = e1;
    r_mempool_free_entry(e1);      /* first free: fine */
    _mempool_free_entry(e1_saved); /* second free, e2 still live: fatal */
    _exit(0); /* unreachable if ccol_assert() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// Regression: r_mempool_destroy's own documented leak-detection contract had
// no coverage for the fallback_at_first_exhaustion policy, where the leak
// check is performed implicitly, once per sub-pool, by each sub-pool's own
// mempool_destroy() call inside _r_mempool_destroy's teardown loop (there is
// no separate, r_mempool-level check for this policy the way there is for
// fallback_at_last_exhaustion's pseudo_pool). A regression that broke that
// per-sub-pool propagation would go unnoticed by the rest of this suite.
TEST(r_mempools, destroy_with_leaked_first_exhaustion_entry_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    r_mempool *rmp = r_mempool_create(4, 6, 7, fallback_at_first_exhaustion,
                                      true, NULL, NULL);
    for (size_t i = 0; i < 128; ++i) r_mempool_alloc_entry(rmp, 16);
    void *leaked = r_mempool_alloc_entry(rmp, 16); /* spills to pool[0]'s own
                                                        fallback */
    (void)leaked;                                  /* never freed */
    r_mempool_destroy(rmp);                        /* must assert on the leak */
    _exit(0); /* unreachable if ccol_assert() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// Regression: the same leak-detection contract for fallback_at_last_
// exhaustion had no coverage either. This exercises the explicit
// mempool_dynamic_allocs_count(&rmp->pseudo_pool) > 0 check at the top of
// _r_mempool_destroy directly.
TEST(r_mempools,
     destroy_with_leaked_last_exhaustion_pseudo_pool_entry_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    r_mempool *rmp = r_mempool_create(4, 6, 7, fallback_at_last_exhaustion,
                                      true, NULL, NULL);
    for (size_t i = 0; i < 128; ++i) r_mempool_alloc_entry(rmp, 16);
    for (size_t i = 0; i < 64; ++i) r_mempool_alloc_entry(rmp, 32);
    for (size_t i = 0; i < 32; ++i) r_mempool_alloc_entry(rmp, 64);
    void *leaked = r_mempool_alloc_entry(rmp, 16); /* all tiers full, spills
                                                        to the pseudo_pool */
    (void)leaked;                                  /* never freed */
    r_mempool_destroy(rmp);                        /* must assert on the leak */
    _exit(0); /* unreachable if ccol_assert() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

TEST(r_mempools, create_fails_smallest_size_below_minimum) {
  // SS=3 => smallest_size=8 < min_allowed_smallest_size=16, must fail.
  char *err;
  r_mempool *rmp =
      r_mempool_create(3, 10, 10, fallback_disabled, false, NULL, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(r_mempools, create_preallocated_buffer_fails) {
  // SS=4, LS=5, SC=4: correct buffer size is 896 bytes.
  DECLARE_PREALLOCATED_RMEMPOOL_BUFFER(correct_buf, 4, 5, 4);
  char *err;

  r_mempool *rmp = r_mempool_create_from_preallocated_buffer(
      NULL, sizeof(correct_buf), 4, 5, 4, fallback_disabled, false, NULL, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  _Alignas(__internal_entry_header) uint8_t small_buf[sizeof(correct_buf) - 1];
  rmp = r_mempool_create_from_preallocated_buffer(small_buf, sizeof(small_buf),
                                                  4, 5, 4, fallback_disabled,
                                                  false, NULL, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  _Alignas(__internal_entry_header) uint8_t large_buf[sizeof(correct_buf) + 1];
  rmp = r_mempool_create_from_preallocated_buffer(large_buf, sizeof(large_buf),
                                                  4, 5, 4, fallback_disabled,
                                                  false, NULL, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  // Invalid params: SS >= LS.
  rmp = r_mempool_create_from_preallocated_buffer(
      correct_buf, sizeof(correct_buf), 5, 4, 4, fallback_disabled, false, NULL,
      &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// Regression: r_mempool_create_from_preallocated_buffer must reject a
// misaligned top-level buffer up front, mirroring
// mempool_create_from_preallocated_buffer's own equivalent check, rather than
// letting it surface later (or not at all) from deep inside per-sub-pool
// construction.
TEST(r_mempools, create_preallocated_buffer_fails_misaligned) {
  _Alignas(__internal_entry_header) uint8_t buf[897];  // 896 + 1
  char *err = NULL;
  r_mempool *rmp = r_mempool_create_from_preallocated_buffer(
      buf + 1, sizeof(buf) - 1, 4, 5, 4, fallback_disabled, false, NULL, &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(preallocated_r_mempools, exhaust_all_fallback_at_first_exhaustion) {
  static DECLARE_PREALLOCATED_RMEMPOOL_BUFFER(buf, 4, 6, 7);

  r_mempool *rmp = r_mempool_create_from_preallocated_buffer(
      buf, sizeof(buf), 4, 6, 7, fallback_at_first_exhaustion, false, NULL,
      NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *fill[224];
  size_t iter = 0;
  for (size_t i = 0; i < 128; ++i)
    fill[iter++] = r_mempool_alloc_entry(rmp, 16);
  for (size_t i = 0; i < 64; ++i) fill[iter++] = r_mempool_alloc_entry(rmp, 32);
  for (size_t i = 0; i < 32; ++i) fill[iter++] = r_mempool_alloc_entry(rmp, 64);

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size),
               r_mempool_total_capacity(rmp, size));
  }

  void *dyn16 = r_mempool_alloc_entry(rmp, 16);
  REQUIRE_NE(dyn16, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);

  void *dyn32 = r_mempool_alloc_entry(rmp, 32);
  REQUIRE_NE(dyn32, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 1);

  r_mempool_free_entry(dyn16);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);

  r_mempool_free_entry(dyn32);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);

  for (size_t i = 0; i < 224; ++i) r_mempool_free_entry(fill[i]);

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size), 0);
  }

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(preallocated_r_mempools, exhaust_all_fallback_at_last_exhaustion) {
  static DECLARE_PREALLOCATED_RMEMPOOL_BUFFER(buf, 4, 6, 7);

  r_mempool *rmp = r_mempool_create_from_preallocated_buffer(
      buf, sizeof(buf), 4, 6, 7, fallback_at_last_exhaustion, false, NULL,
      NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *fill[224];
  size_t iter = 0;
  for (size_t i = 0; i < 128; ++i)
    fill[iter++] = r_mempool_alloc_entry(rmp, 16);
  for (size_t i = 0; i < 64; ++i) fill[iter++] = r_mempool_alloc_entry(rmp, 32);
  for (size_t i = 0; i < 32; ++i) fill[iter++] = r_mempool_alloc_entry(rmp, 64);

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size),
               r_mempool_total_capacity(rmp, size));
  }

  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);

  void *dyn16 = r_mempool_alloc_entry(rmp, 16);
  REQUIRE_NE(dyn16, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 1);

  r_mempool_free_entry(dyn16);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);

  for (size_t i = 0; i < 224; ++i) r_mempool_free_entry(fill[i]);

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, query_functions_return_zero_for_invalid_sizes) {
  // r_mempool_used_count, r_mempool_total_capacity, and
  // r_mempool_dynamic_allocs_count must return 0 for size=0 and
  // size > largest_size, per their documented contracts.
  r_mempool *rmp = r_mempool_create(4, 6, 7, fallback_at_first_exhaustion,
                                    false, NULL, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  REQUIRE_EQ(r_mempool_used_count(rmp, 0), 0);
  REQUIRE_EQ(r_mempool_total_capacity(rmp, 0), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 0), 0);

  REQUIRE_EQ(r_mempool_used_count(rmp, 65), 0);
  REQUIRE_EQ(r_mempool_total_capacity(rmp, 65), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 65), 0);

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(cmempools, create_small_elem_size_is_bumped_to_min) {
  // elem_size < sizeof(uintptr_t) must be silently bumped to sizeof(uintptr_t).
  // This exercises the `else if (elem_size < sizeof(addr_t))` branch in
  // mempool_create that is otherwise unreachable from normal test paths.
  char *err = NULL;
  mempool *mp = mempool_create(8, 1, false, false, NULL, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)mp, NULL);
  REQUIRE_EQ(mempool_total_capacity(mp), 8);
  REQUIRE_EQ(mempool_used_count(mp), 0);

  void *ptrs[8];
  for (size_t i = 0; i < 8; ++i) {
    ptrs[i] = mempool_alloc_entry(mp);
    REQUIRE_NE((void *)ptrs[i], NULL);
    REQUIRE_EQ(mempool_used_count(mp), i + 1);
  }
  REQUIRE_EQ((void *)mempool_alloc_entry(mp), NULL);

  for (size_t i = 0; i < 8; ++i) {
    mempool_free_entry(ptrs[i]);
  }
  REQUIRE_EQ(mempool_used_count(mp), 0);

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

TEST(r_mempools, dynamic_allocs_count_always_zero_when_fallback_disabled) {
  // r_mempool_dynamic_allocs_count must return 0 for any valid size when
  // the pool was created with fallback_disabled, even after allocations.
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_disabled, false, NULL, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 0);

  void *p = r_mempool_alloc_entry(rmp, 16);
  REQUIRE_NE(p, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 0);

  r_mempool_free_entry(p);

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, calloc_entry_zeroes_fallback_at_first_exhaustion) {
  // r_mempool_calloc_entry must zero exactly the requested number of bytes
  // when the allocation comes from the heap fallback
  // (fallback_at_first_exhaustion).
  r_mempool *rmp = r_mempool_create(4, 6, 7, fallback_at_first_exhaustion,
                                    false, NULL, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  // Exhaust pool[0] (128 x 16-byte slots).
  void *fill[128];
  for (size_t i = 0; i < 128; ++i) {
    fill[i] = r_mempool_alloc_entry(rmp, 16);
    REQUIRE_NE(fill[i], NULL);
    memset(fill[i], 0xFF, 16);
  }
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 128);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);

  // The next calloc must come from the heap fallback and be fully zeroed.
  char *p = r_mempool_calloc_entry(rmp, 16);
  REQUIRE_NE((void *)p, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);
  for (size_t i = 0; i < 16; ++i) {
    REQUIRE_EQ(p[i], 0);
  }

  r_mempool_free_entry(p);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);

  for (size_t i = 0; i < 128; ++i) {
    r_mempool_free_entry(fill[i]);
  }
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, calloc_entry_zeroes_fallback_at_last_exhaustion) {
  // r_mempool_calloc_entry must zero exactly the requested number of bytes
  // when the allocation comes from the pseudo_pool
  // (fallback_at_last_exhaustion).
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_at_last_exhaustion, false, NULL, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  // Exhaust all three sub-pools.
  void *fill[224];
  size_t iter = 0;
  for (size_t i = 0; i < 128; ++i)
    fill[iter++] = r_mempool_alloc_entry(rmp, 16);
  for (size_t i = 0; i < 64; ++i) fill[iter++] = r_mempool_alloc_entry(rmp, 32);
  for (size_t i = 0; i < 32; ++i) fill[iter++] = r_mempool_alloc_entry(rmp, 64);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);

  // The next calloc must come from the pseudo_pool and be fully zeroed.
  char *p = r_mempool_calloc_entry(rmp, 32);
  REQUIRE_NE((void *)p, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 1);
  for (size_t i = 0; i < 32; ++i) {
    REQUIRE_EQ(p[i], 0);
  }

  r_mempool_free_entry(p);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);

  for (size_t i = 0; i < 224; ++i) {
    r_mempool_free_entry(fill[i]);
  }

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools, try_exhausting_with_fallback_at_first_exhaustion_no_locks) {
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_at_first_exhaustion, true, NULL, NULL);

  size_t ptrs_len = 224;  // 16: 128, 32: 64, 64: 32 => 128 + 64 + 32 = 224

  void *ptrs[ptrs_len];
  uint ptr_iterator = 0;

  for (size_t i = 0; i < 128; ++i) {
    ptrs[ptr_iterator++] = r_mempool_alloc_entry(rmp, 16);
  }

  for (size_t i = 0; i < 64; ++i) {
    ptrs[ptr_iterator++] = r_mempool_alloc_entry(rmp, 32);
  }

  for (size_t i = 0; i < 32; ++i) {
    ptrs[ptr_iterator++] = r_mempool_alloc_entry(rmp, 64);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size),
               r_mempool_total_capacity(rmp, size));
  }

  void *tmp_ptr_16 = r_mempool_alloc_entry(rmp, 16);
  REQUIRE_NE((void *)tmp_ptr_16, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);

  void *tmp_ptr_32 = r_mempool_alloc_entry(rmp, 32);
  REQUIRE_NE((void *)tmp_ptr_32, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 1);

  void *tmp_ptr_64 = r_mempool_alloc_entry(rmp, 64);
  REQUIRE_NE((void *)tmp_ptr_64, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 1);

  for (size_t i = 0; i < ptrs_len; ++i) {
    r_mempool_free_entry(ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size), 0);
  }

  r_mempool_free_entry(tmp_ptr_16);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);

  r_mempool_free_entry(tmp_ptr_32);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);

  r_mempool_free_entry(tmp_ptr_64);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 0);

  r_mempool_destroy(rmp);
}

TEST(r_mempools, try_exhausting_with_fallback_at_last_exhaustion_no_locks) {
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_at_last_exhaustion, true, NULL, NULL);

  size_t ptrs_len = 224;  // 16: 128, 32: 64, 64: 32 => 128 + 64 + 32 = 224

  void *ptrs[ptrs_len];
  uint ptr_iterator = 0;

  for (size_t i = 0; i < 128; ++i) {
    ptrs[ptr_iterator++] = r_mempool_alloc_entry(rmp, 16);
  }

  for (size_t i = 0; i < 64; ++i) {
    ptrs[ptr_iterator++] = r_mempool_alloc_entry(rmp, 32);
  }

  for (size_t i = 0; i < 32; ++i) {
    ptrs[ptr_iterator++] = r_mempool_alloc_entry(rmp, 64);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size),
               r_mempool_total_capacity(rmp, size));
  }

  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 0);

  void *tmp_ptr_16 = r_mempool_alloc_entry(rmp, 16);
  REQUIRE_NE((void *)tmp_ptr_16, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 1);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 1);

  void *tmp_ptr_32 = r_mempool_alloc_entry(rmp, 32);
  REQUIRE_NE((void *)tmp_ptr_32, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 2);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 2);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 64), 2);

  for (size_t i = 0; i < ptrs_len; ++i) {
    r_mempool_free_entry(ptrs[i]);
  }

  for (size_t size = 16; size <= 64; size *= 2) {
    REQUIRE_EQ(r_mempool_used_count(rmp, size), 0);
  }

  r_mempool_free_entry(tmp_ptr_16);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);
  r_mempool_free_entry(tmp_ptr_32);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);

  r_mempool_destroy(rmp);
}

TEST(preallocated_r_mempools, custom_allocator_propagated) {
  // Verifies that r_mempool_create_from_preallocated_buffer correctly
  // propagates a custom allocator to sub-pool structs and the reverse lookup
  // array, so all heap allocations and frees go through the same functions.
  ccol_memmgmt_procs_t m_procs = {
      .malloc = malloc, .calloc = calloc, .realloc = realloc, .free = free};

  static DECLARE_PREALLOCATED_RMEMPOOL_BUFFER(buf, 4, 6, 7);

  r_mempool *rmp = r_mempool_create_from_preallocated_buffer(
      buf, sizeof(buf), 4, 6, 7, fallback_at_first_exhaustion, false, &m_procs,
      NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *fill[128];
  for (size_t i = 0; i < 128; ++i) {
    fill[i] = r_mempool_alloc_entry(rmp, 16);
    REQUIRE_NE(fill[i], NULL);
  }
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 128);

  void *dyn = r_mempool_alloc_entry(rmp, 16);
  REQUIRE_NE(dyn, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);

  r_mempool_free_entry(dyn);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);

  for (size_t i = 0; i < 128; ++i) {
    r_mempool_free_entry(fill[i]);
  }
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

TEST(r_mempools,
     fallback_at_last_exhaustion_escalation_not_counted_as_fallback) {
  // SS=4, LS=6, SC=7: pool[0]=128x16-byte, pool[1]=64x32-byte.
  // Under fallback_at_last_exhaustion, escalation from pool[0] to pool[1]
  // must NOT increment the pseudo_pool counter; it is only a reallocation
  // within the preallocated budget, not a heap fallback.
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_at_last_exhaustion, false, NULL, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *fill[128];
  for (size_t i = 0; i < 128; ++i) {
    fill[i] = r_mempool_alloc_entry(rmp, 16);
    REQUIRE_NE(fill[i], NULL);
  }
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 128);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);

  // pool[0] is full; this request escalates to pool[1], no pseudo_pool.
  void *escalated = r_mempool_alloc_entry(rmp, 8);
  REQUIRE_NE(escalated, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 1);

  r_mempool_free_entry(escalated);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);

  for (size_t i = 0; i < 128; ++i) r_mempool_free_entry(fill[i]);
  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

// Regression: DECLARE_PREALLOCATED_RMEMPOOL_BUFFER now rejects, at compile
// time via the internal _ccol_rmempool_buffer_params_fit guard, any
// (smallest_size_power_of_two, largest_size_power_of_two,
// number_of_smallest_size_elems_power_of_two) combination that would make
// CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE's own arithmetic overflow
// size_t, or that violates the "SC >= LS - SS" precondition
// CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE's own division needs to be
// mathematically exact; closing a gap DECLARE_PREALLOCATED_MEMPOOL_
// BUFFER's own sibling _Static_assert already had. A genuinely rejected
// combination can't be fed into DECLARE_PREALLOCATED_RMEMPOOL_BUFFER
// itself without failing to compile this whole test binary, so the guard's
// boolean logic is exercised directly here instead, cross-checked against
// cases this file already knows the runtime accepts or rejects for the
// identical underlying reason.
TEST(preallocated_r_mempools, buffer_params_fit_matches_known_outcomes) {
  // The exact overflow case
  // create_rejects_configuration_whose_true_buffer_size_overflows proves
  // the runtime rejects (SS=4, LS=63, SC=62).
  REQUIRE_FALSE(_ccol_rmempool_buffer_params_fit(4, 63, 62));

  // SC < LS - SS: violates the exact-division precondition even though no
  // individual term overflows on its own.
  REQUIRE_FALSE(_ccol_rmempool_buffer_params_fit(4, 6, 1));

  // LS <= SS is not a valid pool shape at all (no room for even one tier
  // above the smallest).
  REQUIRE_FALSE(_ccol_rmempool_buffer_params_fit(8, 4, 10));
  REQUIRE_FALSE(_ccol_rmempool_buffer_params_fit(6, 6, 10));

  // Every (SS, LS, SC) triple already used by a real
  // DECLARE_PREALLOCATED_RMEMPOOL_BUFFER call site elsewhere in this file
  // must be accepted; if it weren't, this whole binary would already have
  // failed to compile. Re-asserted here directly so a regression that
  // narrows the guard shows up as an ordinary test failure rather than only
  // ever as a build break someone has to bisect.
  REQUIRE_TRUE(_ccol_rmempool_buffer_params_fit(4, 6, 7));
  REQUIRE_TRUE(_ccol_rmempool_buffer_params_fit(4, 16, 12));
  REQUIRE_TRUE(_ccol_rmempool_buffer_params_fit(4, 5, 4));

  // The large-but-genuinely-fitting case
  // calculate_preallocated_buffer_size_no_premature_overflow exercises
  // directly against CALCULATE_PREALLOCATED_RMEMPOOL_BUFFER_SIZE must also
  // be accepted by this guard, but only on a platform wide enough to fit it:
  // LS=33 alone already exceeds a 32-bit size_t's own 32-bit width (the
  // macro's own first sub-condition, `(size_t)(LS) < sizeof(size_t) *
  // CHAR_BIT`), so on a platform like i386 this triple is correctly,
  // genuinely rejected rather than accepted; not a bug in the guard, a
  // real difference in what "genuinely fitting" means on that platform.
#if SIZE_MAX > 0xFFFFFFFFu
  REQUIRE_TRUE(_ccol_rmempool_buffer_params_fit(4, 33, 30));
#else
  REQUIRE_FALSE(_ccol_rmempool_buffer_params_fit(4, 33, 30));
#endif

  // The tightest possible valid shape, SC == LS - SS exactly (mirrors
  // create_succeeds_with_minimum_sc's own boundary).
  REQUIRE_TRUE(_ccol_rmempool_buffer_params_fit(4, 6, 2));
}

// Regression: the new _Static_assert must not break the calling convention
// every other DECLARE_PREALLOCATED_RMEMPOOL_BUFFER call site in this file
// relies on (a `static` storage-class prefix at function scope), so the
// buffer gets static rather than stack storage duration. A naive placement
// of the guard's _Static_assert ahead of the array declaration would have
// made that `static` bind to the assertion statement instead of the array
// declaration, a hard compile error for every one of those call sites;
// this test compiling and passing at all, at the exact minimum-SC boundary
// _ccol_rmempool_buffer_params_fit's own test above only checks in the
// abstract, is the regression test.
TEST(preallocated_r_mempools,
     declare_with_static_prefix_at_minimum_sc_boundary) {
  static DECLARE_PREALLOCATED_RMEMPOOL_BUFFER(min_sc_buf, 4, 6, 2);
  r_mempool *rmp = r_mempool_create_from_preallocated_buffer(
      min_sc_buf, sizeof(min_sc_buf), 4, 6, 2, fallback_disabled, false, NULL,
      NULL);
  REQUIRE_NE((void *)rmp, NULL);

  void *p = r_mempool_alloc_entry(rmp, 64);
  REQUIRE_NE(p, NULL);
  REQUIRE_EQ(r_mempool_used_count(rmp, 64), 1);
  r_mempool_free_entry(p);

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

// C_MEMPOOL / C_R_MEMPOOL CONCURRENCY STRESS TESTS
//
// Neither mempool nor r_mempool had any test exercising genuinely
// concurrent access before this, despite both being documented as
// thread-safe when created with single_threaded=false. Failures are
// recorded into each thread's own argument struct and checked from the
// main thread after every worker has joined, rather than calling a tau
// REQUIRE_* macro from a non-main thread (matching the pattern already
// established by tests/clrucache/tests.c's own concurrency suite).

#define MP_STRESS_THREADS 8
#define MP_STRESS_ITERS 2000

typedef struct {
  mempool *mp;
  int thread_idx;
  bool ok;
} mp_stress_arg_t;

static void *mp_stress_worker(void *arg) {
  mp_stress_arg_t *a = (mp_stress_arg_t *)arg;
  a->ok = true;
  for (int i = 0; i < MP_STRESS_ITERS; ++i) {
    int64_t *slot = (int64_t *)mempool_alloc_entry(a->mp);
    if (!slot) {
      /* fallback_to_dynamic_memory is enabled below, so this can only mean
       * genuine system OOM or a corrupted pool. */
      a->ok = false;
      break;
    }
    int64_t marker = (int64_t)a->thread_idx * 1000000 + i;
    *slot = marker;
    /* Read back immediately: if a concurrent alloc on another thread
     * incorrectly handed out this exact same slot, that thread's own write
     * would have clobbered ours by now. */
    if (*slot != marker) {
      a->ok = false;
      mempool_free_entry(slot);
      break;
    }
    mempool_free_entry(slot);
  }
  return NULL;
}

TEST(cmempools, concurrent_alloc_free_stress) {
  // Deliberately small (64 real slots) relative to MP_STRESS_THREADS (8)
  // and MP_STRESS_ITERS (2000 each), so there is genuine cross-thread
  // contention on the free list and the dynamic fallback path is
  // exercised too, not just uncontended single-thread churn.
  mempool *mp = mempool_create(64, sizeof(int64_t), true, false, NULL, NULL);
  REQUIRE_NE((void *)mp, NULL);

  pthread_t tids[MP_STRESS_THREADS];
  mp_stress_arg_t args[MP_STRESS_THREADS];
  for (int i = 0; i < MP_STRESS_THREADS; ++i) {
    args[i].mp = mp;
    args[i].thread_idx = i;
    args[i].ok = false;
    REQUIRE_EQ(pthread_create(&tids[i], NULL, mp_stress_worker, &args[i]), 0);
  }
  for (int i = 0; i < MP_STRESS_THREADS; ++i) {
    REQUIRE_EQ(pthread_join(tids[i], NULL), 0);
  }
  for (int i = 0; i < MP_STRESS_THREADS; ++i) {
    REQUIRE_TRUE(args[i].ok);
  }

  REQUIRE_EQ(mempool_used_count(mp), 0);
  REQUIRE_EQ(mempool_dynamic_allocs_count(mp), 0);
  REQUIRE_EQ(mempool_total_capacity(mp), 64);

  mempool_destroy(mp);
  REQUIRE_EQ((void *)mp, NULL);
}

#define RMP_STRESS_THREADS 8
#define RMP_STRESS_ITERS 1500

typedef struct {
  r_mempool *rmp;
  int thread_idx;
  bool ok;
} rmp_stress_arg_t;

static void *rmp_stress_worker(void *arg) {
  rmp_stress_arg_t *a = (rmp_stress_arg_t *)arg;
  static const size_t sizes[] = {8, 20, 50}; /* -> 16B / 32B / 64B tiers */
  a->ok = true;
  for (int i = 0; i < RMP_STRESS_ITERS; ++i) {
    size_t size = sizes[i % 3];
    int64_t *p = (int64_t *)r_mempool_alloc_entry(a->rmp, size);
    if (!p) {
      a->ok = false;
      break;
    }
    int64_t marker = (int64_t)a->thread_idx * 1000000 + i;
    *p = marker;
    if (*p != marker) {
      a->ok = false;
      r_mempool_free_entry(p);
      break;
    }
    r_mempool_free_entry(p);
  }
  return NULL;
}

TEST(r_mempools, concurrent_alloc_free_stress) {
  // SS=4, LS=6, SC=4: pool0=16x16B, pool1=8x32B, pool2=4x64B; small
  // relative to RMP_STRESS_THREADS so real cross-thread contention, tier
  // escalation, and the pseudo_pool fallback (fallback_at_last_exhaustion)
  // are all genuinely exercised, not just single-threaded free-list churn.
  r_mempool *rmp =
      r_mempool_create(4, 6, 4, fallback_at_last_exhaustion, false, NULL, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  pthread_t tids[RMP_STRESS_THREADS];
  rmp_stress_arg_t args[RMP_STRESS_THREADS];
  for (int i = 0; i < RMP_STRESS_THREADS; ++i) {
    args[i].rmp = rmp;
    args[i].thread_idx = i;
    args[i].ok = false;
    REQUIRE_EQ(pthread_create(&tids[i], NULL, rmp_stress_worker, &args[i]), 0);
  }
  for (int i = 0; i < RMP_STRESS_THREADS; ++i) {
    REQUIRE_EQ(pthread_join(tids[i], NULL), 0);
  }
  for (int i = 0; i < RMP_STRESS_THREADS; ++i) {
    REQUIRE_TRUE(args[i].ok);
  }

  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 64), 0);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 0);

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}

#define RMP_REALLOC_STRESS_THREADS 6
#define RMP_REALLOC_STRESS_ITERS 800

typedef struct {
  r_mempool *rmp;
  int thread_idx;
  bool ok;
} rmp_realloc_stress_arg_t;

static void *rmp_realloc_stress_worker(void *arg) {
  rmp_realloc_stress_arg_t *a = (rmp_realloc_stress_arg_t *)arg;
  a->ok = true;
  for (int i = 0; i < RMP_REALLOC_STRESS_ITERS; ++i) {
    // Each thread only ever touches its own, exclusively-owned pointer;
    // concurrently freeing/reallocating the SAME pointer from two threads
    // is not a supported usage pattern for any pointer-based allocator,
    // this one included, so that scenario is deliberately not exercised
    // here.
    size_t size = 8 + (size_t)(i % 3) * 16; /* 8, 24, 40 -> 16/32/64 tiers */
    char *p = (char *)r_mempool_alloc_entry(a->rmp, size);
    if (!p) {
      a->ok = false;
      break;
    }
    p[0] = (char)(a->thread_idx & 0x7f);

    size_t grown_size = 8 + (size_t)((i + 1) % 3) * 16;
    p = (char *)r_mempool_realloc_entry(a->rmp, p, grown_size);
    if (!p) {
      a->ok = false;
      break;
    }
    if (p[0] != (char)(a->thread_idx & 0x7f)) {
      a->ok = false;
      r_mempool_free_entry(p);
      break;
    }
    r_mempool_free_entry(p);
  }
  return NULL;
}

TEST(r_mempools, concurrent_realloc_stress) {
  r_mempool *rmp =
      r_mempool_create(4, 6, 4, fallback_at_last_exhaustion, false, NULL, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  pthread_t tids[RMP_REALLOC_STRESS_THREADS];
  rmp_realloc_stress_arg_t args[RMP_REALLOC_STRESS_THREADS];
  for (int i = 0; i < RMP_REALLOC_STRESS_THREADS; ++i) {
    args[i].rmp = rmp;
    args[i].thread_idx = i;
    args[i].ok = false;
    REQUIRE_EQ(
        pthread_create(&tids[i], NULL, rmp_realloc_stress_worker, &args[i]), 0);
  }
  for (int i = 0; i < RMP_REALLOC_STRESS_THREADS; ++i) {
    REQUIRE_EQ(pthread_join(tids[i], NULL), 0);
  }
  for (int i = 0; i < RMP_REALLOC_STRESS_THREADS; ++i) {
    REQUIRE_TRUE(args[i].ok);
  }

  REQUIRE_EQ(r_mempool_used_count(rmp, 16), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 32), 0);
  REQUIRE_EQ(r_mempool_used_count(rmp, 64), 0);

  r_mempool_destroy(rmp);
  REQUIRE_EQ((void *)rmp, NULL);
}
