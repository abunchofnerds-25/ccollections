#include <cmempool.h>
#include <stdlib.h>
#include <string.h>
#include <tau/tau.h>
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

TEST(cmempools, create_from_preallocated_fails_small_elem_size) {
  uint8_t buf[256];
  char *err;
  mempool *mp = mempool_create_from_preallocated_buffer(
      buf, sizeof(buf), sizeof(uintptr_t) - 1, false, false, NULL, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(cmempools, create_from_preallocated_fails_elem_count_zero) {
  // A buffer exactly sizeof(__internal_entry_header) bytes passes the initial
  // size guard but cannot hold even one element of size 64 (extended_elem_size
  // = 64 + offsetof(header, next) > sizeof(header)).  The function must return
  // NULL with the "calculated elem_count is zero" error.
  uint8_t buf[sizeof(__internal_entry_header)];
  char *err;
  mempool *mp =
      mempool_create_from_preallocated_buffer(buf, sizeof(buf), 64, false,
                                              false, NULL, &err);
  REQUIRE_EQ((void *)mp, NULL);
  REQUIRE_NE((void *)err, NULL);
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

TEST(r_mempools, realloc_first_exhaustion_entry_grows) {
  // Regression for Bug A: when a fallback_at_first_exhaustion dynamic entry
  // (elem_is_not_a_pool_member, pool_ptr->extended_elem_size != 0) is
  // reallocated to a larger pool, min_user_size must be bounded by the old
  // slot's user size, not the new requested size.  Before the fix, the copy
  // used new_size bytes from a smaller allocation — a heap over-read caught
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
  // are valid — reading 32 bytes would over-run the original heap allocation.
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
  // fallback_at_last_exhaustion pseudo_pool entries
  // (pool_ptr->extended_elem_size
  // == 0) and avoid reading beyond the original allocation.  Because the
  // original user size is not stored in the header, the copy is skipped
  // entirely (min_user_size = 0) to prevent a buffer over-read on grow.
  // Data is therefore NOT preserved across pseudo-pool reallocs; only the
  // allocation/free bookkeeping is verified here.
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

  // Request 32 bytes when all pools are exhausted → pseudo_pool entry
  // (pool_ptr == &rmp->pseudo_pool, extended_elem_size == 0).
  char *entry = r_mempool_alloc_entry(rmp, 32);
  REQUIRE_NE((void *)entry, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 1);

  // Shrink to 16 bytes.  The old entry is freed; a new one is returned.
  // No data copy is performed (original size unrecoverable from the header).
  char *shrunk = r_mempool_realloc_entry(rmp, entry, 16);
  REQUIRE_NE((void *)shrunk, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);

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
  // allocation.  Before the fix, min_user_size was set to the NEW (larger)
  // requested size, causing mem_cpy to read beyond the original heap block —
  // caught by Valgrind/ASan as a buffer over-read.  With min_user_size = 0,
  // no copy is performed; we only verify that the call succeeds and that
  // free-list bookkeeping stays consistent.
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

  // Grow to 32 bytes.  The original size (16) is unrecoverable; the copy is
  // skipped entirely.  A valid new 32-byte pseudo_pool entry is returned.
  // fallback_at_last_exhaustion uses a single shared pseudo_pool counter for
  // all sizes, so after the realloc the old 16-byte entry is freed and the
  // new 32-byte entry is live — total pseudo_pool count stays at 1.
  char *grown = r_mempool_realloc_entry(rmp, entry, 32);
  REQUIRE_NE((void *)grown, NULL);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 16), 1);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 1);

  r_mempool_free_entry(grown);
  REQUIRE_EQ(r_mempool_dynamic_allocs_count(rmp, 32), 0);

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
  // custom allocator — an allocator mismatch detectable by Valgrind.
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

TEST(r_mempools, realloc_returns_null_when_full_no_fallback_preserves_original) {
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
      NULL, sizeof(correct_buf), 4, 5, 4, fallback_disabled, false, NULL,
      &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  uint8_t small_buf[sizeof(correct_buf) - 1];
  rmp = r_mempool_create_from_preallocated_buffer(
      small_buf, sizeof(small_buf), 4, 5, 4, fallback_disabled, false, NULL,
      &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  uint8_t large_buf[sizeof(correct_buf) + 1];
  rmp = r_mempool_create_from_preallocated_buffer(
      large_buf, sizeof(large_buf), 4, 5, 4, fallback_disabled, false, NULL,
      &err);
  REQUIRE_EQ((void *)rmp, NULL);
  REQUIRE_NE((void *)err, NULL);

  // Invalid params: SS >= LS.
  rmp = r_mempool_create_from_preallocated_buffer(
      correct_buf, sizeof(correct_buf), 5, 4, 4, fallback_disabled, false,
      NULL, &err);
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
  // when the allocation comes from the heap fallback (fallback_at_first_exhaustion).
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
  // when the allocation comes from the pseudo_pool (fallback_at_last_exhaustion).
  r_mempool *rmp =
      r_mempool_create(4, 6, 7, fallback_at_last_exhaustion, false, NULL, NULL);
  REQUIRE_NE((void *)rmp, NULL);

  // Exhaust all three sub-pools.
  void *fill[224];
  size_t iter = 0;
  for (size_t i = 0; i < 128; ++i)
    fill[iter++] = r_mempool_alloc_entry(rmp, 16);
  for (size_t i = 0; i < 64; ++i)
    fill[iter++] = r_mempool_alloc_entry(rmp, 32);
  for (size_t i = 0; i < 32; ++i)
    fill[iter++] = r_mempool_alloc_entry(rmp, 64);
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

TEST(r_mempools, fallback_at_last_exhaustion_escalation_not_counted_as_fallback) {
  // SS=4, LS=6, SC=7: pool[0]=128x16-byte, pool[1]=64x32-byte.
  // Under fallback_at_last_exhaustion, escalation from pool[0] to pool[1]
  // must NOT increment the pseudo_pool counter — it is only a reallocation
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

  // pool[0] is full; this request escalates to pool[1] — no pseudo_pool.
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
