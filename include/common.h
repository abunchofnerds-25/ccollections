#pragma once

#include <stddef.h>

typedef enum ccollections_retval_t {
  ccol_unknown_failure = -8,
  ccol_container_empty,
  ccol_container_full,
  ccol_timed_out,
  ccol_not_permitted,
  ccol_invalid_args,
  ccol_key_not_found,
  ccol_not_enough_memory,
  ccol_success,
  ccol_success_threshold = ccol_success
} ccol_retval_t;

typedef struct ccol_memmgmt_procs_t {
  void* (*malloc)(size_t size);
  void (*free)(void* ptr);
  void* (*calloc)(size_t elem_count, size_t elem_size);
  void* (*realloc)(void* ptr, size_t size);
} ccol_memmgmt_procs_t;
